#!/usr/bin/env python3
"""LPC-AIO firmware-over-the-air server. One file, Python standard library only.

Units (port 80, through nginx, paths under /lpc/):
  GET /lpc/check?id=&p=&cc=&v=&c=&s=&t= -> "OTA NONE", "OTA FW <file bytes> <file crc> <image crc>" or
                                         (v6.9.63, cc = the CTRL's running image) "OTA CTRL <same three>"
  GET /lpc/fw?id=&c=<file crc>&o=<offset>&n=<bytes>
                                      -> "@<crc32 of the piece>\n" + the piece of the .hex file
Office (port 5002, login): tabs Stations (send an update, see each station's state), Firmware files
(upload), History, Password (change the admin password; kept hashed in the database, ota.env is the fallback).

A file is accepted only if it is a complete main-firmware .hex, or (v6.9.63) a CTRL ctrl.hex, whose image
header and CRC are right: the same checks the unit makes before it installs anything. A CTRL file is the
same for every station type; a station gets it only when it has no main-board update waiting.
"""
import contextlib
import hashlib
import hmac
import html
import json
import os
import re
import secrets
import sqlite3
import struct
import threading
import time
import zlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

DATA_DIR = os.environ.get("LPCOTA_DATA", "/opt/lpc-ota/data")
BUILDS_DIR = os.path.join(DATA_DIR, "builds")
DB_PATH = os.path.join(DATA_DIR, "ota.db")
PORT = int(os.environ.get("LPCOTA_PORT", "5002"))
BIND = os.environ.get("LPCOTA_BIND", "0.0.0.0")
ADMIN_USER = os.environ.get("LPCOTA_USER", "admin")
ADMIN_PASSWORD = os.environ.get("LPCOTA_PASSWORD", "")

SLOT_A_BASE = 0x18000
SLOT_SIZE = 0x20000
IMAGE_MAGIC = 0x31474D49
BOARD_ID = 0x1768
CTRL_SLOT_A_BASE = 0x2400       # v6.9.63: CTRL (LPC845) firmware, ctrl.hex
CTRL_SLOT_SIZE = 0x6000
CTRL_BOARD_ID = 0x845
CTRL_PROFILE = 9                # builds.profile of a CTRL file: for every station type
TYPES = {0: "KSNDMC TRG", 1: "BIHAR TRG", 2: "KSNDMC TWS"}
MAX_UPLOAD = 2 * 1024 * 1024
MAX_PIECE = 2048
UNIT_STATUS = {"0": "idle", "1": "downloading", "2": "file ready, installing", "3": "install failed, went back",
               "4": "file refused", "5": "no SD card"}

_db_lock = threading.Lock()


# ---------------------------------------------------------------- firmware file checks

def parse_firmware_hex(data):
    """Returns (profile, version, image_crc, image_len). Raises ValueError saying what is wrong."""
    mem = {}
    base = 0
    ended = False
    for n, raw in enumerate(data.splitlines(), 1):
        line = raw.strip()
        if not line:
            continue
        if ended:
            raise ValueError(f"line {n}: data after the end record")
        if line[:1] != b":":
            raise ValueError(f"line {n}: not an Intel hex line")
        try:
            rec = bytes.fromhex(line[1:].decode("ascii"))
        except (ValueError, UnicodeDecodeError):
            raise ValueError(f"line {n}: not hex digits")
        if len(rec) < 5 or len(rec) != rec[0] + 5 or sum(rec) & 0xFF:
            raise ValueError(f"line {n}: bad length or checksum")
        addr, typ, payload = (rec[1] << 8) | rec[2], rec[3], rec[4:-1]
        if typ == 0:
            for i, b in enumerate(payload):
                mem[base + addr + i] = b
        elif typ == 1:
            ended = True
        elif typ == 2:
            base = int.from_bytes(payload, "big") << 4
        elif typ == 4:
            base = int.from_bytes(payload, "big") << 16
        elif typ not in (3, 5):
            raise ValueError(f"line {n}: unexpected record type {typ}")
    if not ended:
        raise ValueError("the file stops early (no end record)")
    if not mem:
        raise ValueError("no data in the file")
    lo, hi = min(mem), max(mem)
    ctrl = lo == CTRL_SLOT_A_BASE and hi < CTRL_SLOT_A_BASE + CTRL_SLOT_SIZE
    if not ctrl and (lo != SLOT_A_BASE or hi >= SLOT_A_BASE + SLOT_SIZE):
        raise ValueError("not a firmware file (fw_ksn_trg.hex, fw_bihar.hex, fw_ksn_tws.hex or ctrl.hex): "
                         f"its data is at 0x{lo:05X}-0x{hi:05X}")
    base = CTRL_SLOT_A_BASE if ctrl else SLOT_A_BASE
    if len(mem) != hi - lo + 1:
        raise ValueError("lines missing in the file")
    img = bytearray(mem[a] for a in range(lo, hi + 1))
    off = struct.unpack_from("<I", img, 8 * 4)[0] - base
    if off < 0 or off + 36 > len(img):
        raise ValueError("no image header")
    magic, board, length, crc, profile = struct.unpack_from("<IIIII", img, off)
    if magic != IMAGE_MAGIC or board != (CTRL_BOARD_ID if ctrl else BOARD_ID):
        raise ValueError("no image header for the " + ("CTRL" if ctrl else "main board"))
    if length != len(img):
        raise ValueError(f"image length {length} in the header, {len(img)} in the file")
    struct.pack_into("<I", img, off + 12, 0)
    if zlib.crc32(bytes(img)) & 0xFFFFFFFF != crc:
        raise ValueError("image CRC is wrong: a damaged file")
    if ctrl:
        version = bytes(img[off + 20:off + 36]).split(b"\0")[0].decode("ascii", "replace")
        return CTRL_PROFILE, version, crc, len(img)
    if profile not in TYPES:
        raise ValueError(f"unknown station type {profile}")
    version = bytes(img[off + 20:off + 36]).split(b"\0")[0].decode("ascii", "replace")
    return profile, version, crc, len(img)


# ---------------------------------------------------------------- database

@contextlib.contextmanager
def db():
    """One connection per use: committed (or rolled back on an error) and always closed. sqlite3's own
    "with" only commits, and connections left open ran the service out of file handles."""
    con = sqlite3.connect(DB_PATH, timeout=10)
    con.row_factory = sqlite3.Row
    try:
        yield con
        con.commit()
    except BaseException:
        con.rollback()
        raise
    finally:
        con.close()


def init_db():
    os.makedirs(BUILDS_DIR, exist_ok=True)
    with db() as con:
        con.execute("PRAGMA journal_mode=WAL")
        con.executescript("""
        CREATE TABLE IF NOT EXISTS builds (
            id INTEGER PRIMARY KEY AUTOINCREMENT, profile INTEGER, version TEXT, image_crc TEXT,
            file_crc TEXT, size INTEGER, note TEXT, uploaded_at INTEGER);
        CREATE TABLE IF NOT EXISTS type_targets (profile INTEGER PRIMARY KEY, build_id INTEGER);
        CREATE TABLE IF NOT EXISTS stations (
            key TEXT PRIMARY KEY, shown_id TEXT, profile INTEGER, version TEXT, running_crc TEXT,
            unit_status TEXT, tried_crc TEXT, last_seen INTEGER, last_ip TEXT,
            target TEXT DEFAULT 'type', offered_crc TEXT, progress INTEGER DEFAULT 0, result TEXT DEFAULT '');
        CREATE TABLE IF NOT EXISTS events (id INTEGER PRIMARY KEY AUTOINCREMENT, at INTEGER, station TEXT, text TEXT);
        CREATE TABLE IF NOT EXISTS settings (key TEXT PRIMARY KEY, value TEXT);
        CREATE TABLE IF NOT EXISTS health (id INTEGER PRIMARY KEY AUTOINCREMENT, key TEXT, at INTEGER, data TEXT);
        CREATE INDEX IF NOT EXISTS health_key ON health (key, id);
        """)
        ecols = {r["name"] for r in con.execute("PRAGMA table_info(events)")}
        if "profile" not in ecols:
            # Office page v4: everything is kept apart by unit type. Old events get their type from the
            # station, or from the build or type name in the text; the rest (password) show for every type.
            con.execute("ALTER TABLE events ADD COLUMN profile INTEGER")
            con.execute("UPDATE events SET profile=(SELECT profile FROM stations WHERE stations.shown_id=events.station)"
                        " WHERE station != ''")
            for e in con.execute("SELECT id, text FROM events WHERE station='' OR station IS NULL").fetchall():
                p = next((k for k, t in TYPES.items() if t in e["text"]), None)
                m = re.match(r"build #(\d+)", e["text"])
                if p is None and m:
                    b = con.execute("SELECT profile FROM builds WHERE id=?", (int(m.group(1)),)).fetchone()
                    p = b["profile"] if b else None
                con.execute("UPDATE events SET profile=? WHERE id=?", (p, e["id"]))
        cols = {r["name"] for r in con.execute("PRAGMA table_info(stations)")}
        if "health" not in cols:
            con.execute("ALTER TABLE stations ADD COLUMN health TEXT DEFAULT ''")
        if "health_at" not in cols:
            con.execute("ALTER TABLE stations ADD COLUMN health_at INTEGER DEFAULT 0")
        # v6.9.63: CTRL by OTA. ctrl_target is a CTRL build id or '' (none); ctrl_crc what the unit says its
        # CTRL runs ('' from firmware before v6.9.63, which can't take a CTRL file)
        for col in ("ctrl_target", "ctrl_crc", "ctrl_offered", "ctrl_result"):
            if col not in cols:
                con.execute(f"ALTER TABLE stations ADD COLUMN {col} TEXT DEFAULT ''")


def add_event(con, station, text, profile=None):
    """profile: the unit type it belongs to; for a station, its own when not given. None: every type."""
    if profile is None and station:
        row = con.execute("SELECT profile FROM stations WHERE shown_id=?", (station,)).fetchone()
        profile = row["profile"] if row else None
    con.execute("INSERT INTO events (at, station, text, profile) VALUES (?,?,?,?)", (int(time.time()), station, text, profile))
    con.execute("DELETE FROM events WHERE id < (SELECT MAX(id) FROM events) - 2000")


def station_key(stn_id):
    return stn_id.lstrip("0") or "0"


def target_build(con, st):
    """The build this station should be running, or None."""
    if st["target"] == "hold":
        return None
    if st["target"] == "type":
        row = con.execute("SELECT build_id FROM type_targets WHERE profile=?", (st["profile"],)).fetchone()
        build_id = row["build_id"] if row else None
    else:
        build_id = int(st["target"])
    if build_id is None:
        return None
    return con.execute("SELECT * FROM builds WHERE id=?", (build_id,)).fetchone()


# ---------------------------------------------------------------- health report
# The daily report rides on the unit's check (/lpc/check), as more short fields after the update ones.
# One list defines each field: what the unit sends, how the office page names it, and when it needs
# a look. A unit without them (before v6.9.60) is unaffected. Planned unit side: docs/SYSTEM_DESIGN.md 7.7.

HEALTH_FIELDS = [   # (field, label, unit)
    ("hs", "Health status", ""),                    # "OK" or fault codes joined by ".", e.g. WDOG.CRASH
    ("day", "Day reported", ""),                    # v6.9.62: MET day 08:45 to 08:30, named by its close date YYYYMMDD
    ("sent", "Records sent that day", "of 96"),     # v6.9.62: delivered, in their own slot or later
    ("bk", "... of them by backlog", ""),           # v6.9.62: delivered later than their own slot
    ("bd", "Battery average, day (10:00-16:00)", "V"),
    ("bn", "Battery average, night (22:00-04:00)", "V"),
    ("bmin", "Battery lowest that day", "V"),       # up to v6.9.61: lowest in the last 24 h
    ("bv", "Battery now", "V"),
    ("ne", "Errors logged that day", ""),           # v6.9.62
    ("err", "Newest error that day", ""),           # v6.9.62
    ("sv", "Solar", "V"),
    ("sig", "Signal", "dBm"),
    ("net", "Network", ""),                         # 2 or 4 (G)
    ("op", "Operator", ""),
    ("q", "Records waiting now", ""),
    ("dur", "Average time on per slot", "s"),
    ("rst", "Fault restarts since last report", ""),
    ("wdt", "Watchdog restarts since last report", ""),
    ("sd", "SD card", ""),                          # 1 working, 0 fault
    ("ctrl", "CTRL firmware", ""),
    ("clk", "Clock corrected by", "s"),
    ("sens", "Sensors", ""),                        # TWS: e.g. TH-OK,WS-OK,WD-OK
    ("gps", "Location", ""),                        # lat,lon
    ("ok", "Slots sent on time, 24 h", "of 96"),    # v6.9.60-61 only (shown when sent)
    ("late", "Queued to send later, 24 h", ""),     # v6.9.60-61 only
    ("miss", "Slots missed or unit off, 24 h", ""), # v6.9.60-61 only
]
HEALTH_UNITS = {k: u for k, _, u in HEALTH_FIELDS}
HEALTH_VALUE = re.compile(r"[0-9A-Za-z._,:+ -]{0,60}")


def _f(v):
    return float(v)


HEALTH_RULES = [    # (field, needs a look if, class, text)
    # Watchdog and fault restarts have rules of their own: hs only for any other code
    ("hs", lambda v: bool(set(v.split(".")) - {"OK", "", "WDOG", "CRASH"}), "bad",
     lambda v: ", ".join(c for c in v.split(".") if c not in ("WDOG", "CRASH"))),
    ("sd", lambda v: v == "0", "bad", lambda v: "SD card fault"),
    ("wdt", lambda v: _f(v) > 0, "bad", lambda v: f"{v} watchdog restart(s)"),
    ("rst", lambda v: _f(v) > 0, "bad", lambda v: f"{v} fault restart(s)"),
    ("bv", lambda v: _f(v) < 11.0, "bad", lambda v: f"Battery {v} V"),
    ("bmin", lambda v: _f(v) < 11.0, "wait", lambda v: f"Battery fell to {v} V"),
    ("bn", lambda v: _f(v) < 11.5, "wait", lambda v: f"Night battery average {v} V"),
    ("sent", lambda v: _f(v) <= 92, "wait", lambda v: f"{96 - int(_f(v))} of 96 not sent"),
    ("ne", lambda v: _f(v) > 0, "wait", lambda v: f"{v} error(s) logged"),
    ("miss", lambda v: _f(v) >= 4, "wait", lambda v: f"{v} slots missed"),
    ("q", lambda v: _f(v) >= 4, "wait", lambda v: f"{v} records waiting"),
    ("sig", lambda v: _f(v) <= -105, "wait", lambda v: f"Weak signal {v} dBm"),
]


def store_health(con, key, q, now):
    h = {k: q[k] for k, _, _ in HEALTH_FIELDS if k in q and HEALTH_VALUE.fullmatch(q[k])}
    if not h:
        return
    data = json.dumps(h, separators=(",", ":"))
    con.execute("INSERT INTO health (key, at, data) VALUES (?,?,?)", (key, now, data))
    con.execute("UPDATE stations SET health=?, health_at=? WHERE key=?", (data, now, key))
    con.execute("DELETE FROM health WHERE key=? AND at < ?", (key, now - 400 * 86400))


# ---------------------------------------------------------------- unit requests

def unit_check(q, ip):
    stn = q.get("id", "")
    if not re.fullmatch(r"[0-9A-Za-z]{1,12}", stn):
        return "OTA NONE\n"
    try:
        profile = int(q.get("p", "-1"))
    except ValueError:
        profile = -1
    version = q.get("v", "")[:16]
    crc = q.get("c", "").upper()[:8]
    status = q.get("s", "0")[:1]
    tried = q.get("t", "").upper()[:8]
    cc = q.get("cc", "").upper()[:8]
    key = station_key(stn)
    now = int(time.time())
    with _db_lock, db() as con:
        st = con.execute("SELECT * FROM stations WHERE key=?", (key,)).fetchone()
        if st is None:
            con.execute("INSERT INTO stations (key, shown_id, profile, version, running_crc, unit_status, tried_crc,"
                        " last_seen, last_ip) VALUES (?,?,?,?,?,?,?,?,?)",
                        (key, stn, profile, version, crc, status, tried, now, ip))
            add_event(con, stn, f"first check-in: {TYPES.get(profile, '?')} {version} ({crc})", profile)
            st = con.execute("SELECT * FROM stations WHERE key=?", (key,)).fetchone()
        else:
            if st["running_crc"] != crc:
                add_event(con, stn, f"now running {version} ({crc}), was {st['version']} ({st['running_crc']})")
            if st["unit_status"] != status:
                add_event(con, stn, f"unit says: {UNIT_STATUS.get(status, status)}")
            con.execute("UPDATE stations SET shown_id=?, profile=?, version=?, running_crc=?, unit_status=?,"
                        " tried_crc=?, last_seen=?, last_ip=? WHERE key=?",
                        (stn, profile, version, crc, status, tried, now, ip, key))
            st = con.execute("SELECT * FROM stations WHERE key=?", (key,)).fetchone()
        store_health(con, key, q, now)
        build = target_build(con, st)
        reply = "OTA NONE\n"
        result = st["result"]
        offered = None
        if build is None:
            result = ""
        elif build["profile"] != profile:
            result = "target is for another station type: not offered"
        elif build["image_crc"] == crc:
            if st["offered_crc"] == crc and not result.startswith("updated"):
                add_event(con, stn, f"UPDATED to build {build['id']} {build['version']} ({crc})")
            result = "updated: running the target" if st["offered_crc"] == crc else "running the target"
        elif status == "3" and tried == build["image_crc"]:
            if not result.startswith("FAILED"):
                add_event(con, stn, f"FAILED: build {build['id']} did not run and the unit went back")
            result = "FAILED: the unit went back to its old firmware. Upload a new build to try again"
        else:
            reply = f"OTA FW {build['size']} {build['file_crc']} {build['image_crc']}\n"
            offered = build["image_crc"]
            if st["offered_crc"] != offered:
                add_event(con, stn, f"offered build {build['id']} {build['version']} ({offered})")
            result = "offered, waiting for the unit"
        if offered:
            con.execute("UPDATE stations SET offered_crc=?, result=? WHERE key=?", (offered, result, key))
        else:
            con.execute("UPDATE stations SET result=? WHERE key=?", (result, key))
        if offered is None:
            reply = ctrl_check(con, st, stn, cc, status, tried) or reply
    return reply


def ctrl_check(con, st, stn, cc, status, tried):
    """v6.9.63: the CTRL file this station should get, as the reply, or None. Only once no main-board
    update is waiting, and only to a unit that says what its CTRL runs (v6.9.63 on)."""
    con.execute("UPDATE stations SET ctrl_crc=? WHERE key=?", (cc, st["key"]))
    if not (st["ctrl_target"] or "").isdigit():
        return None
    b = con.execute("SELECT * FROM builds WHERE id=? AND profile=?", (int(st["ctrl_target"]), CTRL_PROFILE)).fetchone()
    if b is None:
        return None
    result, offered, reply = st["ctrl_result"] or "", None, None
    if cc == b["image_crc"]:
        if st["ctrl_offered"] == cc and not result.startswith("updated"):
            add_event(con, stn, f"UPDATED CTRL to {b['version']} ({cc})")
        result = "updated: running the target" if st["ctrl_offered"] == cc else "running the target"
    elif status == "3" and tried == b["image_crc"]:
        if not result.startswith("FAILED"):
            add_event(con, stn, f"FAILED: CTRL {b['version']} did not run and the CTRL went back")
        result = "FAILED: the CTRL went back to its old firmware. Upload a new CTRL file to try again"
    elif not re.fullmatch(r"[0-9A-F]{8}", cc) or cc == "00000000":
        result = "waiting: the unit needs main firmware v6.9.63 or later first"
    else:
        reply = f"OTA CTRL {b['size']} {b['file_crc']} {b['image_crc']}\n"
        offered = b["image_crc"]
        if st["ctrl_offered"] != offered:
            add_event(con, stn, f"offered CTRL {b['version']} ({offered})")
        result = "offered, waiting for the unit"
    con.execute("UPDATE stations SET ctrl_result=?" + (", ctrl_offered=?" if offered else "") + " WHERE key=?",
                (result, offered, st["key"]) if offered else (result, st["key"]))
    return reply


def unit_piece(q):
    """Returns (status, body)."""
    stn = q.get("id", "")
    file_crc = q.get("c", "").upper()
    try:
        off, n = int(q.get("o", "")), int(q.get("n", ""))
    except ValueError:
        return 400, b"OTA BAD\n"
    if not re.fullmatch(r"[0-9A-F]{8}", file_crc) or off < 0 or n < 1 or n > MAX_PIECE:
        return 400, b"OTA BAD\n"
    with db() as con:
        build = con.execute("SELECT * FROM builds WHERE file_crc=?", (file_crc,)).fetchone()
    if build is None:
        return 404, b"OTA GONE\n"
    try:
        with open(os.path.join(BUILDS_DIR, f"{build['id']}.hex"), "rb") as f:
            f.seek(off)
            piece = f.read(n)
    except OSError:
        return 404, b"OTA GONE\n"
    if not piece:
        return 416, b"OTA END\n"
    with _db_lock, db() as con:
        col = "ctrl_result" if build["profile"] == CTRL_PROFILE else "result"
        con.execute(f"UPDATE stations SET progress=?, {col}=? WHERE key=?",
                    (off + len(piece), f"downloading: {100 * (off + len(piece)) // build['size']}%", station_key(stn)))
    return 200, b"@%08X\n" % (zlib.crc32(piece) & 0xFFFFFFFF) + piece


# ---------------------------------------------------------------- office login and password

SESSIONS = {}
FAILED_LOGINS = {}
PBKDF2_ROUNDS = 200_000


def get_setting(key):
    with db() as con:
        row = con.execute("SELECT value FROM settings WHERE key=?", (key,)).fetchone()
    return row["value"] if row else None


def hash_password(pw):
    salt = secrets.token_hex(16)
    digest = hashlib.pbkdf2_hmac("sha256", pw.encode(), bytes.fromhex(salt), PBKDF2_ROUNDS).hex()
    return f"pbkdf2${PBKDF2_ROUNDS}${salt}${digest}"


def password_ok(user, pw):
    """The password set on the Password tab, or the one in ota.env while none has been set there."""
    if not hmac.compare_digest(user.encode(), ADMIN_USER.encode()):
        return False
    stored = get_setting("admin_pw")
    if stored:
        try:
            _, rounds, salt, digest = stored.split("$")
            test = hashlib.pbkdf2_hmac("sha256", pw.encode(), bytes.fromhex(salt), int(rounds)).hex()
        except ValueError:
            return False
        return hmac.compare_digest(test, digest)
    return bool(ADMIN_PASSWORD) and hmac.compare_digest(
        hashlib.sha256(pw.encode()).digest(), hashlib.sha256(ADMIN_PASSWORD.encode()).digest())


# ---------------------------------------------------------------- office pages

PAGE_CSS = """
:root{--bg:#f4f6f8;--card:#fff;--ink:#17202b;--mute:#667085;--line:#e2e6ea;--acc:#1f6feb;--acc-ink:#fff;
--ok:#16794c;--ok-bg:#e6f4ec;--bad:#b42318;--bad-bg:#fdecea;--wait:#9a5b00;--wait-bg:#fff4e0;--busy:#1f5fc4;--busy-bg:#e8f0fd;--off-bg:#eef0f3}
@media (prefers-color-scheme: dark){:root{--bg:#10141a;--card:#191f27;--ink:#e6e9ee;--mute:#98a2b3;--line:#2a323d;--acc:#5b9cff;--acc-ink:#0b1220;
--ok:#4ac48a;--ok-bg:#12301f;--bad:#f97066;--bad-bg:#3a1714;--wait:#f5b544;--wait-bg:#33260c;--busy:#7fb0ff;--busy-bg:#14284a;--off-bg:#232a34}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--ink);font:15px/1.5 system-ui,-apple-system,Segoe UI,Roboto,sans-serif}
header{background:var(--card);border-bottom:1px solid var(--line)}
.top{max-width:1000px;margin:0 auto;padding:12px 16px 0;display:flex;justify-content:space-between;align-items:center;gap:12px}
.top b{font-size:17px}.top a{color:var(--mute);text-decoration:none;font-size:14px}
nav{max-width:1000px;margin:0 auto;padding:0 16px;display:flex;gap:4px;overflow-x:auto}
nav a{padding:10px 14px;color:var(--mute);text-decoration:none;border-bottom:3px solid transparent;white-space:nowrap;font-weight:500}
nav a.on{color:var(--ink);border-bottom-color:var(--acc)}
main{max-width:1000px;margin:0 auto;padding:16px}
.card{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:18px;margin-bottom:16px}
h2{margin:0 0 4px;font-size:17px}.hint{margin:0 0 14px;color:var(--mute);font-size:14px}
.msg{padding:12px 16px;border-radius:10px;margin-bottom:16px;background:var(--busy-bg);color:var(--ink);border:1px solid var(--line)}
.msg.bad{background:var(--bad-bg)}
input,select,button{font:inherit;padding:8px 11px;border:1px solid var(--line);border-radius:8px;background:var(--card);color:var(--ink)}
button{background:var(--acc);color:var(--acc-ink);border-color:var(--acc);cursor:pointer;font-weight:600}
button.plain{background:transparent;color:var(--ink);border-color:var(--line);font-weight:500}
button.del{background:transparent;color:var(--bad);border-color:var(--line);font-weight:500}
button.small{padding:4px 10px;font-size:13px}
label.f{display:block;font-weight:600;margin:14px 0 6px}label.f:first-child{margin-top:0}
.stack{display:flex;flex-direction:column;gap:12px}
.list{display:flex;flex-direction:column;gap:10px}
.st{display:grid;grid-template-columns:1fr auto;gap:6px 16px;align-items:center;padding:14px;border:1px solid var(--line);border-radius:10px}
.st .id{font-size:17px;font-weight:700}.st .sub{color:var(--mute);font-size:13px}
.st .right{text-align:right}
.pill{display:inline-block;padding:3px 11px;border-radius:999px;font-size:13px;font-weight:600;white-space:nowrap}
.pill.ok{background:var(--ok-bg);color:var(--ok)}.pill.bad{background:var(--bad-bg);color:var(--bad)}
.pill.wait{background:var(--wait-bg);color:var(--wait)}.pill.busy{background:var(--busy-bg);color:var(--busy)}
.pill.off{background:var(--off-bg);color:var(--mute)}
.bar{height:6px;background:var(--off-bg);border-radius:3px;overflow:hidden;margin-top:6px;min-width:140px}
.bar i{display:block;height:100%;background:var(--busy)}
.ticks{display:flex;flex-wrap:wrap;gap:8px}
.tick{display:flex;align-items:center;gap:8px;padding:8px 12px;border:1px solid var(--line);border-radius:8px;cursor:pointer}
.tick input{margin:0;width:17px;height:17px}
.tick.hide{display:none}
.scroll{overflow-x:auto}table{border-collapse:collapse;width:100%;font-size:14px}
th,td{text-align:left;padding:9px 10px;border-bottom:1px solid var(--line);vertical-align:top}
th{color:var(--mute);font-weight:600;font-size:12px;text-transform:uppercase;letter-spacing:.03em;white-space:nowrap}
td.nw{white-space:nowrap}code,.crc{font:12px ui-monospace,Menlo,Consolas,monospace;color:var(--mute)}
a{color:var(--acc)}.mute{color:var(--mute)}.okt{color:var(--ok)}.badt{color:var(--bad)}
.steps{margin:0 0 14px;padding-left:20px;color:var(--mute);font-size:14px}
details summary{cursor:pointer;color:var(--acc);font-size:14px}
.row{display:flex;gap:8px;flex-wrap:wrap;align-items:center}
.login{max-width:360px;margin:12vh auto}.login input{width:100%;margin-bottom:10px}.login button{width:100%}
.pw input{width:100%;max-width:340px}
@media (max-width:600px){.st{grid-template-columns:1fr}.st .right{text-align:left}}
.seg{display:flex;gap:6px;flex-wrap:wrap;margin:0 0 12px}
.seg a{padding:8px 14px;border:1px solid var(--line);border-radius:999px;background:var(--card);color:var(--ink);text-decoration:none;font-weight:600;font-size:14px}
.seg a span{color:var(--mute);font-weight:500;margin-left:4px}.seg a.on{background:var(--acc);border-color:var(--acc);color:var(--acc-ink)}
.seg a.on span{color:var(--acc-ink);opacity:.8}
.seg.types{margin:0 0 16px}.seg.types a{font-size:15px}.seg a.on .badt{color:var(--acc-ink)}
table.list td{vertical-align:middle}
.dot{display:inline-block;width:9px;height:9px;border-radius:50%;margin-right:8px;background:var(--ok)}
.dot.bad{background:var(--bad)}.dot.wait{background:var(--wait)}
.kv{display:grid;grid-template-columns:repeat(auto-fill,minmax(210px,1fr));gap:12px 18px}
.kv>div{display:flex;flex-direction:column;gap:2px}.kv>div>span:first-child{color:var(--mute);font-size:12px;text-transform:uppercase;letter-spacing:.03em}
td .pill{margin:1px 2px}
select,input{max-width:100%}.card{min-width:0}.tick{max-width:100%}.tick span{min-width:0;overflow-wrap:anywhere}
"""

TABS = [("stations", "Stations"), ("firmware", "Firmware files"), ("history", "History"), ("password", "Password")]


def esc(v):
    return html.escape(str(v if v is not None else ""))


def when(ts):
    if not ts:
        return "never"
    age = int(time.time()) - ts
    text = time.strftime("%d %b %H:%M", time.localtime(ts))
    if age < 60:
        return f"{text} (just now)"
    if age < 3600:
        return f"{text} ({age // 60} min ago)"
    if age < 172800:
        return f"{text} ({age // 3600} h ago)"
    return f"{text} ({age // 86400} days ago)"


TYPED_TABS = ("stations", "firmware", "history")


def type_bar(tab, ptype):
    """The unit type switch: Stations, Firmware files and History each show one type at a time.
    Each type shows its station count and how many of them need a look, so nothing is missed."""
    with db() as con:
        stations = con.execute("SELECT * FROM stations").fetchall()
    counts = type_counts(stations)
    flagged = {p: 0 for p in TYPES}
    for st in stations:
        if st["profile"] in flagged and attention(st, health_of(st)):
            flagged[st["profile"]] += 1
    items = []
    for p, t in TYPES.items():
        warn = f" <b class='badt'>· {flagged[p]} need a look</b>" if flagged[p] else ""
        items.append(f"<a href='/?tab={tab}&type={p}' class='{'on' if p == ptype else ''}'>{esc(t)} "
                     f"<span>{counts[p]}</span>{warn}</a>")
    return "<div class='seg types'>" + "".join(items) + "</div>"


def page(title, body, tab=None, message="", script="", ptype=None):
    head = (f"<!doctype html><html lang='en'><head><meta charset='utf-8'>"
            f"<meta name='viewport' content='width=device-width,initial-scale=1'><title>{esc(title)}</title>"
            f"<style>{PAGE_CSS}</style></head><body>")
    if tab is None:
        return (head + f"<main>{body}</main></body></html>").encode()
    tq = f"&type={ptype}" if ptype in TYPES else ""
    nav = "".join(f"<a href='/?tab={k}{tq if k in TYPED_TABS else ''}' class='{'on' if k == tab else ''}'>{t}</a>"
                  for k, t in TABS)
    bar = type_bar(tab, ptype) if tab in TYPED_TABS and ptype in TYPES else ""
    msg = ""
    if message:
        cls = "msg bad" if message.lower().startswith(("file refused", "wrong", "not ", "no ", "the new", "build #")) and "deleted" not in message else "msg"
        msg = f"<div class='{cls}'>{esc(message)}</div>"
    return (head + f"<header><div class='top'><b>LPC-AIO stations &amp; firmware</b><a href='/logout'>Log out</a></div>"
            f"<nav>{nav}</nav></header><main>{bar}{msg}{body}</main>{script}</body></html>").encode()


def build_name(b):
    return f"{b['version']} · {TYPES.get(b['profile'], 'CTRL, every type')} (#{b['id']})"


def ctrl_state(s, b):
    """v6.9.63: (pill class, text) for the station's CTRL update, or None when none is set."""
    if b is None:
        return None
    res = s["ctrl_result"] or ""
    if res.startswith("FAILED"):
        return "bad", "CTRL update failed"
    if res.startswith("updated"):
        return "ok", f"CTRL updated to {b['version']}"
    if res == "running the target":
        return "ok", "CTRL up to date"
    if res.startswith("downloading"):
        return "busy", f"CTRL {res}"
    if res.startswith("waiting: the unit needs"):
        return "wait", "CTRL: needs v6.9.63 first"
    if res.startswith("offered"):
        return "busy", "CTRL: starting download"
    return "wait", f"CTRL {b['version']} at next check"


def ctrl_build(con, s):
    t = s["ctrl_target"] or ""
    return con.execute("SELECT * FROM builds WHERE id=? AND profile=?", (int(t), CTRL_PROFILE)).fetchone() if t.isdigit() else None


def station_state(s, build):
    """(pill class, pill text, detail line, download percent or None) in plain words."""
    res = s["result"] or ""
    us = s["unit_status"]
    if res.startswith("FAILED"):
        return "bad", "Update failed", "The new firmware did not run, so the unit went back to its old one. Upload a new build to try again.", None
    if res.startswith("target is for another"):
        return "bad", "Wrong type", "The chosen firmware is for a different station type. Choose another.", None
    if us == "4":
        return "bad", "File refused", "The unit refused the downloaded file. It will try again at its next check.", None
    if us == "5":
        return "bad", "No SD card", "The unit has no working SD card, so it can't update.", None
    if s["target"] == "hold":
        return "off", "Updates stopped", "This station won't update until you resume it.", None
    if build is None:
        return "off", "No update set", "", None
    if res.startswith("updated"):
        return "ok", f"Updated to {build['version']}", "", None
    if res == "running the target":
        return "ok", "Up to date", "", None
    if res.startswith("downloading"):
        pct = s["progress"] * 100 // build["size"] if build["size"] else 0
        pct = max(0, min(100, pct))
        if pct >= 100:
            return "busy", "Downloaded", "Installs when this slot ends, then reports the new version at its next check.", 100
        return "busy", f"Downloading {pct}%", "", pct
    if us == "2":
        return "busy", "Installing", "", None
    if res.startswith("offered"):
        return "busy", "Starting download", "", None
    return "wait", "Waiting for next check", f"It will get {build['version']} at its next check.", None


STALE_AFTER_S = 26 * 3600      # A unit checks once a day: no check for longer than this needs a look
TYPE_SLUG = {0: "ksn-trg", 1: "bihar-trg", 2: "ksn-tws"}


def health_of(s):
    try:
        return json.loads(s["health"] or "{}")
    except (ValueError, TypeError, IndexError, KeyError):
        return {}


def attention(s, h):
    """What needs a look at this station: [(class, text)], worst first. Uses HEALTH_RULES for the report."""
    out = []
    now = int(time.time())
    if not s["last_seen"]:
        out.append(("wait", "Never checked in"))
    elif now - s["last_seen"] > STALE_AFTER_S:
        out.append(("bad", f"No check-in for {(now - s['last_seen']) // 3600} h"))
    res = s["result"] or ""
    if res.startswith("FAILED"):
        out.append(("bad", "Update failed"))
    if (s["ctrl_result"] or "").startswith("FAILED"):
        out.append(("bad", "CTRL update failed"))
    elif res.startswith("target is for another"):
        out.append(("bad", "Wrong firmware type set"))
    if s["unit_status"] in ("4", "5"):
        out.append(("bad", UNIT_STATUS[s["unit_status"]].capitalize()))
    if h and s["health_at"] and now - s["health_at"] <= STALE_AFTER_S + 3600:
        for key, test, cls, text in HEALTH_RULES:
            v = h.get(key)
            if v is None or v == "":
                continue
            try:
                hit = test(v)
            except ValueError:
                hit = False
            if hit:
                out.append((cls, text(v)))
    out.sort(key=lambda a: a[0] != "bad")
    return out


def fmt_health(key, v):
    if v is None or v == "":
        return "<span class='mute'>—</span>"
    unit = HEALTH_UNITS.get(key, "")
    if key == "sd":
        return "<span class='okt'>OK</span>" if v == "1" else "<span class='badt'>Fault</span>"
    if key == "hs":
        return "<span class='okt'>OK</span>" if v == "OK" else f"<span class='badt'>{esc(v.replace('.', ', '))}</span>"
    if key == "net":
        return esc(f"{v}G") if v in ("2", "4") else esc(v)
    if key == "day" and re.fullmatch(r"\d{8}", v):
        try:
            t = time.mktime(time.strptime(v, "%Y%m%d"))
        except ValueError:
            return esc(v)
        return esc(time.strftime("%d %b", time.localtime(t - 86400)) + " 08:45 to " + time.strftime("%d %b", time.localtime(t)) + " 08:30")
    if key == "err":
        return f"<span class='badt'>{esc(v)}</span>"
    if key == "gps" and "," in v:
        lat, _, lon = v.partition(",")
        return f"<a href='https://maps.google.com/?q={esc(lat)},{esc(lon)}' target='_blank' rel='noopener'>{esc(v)}</a>"
    return esc(v) + (f" <span class='mute'>{esc(unit)}</span>" if unit else "")


def _v(h, k, unit=""):
    v = h.get(k)
    if v in (None, ""):
        return "<span class='mute'>—</span>"
    return esc(v) + (f" <span class='mute'>{unit}</span>" if unit else "")


def fw_text(s, h):
    """Main firmware with the CTRL firmware next to it."""
    ctrl = h.get("ctrl") or ""
    return (f"<b>{esc(s['version']) or '—'}</b> <span class='mute'>· CTRL</span> "
            + (f"<b>{esc(ctrl)}</b>" if ctrl else "<span class='mute'>—</span>"))


def sent_text(h):
    """'92/96 (3 backlog)' from a v6.9.62 report; '—' from an older one."""
    if h.get("sent") in (None, ""):
        return "<span class='mute'>—</span>"
    bk = h.get("bk") or "0"
    return f"<b>{esc(h['sent'])}</b><span class='mute'>/96</span> <span class='mute'>({esc(bk)} backlog)</span>"


def summary_line(h):
    """The short line under a station in Needs attention: not sent, day/night battery, carrier."""
    bits = []
    if h.get("sent") not in (None, ""):
        try:
            int(h["sent"])
            bits.append(f"Sent {esc(h['sent'])}/96 ({esc(h.get('bk') or '0')} by backlog)")
        except ValueError:
            pass
    if h.get("bd") or h.get("bn"):
        bits.append(f"Battery day {esc(h.get('bd') or '—')} V / night {esc(h.get('bn') or '—')} V")
    if h.get("op"):
        bits.append(esc(h["op"]) + (f" {esc(h['net'])}G" if h.get("net") in ("2", "4") else ""))
    return " · ".join(bits)


def status_text(h):
    """One line: OK, or what went wrong (restarts and SD card folded in)."""
    bad = []
    try:
        if int(h.get("wdt") or 0):
            bad.append(f"{h['wdt']} watchdog restart(s)")
        if int(h.get("rst") or 0):
            bad.append(f"{h['rst']} fault restart(s)")
    except ValueError:
        pass
    if h.get("sd") == "0":
        bad.append("SD card fault")
    if not bad and h.get("hs") not in (None, "", "OK"):
        bad.append(h["hs"].replace(".", ", "))
    if not h:
        return "<span class='mute'>—</span>"
    return f"<span class='badt'>{esc(', '.join(bad))}</span>" if bad else "<span class='okt'>OK</span>"


def health_rows(s, h):
    """The Health card: (label, html), short and in the order asked for (user, 18 Nov)."""
    try:
        not_sent = str(96 - int(h["sent"])) if h.get("sent") not in (None, "") else ""
    except ValueError:
        not_sent = ""
    errs = _v(h, "ne")
    if h.get("err"):
        errs += f" <span class='mute'>· newest:</span> <span class='badt'>{esc(h['err'])}</span>"
    net = " · ".join(x for x in (_v(h, "sig", "dBm") if h.get("sig") else "",
                                 esc(h["net"] + "G") if h.get("net") in ("2", "4") else "",
                                 esc(h.get("op") or "")) if x) or "<span class='mute'>—</span>"
    rows = [
        ("Status", status_text(h)),
        ("Day reported", fmt_health("day", h.get("day"))),
        ("Slots sent", sent_text(h)),
        ("Slots not sent", esc(not_sent) if not_sent else "<span class='mute'>—</span>"),
        ("Battery, day average (10:00-16:00)", _v(h, "bd", "V")),
        ("Battery, night average (22:00-04:00)", _v(h, "bn", "V")),
        ("Battery, lowest that day", _v(h, "bmin", "V") if h.get("sent") not in (None, "") else "<span class='mute'>—</span>"),
        ("Battery now", _v(h, "bv", "V")),
        ("Errors that day", errs),
        ("Network", net),
        ("Records waiting now", _v(h, "q")),
        ("Time on per slot", _v(h, "dur", "s")),
        ("Clock corrected by", _v(h, "clk", "s")),
    ]
    if h.get("sv"):
        rows.append(("Solar", _v(h, "sv", "V")))
    if h.get("sens"):
        rows.append(("Sensors", _v(h, "sens")))
    if h.get("gps"):
        rows.append(("Location", fmt_health("gps", h["gps"])))
    return rows


HISTORY_COLS = [
    ("Day", lambda d: fmt_health("day", d.get("day"))),
    ("Sent", sent_text),
    ("Day avg", lambda d: _v(d, "bd", "V")),
    ("Night avg", lambda d: _v(d, "bn", "V")),
    ("Lowest", lambda d: _v(d, "bmin", "V") if d.get("sent") not in (None, "") else "<span class='mute'>—</span>"),
    ("Errors", lambda d: _v(d, "ne")),
    ("Carrier", lambda d: _v(d, "op")),
    ("Status", status_text),
]


def type_counts(stations):
    counts = {p: 0 for p in TYPES}
    for s in stations:
        if s["profile"] in counts:
            counts[s["profile"]] += 1
    return counts


def pick_type(ptype):
    """The type asked for, or the one with the most stations."""
    if ptype in TYPES:
        return ptype
    with db() as con:
        counts = type_counts(con.execute("SELECT profile FROM stations").fetchall())
    return max(TYPES, key=lambda p: (counts[p], -p))


def stations_tab(ptype, search):
    with db() as con:
        builds = con.execute("SELECT * FROM builds ORDER BY id DESC").fetchall()
        targets = {r["profile"]: r["build_id"] for r in con.execute("SELECT * FROM type_targets")}
        stations = con.execute("SELECT * FROM stations ORDER BY CAST(key AS INTEGER), key").fetchall()
        st_builds = {s["key"]: target_build(con, s) for s in stations}
        st_ctrl = {s["key"]: ctrl_state(s, ctrl_build(con, s)) for s in stations}
    by_id = {b["id"]: b for b in builds}
    h = []

    # Needs attention: this type's stations (the type switch above counts the others)
    flagged = []
    for s in stations:
        if s["profile"] != ptype:
            continue
        a = attention(s, health_of(s))
        if a:
            flagged.append((s, a))
    h.append(f"<div class='card'><h2>{esc(TYPES[ptype])}: needs attention</h2>")
    if flagged:
        h.append("<div class='scroll'><table><tr><th>Station</th><th>What</th></tr>")
        for s, a in flagged:
            pills = " ".join(f"<span class='pill {c}'>{esc(t)}</span>" for c, t in a)
            line = summary_line(health_of(s))
            h.append(f"<tr><td class='nw'><a href='/s?k={esc(s['key'])}'><b>{esc(s['shown_id'])}</b></a></td>"
                     f"<td>{pills}{'<div class=mute style=margin-top:4px>' + line + '</div>' if line else ''}</td></tr>")
        h.append("</table></div>")
    else:
        h.append(f"<p class='hint' style='margin:0'><span class='okt'>Nothing.</span> Every {esc(TYPES[ptype])} station "
                 "checked in within 26 hours and reports no problem.</p>")
    h.append("</div>")

    # The type's stations
    rows = [s for s in stations if s["profile"] == ptype]
    if search:
        rows = [s for s in rows if search.lstrip("0") in s["shown_id"].lstrip("0")]
    h.append(f"<div class='card'><div class='row' style='justify-content:space-between;margin-bottom:10px'>"
             f"<h2 style='margin:0'>{esc(TYPES[ptype])} stations</h2>"
             f"<form method='get' action='/' class='row'><input type='hidden' name='tab' value='stations'>"
             f"<input type='hidden' name='type' value='{ptype}'><input name='q' value='{esc(search)}' "
             f"placeholder='Find a station ID' size='16'><button class='plain'>Find</button></form></div>"
             "<div class='scroll'><table class='list'><tr><th>Station</th><th>Firmware</th><th>Sent (last day)</th>"
             "<th>Battery day / night</th><th>Carrier</th><th>Last check</th><th>Update</th></tr>")
    for s in rows:
        hl = health_of(s)
        cls, label, _, pct = station_state(s, st_builds[s["key"]])
        a = attention(s, hl)
        dot = f"<span class='dot {a[0][0]}' title='{esc(', '.join(t for _, t in a))}'></span>" if a else "<span class='dot'></span>"
        bar = f"<div class='bar'><i style='width:{pct}%'></i></div>" if pct is not None else ""
        if hl.get("bd") or hl.get("bn"):
            bat = f"{esc(hl.get('bd') or '—')} / {esc(hl.get('bn') or '—')} <span class='mute'>V</span>"
        else:
            bat = "<span class='mute'>—</span>"
        carrier = esc(hl.get("op") or "") + (f" <span class='mute'>{esc(hl['net'])}G</span>" if hl.get("net") in ("2", "4") else "")
        h.append(f"<tr><td class='nw'>{dot}<a href='/s?k={esc(s['key'])}'><b>{esc(s['shown_id'])}</b></a></td>"
                 f"<td class='nw'>{fw_text(s, hl)}</td><td class='nw'>{sent_text(hl)}</td>"
                 f"<td class='nw'>{bat}</td><td class='nw'>{carrier or '<span class=mute>—</span>'}</td>"
                 f"<td class='nw'>{esc(when(s['last_seen']))}</td>"
                 f"<td class='nw'><span class='pill {cls}'>{esc(label)}</span>{bar}"
                 + (f"<div style='margin-top:4px'><span class='pill {st_ctrl[s['key']][0]}'>{esc(st_ctrl[s['key']][1])}</span></div>"
                    if st_ctrl[s["key"]] else "") + "</td></tr>")
    if not rows:
        h.append(f"<tr><td colspan='7' class='mute'>{'No station matches.' if search else 'No station of this type has checked in yet.'}</td></tr>")
    h.append("</table></div>")
    if not any(health_of(s) for s in rows) and rows:
        h.append("<p class='hint' style='margin:10px 0 0'>Battery, signal and slot counts come in the daily health "
                 "report (firmware v6.9.60 on; the last whole day from v6.9.62).</p>")
    h.append("</div>")

    # Send an update, for this type only
    tb = [b for b in builds if b["profile"] in (ptype, CTRL_PROFILE)]
    h.append(f"<div class='card'><h2>Send an update to {esc(TYPES[ptype])} stations</h2>")
    if not tb:
        h.append(f"<p class='hint' style='margin:0'>No {esc(TYPES[ptype])} firmware uploaded yet: see "
                 "<a href='/?tab=firmware'>Firmware files</a>.</p></div>")
    else:
        opts = "".join(f"<option value='{b['id']}'>{esc(build_name(b))}{' · ' + esc(b['note']) if b['note'] else ''}</option>"
                       for b in tb)
        ticks = "".join(f"<label class='tick'><input type='checkbox' name='stn' value='{esc(s['key'])}'>"
                        f"<span><b>{esc(s['shown_id'])}</b> <span class='mute'>{esc(s['version'] or 'not seen yet')}</span></span></label>"
                        for s in stations if s["profile"] == ptype)
        h.append("<p class='hint'>Choose the firmware, tick the stations, press Send. Try one station first. "
                 "A CTRL file goes to the CTRL board of the ticked stations, after any main-board update; "
                 "they need main firmware v6.9.63 or later.</p>"
                 f"<form method='post' action='/send' class='stack'><input type='hidden' name='back' value='/?tab=stations&type={ptype}'>"
                 f"<div><label class='f' for='bsel'>1. Firmware</label><select id='bsel' name='build' required>{opts}</select></div>"
                 f"<div><label class='f'>2. Stations</label><div class='ticks'>{ticks or '<span class=mute>None yet.</span>'}</div></div>"
                 "<label class='tick' style='align-self:flex-start'><input type='checkbox' name='all' value='1'>"
                 f"<span>Every {esc(TYPES[ptype])} station, including ones that check in later</span></label>"
                 "<div><button>Send update</button></div></form>")
        wb = by_id.get(targets.get(ptype))
        if wb:
            h.append(f"<form method='post' action='/type' class='row' style='margin-top:14px'><input type='hidden' name='profile' value='{ptype}'>"
                     f"<input type='hidden' name='build' value=''><input type='hidden' name='back' value='/?tab=stations&type={ptype}'>"
                     f"<span>Whole type: every {esc(TYPES[ptype])} station gets <b>{esc(wb['version'])}</b> (#{wb['id']})</span>"
                     "<button class='del small'>Stop for the type</button></form>")
        h.append("</div>")
    h.append("<details><summary>Add a station that hasn't checked in yet</summary>"
             "<form method='post' action='/station/add' class='row' style='margin-top:10px'>"
             f"<input type='hidden' name='back' value='/?tab=stations&type={ptype}'>"
             "<input name='id' placeholder='Station ID, e.g. 001922' size='18' required pattern='[0-9A-Za-z]{1,12}'>"
             "<select name='profile'>" + "".join(f"<option value='{p}'{' selected' if p == ptype else ''}>{t}</option>"
                                                 for p, t in TYPES.items()) +
             "</select><button class='plain'>Add</button></form></details>")

    script = f"""<script>
setInterval(function(){{
  var busy=document.querySelector('input[type=checkbox]:checked')||
    (document.activeElement&&/INPUT|SELECT|TEXTAREA/.test(document.activeElement.tagName));
  if(!busy) location.replace('/?tab=stations&type={ptype}' + {json.dumps('&q=' + search) if search else "''"});
}},30000);
</script>"""
    return "".join(h), script


def station_page(key, message=""):
    with db() as con:
        s = con.execute("SELECT * FROM stations WHERE key=?", (key,)).fetchone()
        if s is None:
            return None
        build = target_build(con, s)
        builds = con.execute("SELECT * FROM builds WHERE profile=? ORDER BY id DESC", (s["profile"],)).fetchall()
        cbuild = ctrl_build(con, s)
        cbuilds = con.execute("SELECT * FROM builds WHERE profile=? ORDER BY id DESC", (CTRL_PROFILE,)).fetchall()
        reports = con.execute("SELECT * FROM health WHERE key=? ORDER BY id DESC LIMIT 60", (key,)).fetchall()
        events = con.execute("SELECT * FROM events WHERE station=? ORDER BY id DESC LIMIT 100", (s["shown_id"],)).fetchall()
    hl = health_of(s)
    cls, label, detail, pct = station_state(s, build)
    a = attention(s, hl)
    back = f"/?tab=stations&type={s['profile']}"
    h = [f"<p style='margin:0 0 12px'><a href='{back}'>← {esc(TYPES.get(s['profile'], '?'))} stations</a></p>"]

    # Summary
    pills = " ".join(f"<span class='pill {c}'>{esc(t)}</span>" for c, t in a) or "<span class='pill ok'>No problem reported</span>"
    h.append(f"<div class='card'><div class='row' style='justify-content:space-between'><h2 style='margin:0;font-size:22px'>"
             f"{esc(s['shown_id'])} <span class='mute' style='font-size:15px;font-weight:400'>{esc(TYPES.get(s['profile'], '?'))}</span></h2>"
             f"<div>{pills}</div></div>"
             f"<div class='kv' style='margin-top:14px'>"
             f"<div><span>Firmware</span><span>{fw_text(s, hl)} <span class='crc'>{esc(s['running_crc'])}</span></span></div>"
             f"<div><span>Last check</span>{esc(when(s['last_seen']))}</div>"
             f"<div><span>Last health report</span>{esc(when(s['health_at'])) if s['health_at'] else '<span class=mute>none yet (firmware v6.9.60 on)</span>'}</div>"
             f"<div><span>From address</span><span class='crc'>{esc(s['last_ip'])}</span></div></div></div>")

    # Health
    old_report = bool(hl) and hl.get("sent") in (None, "")
    h.append("<div class='card'><h2>Health</h2><p class='hint'>From the daily report at 12:30-14:15, about the day "
             "08:45 to 08:30 before it." + (" <b>This report is from firmware before v6.9.62</b>, which doesn't send "
                                            "the day's counts or battery averages." if old_report else "") +
             "</p><div class='kv'>")
    for label_, value in health_rows(s, hl):
        h.append(f"<div><span>{esc(label_)}</span><span>{value}</span></div>")
    h.append("</div></div>")

    # Update
    opts = [("type", "Same as its type"), ("hold", "Nothing (updates stopped)")] + [
        (str(b["id"]), build_name(b)) for b in builds]
    sel = "".join(f"<option value='{esc(v)}'{' selected' if v == s['target'] else ''}>{esc(t)}</option>" for v, t in opts)
    bar = f"<div class='bar' style='max-width:260px'><i style='width:{pct}%'></i></div>" if pct is not None else ""
    h.append(f"<div class='card'><h2>Firmware update</h2><p style='margin:0 0 6px'><span class='pill {cls}'>{esc(label)}</span></p>{bar}"
             f"{'<p class=hint>' + esc(detail) + '</p>' if detail else ''}"
             f"<form method='post' action='/station' class='row' style='margin-top:10px'><input type='hidden' name='key' value='{esc(s['key'])}'>"
             f"<input type='hidden' name='back' value='/s?k={esc(s['key'])}'><label class='mute' for='tsel'>Gets</label>"
             f"<select id='tsel' name='target'>{sel}</select><button>Set</button></form>")
    # v6.9.63: CTRL
    cst = ctrl_state(s, cbuild)
    copts = [("", "Nothing")] + [(str(b["id"]), build_name(b)) for b in cbuilds]
    csel = "".join(f"<option value='{esc(v)}'{' selected' if v == (s['ctrl_target'] or '') else ''}>{esc(t)}</option>" for v, t in copts)
    h.append(f"<h2 style='margin-top:18px'>CTRL firmware</h2><p class='hint'>Running CRC "
             f"<span class='crc'>{esc(s['ctrl_crc']) or '— (main firmware before v6.9.63 does not say)'}</span></p>"
             + (f"<p style='margin:0 0 6px'><span class='pill {cst[0]}'>{esc(cst[1])}</span></p>" if cst else "") +
             f"<form method='post' action='/station/ctrl' class='row'><input type='hidden' name='key' value='{esc(s['key'])}'>"
             f"<input type='hidden' name='back' value='/s?k={esc(s['key'])}'><label class='mute' for='csel'>Gets</label>"
             f"<select id='csel' name='target'>{csel}</select><button>Set</button></form></div>")

    # Health history
    h.append("<div class='card'><h2>Health history</h2><div class='scroll'><table><tr><th>Received</th>" +
             "".join(f"<th>{esc(t)}</th>" for t, _ in HISTORY_COLS) + "</tr>")
    for r in reports:
        try:
            d = json.loads(r["data"])
        except ValueError:
            d = {}
        h.append(f"<tr><td class='nw'>{esc(when(r['at']))}</td>" +
                 "".join(f"<td class='nw'>{fn(d)}</td>" for _, fn in HISTORY_COLS) + "</tr>")
    if not reports:
        h.append(f"<tr><td colspan='{len(HISTORY_COLS) + 1}' class='mute'>No health report yet.</td></tr>")
    h.append("</table></div></div>")

    # Events
    h.append("<div class='card'><h2>Events</h2><div class='scroll'><table><tr><th>When</th><th>What happened</th></tr>")
    for e in events:
        t = e["text"]
        c = "badt" if t.startswith("FAILED") else "okt" if t.startswith("UPDATED") else ""
        h.append(f"<tr><td class='nw'>{esc(when(e['at']))}</td><td class='{c}'>{esc(t)}</td></tr>")
    if not events:
        h.append("<tr><td colspan='2' class='mute'>Nothing yet.</td></tr>")
    h.append("</table></div></div>")
    return page(f"Station {s['shown_id']}", "".join(h), "stations", message, ptype=s["profile"])


def firmware_tab(ptype):
    with db() as con:
        builds = con.execute("SELECT * FROM builds WHERE profile IN (?,?) ORDER BY profile, id DESC", (ptype, CTRL_PROFILE)).fetchall()
        targets = {r["build_id"] for r in con.execute("SELECT build_id FROM type_targets WHERE build_id IS NOT NULL")}
        used_by = {}
        for s in con.execute("SELECT shown_id, target FROM stations WHERE target NOT IN ('type','hold')"):
            used_by.setdefault(int(s["target"]), []).append(s["shown_id"])
        for s in con.execute("SELECT shown_id, ctrl_target FROM stations WHERE ctrl_target != ''"):
            if s["ctrl_target"].isdigit():
                used_by.setdefault(int(s["ctrl_target"]), []).append(s["shown_id"] + " (CTRL)")
    h = ["<div class='card'><h2>Upload firmware</h2>"
         "<ol class='steps'><li>Use the same file that goes on the SD card: <code>fw_ksn_trg.hex</code>, "
         "<code>fw_bihar.hex</code>, <code>fw_ksn_tws.hex</code>, or <code>ctrl.hex</code> for the CTRL board "
         "(one file for every type).</li>"
         "<li>The station type and version are read from the file. A damaged or wrong file is refused.</li>"
         "<li>It is filed under its own type, whichever type is chosen above.</li>"
         f"<li>Uploading sends it to nobody. Choose stations on the <a href='/?tab=stations&type={ptype}'>Stations</a> tab.</li></ol>"
         "<form method='post' action='/upload' enctype='multipart/form-data' class='stack'>"
         f"<input type='hidden' name='back' value='/?tab=firmware&type={ptype}'>"
         "<div><label class='f'>File</label><input type='file' name='file' accept='.hex' required></div>"
         "<div><label class='f'>Note (optional)</label><input name='note' placeholder='e.g. SMS fix, for test team' size='34' maxlength='60'></div>"
         "<div><button>Upload</button></div></form></div>"]
    h.append(f"<div class='card'><h2>{esc(TYPES[ptype])} and CTRL firmware</h2><div class='scroll'><table>"
             "<tr><th>Version</th><th>Type</th><th>Uploaded</th><th>Note</th><th>Set for</th><th></th></tr>")
    for b in builds:
        users = []
        if b["id"] in targets:
            users.append(f"all {TYPES.get(b['profile'], '?')}")
        users += used_by.get(b["id"], [])
        h.append(f"<tr><td class='nw'><b>{esc(b['version'])}</b> <span class='mute'>#{b['id']}</span><br>"
                 f"<span class='crc'>CRC {esc(b['image_crc'])} · {b['size'] // 1024} KB</span></td>"
                 f"<td class='nw'>{esc(TYPES.get(b['profile'], 'CTRL, every type'))}</td><td>{esc(when(b['uploaded_at']))}</td>"
                 f"<td>{esc(b['note'])}</td><td>{esc(', '.join(users)) or '<span class=mute>nobody</span>'}</td>"
                 f"<td><form method='post' action='/build/delete' onsubmit=\"return confirm('Delete {esc(b['version'])} (#{b['id']})?')\">"
                 f"<input type='hidden' name='back' value='/?tab=firmware&type={ptype}'>"
                 f"<input type='hidden' name='id' value='{b['id']}'><button class='del small'>Delete</button></form></td></tr>")
    if not builds:
        h.append(f"<tr><td colspan='6' class='mute'>No {esc(TYPES[ptype])} firmware uploaded yet.</td></tr>")
    h.append("</table></div></div>")
    return "".join(h)


def history_tab(ptype):
    with db() as con:
        events = con.execute("SELECT * FROM events WHERE profile=? OR profile IS NULL ORDER BY id DESC LIMIT 300",
                             (ptype,)).fetchall()
    h = [f"<div class='card'><h2>{esc(TYPES[ptype])} history</h2><p class='hint'>The newest first. Office events "
         "that belong to no type (password) show under every type.</p><div class='scroll'><table>"
         "<tr><th>When</th><th>Station</th><th>What happened</th></tr>"]
    for e in events:
        t = e["text"]
        cls = "badt" if t.startswith("FAILED") else "okt" if t.startswith("UPDATED") else ""
        h.append(f"<tr><td class='nw'>{esc(when(e['at']))}</td><td class='nw'>{esc(e['station']) or '<span class=mute>office</span>'}</td>"
                 f"<td class='{cls}'>{esc(t)}</td></tr>")
    if not events:
        h.append("<tr><td colspan='3' class='mute'>Nothing yet.</td></tr>")
    h.append("</table></div></div>")
    return "".join(h)


def password_tab():
    return ("<div class='card pw'><h2>Change the password</h2>"
            f"<p class='hint'>The user name stays <b>{esc(ADMIN_USER)}</b>. At least 8 characters. "
            "Everyone else logged in is logged out.</p>"
            "<form method='post' action='/password' class='stack'>"
            "<div><label class='f'>Current password</label><input type='password' name='current' autocomplete='current-password' required></div>"
            "<div><label class='f'>New password</label><input type='password' name='new' autocomplete='new-password' minlength='8' required></div>"
            "<div><label class='f'>New password again</label><input type='password' name='new2' autocomplete='new-password' minlength='8' required></div>"
            "<div><button>Change password</button></div></form></div>")


def office_page(tab, message="", q=None):
    q = q or {}
    if tab == "password":
        return page("Password", password_tab(), tab, message)
    try:
        ptype = int(q.get("type", ""))
    except ValueError:
        ptype = -1
    ptype = pick_type(ptype)
    name = TYPES[ptype]
    if tab == "firmware":
        return page(f"{name} firmware", firmware_tab(ptype), tab, message, ptype=ptype)
    if tab == "history":
        return page(f"{name} history", history_tab(ptype), tab, message, ptype=ptype)
    body, script = stations_tab(ptype, q.get("q", "").strip()[:12])
    return page(f"{name} stations", body, "stations", message, script, ptype=ptype)


def login_page(error=""):
    err = f"<p class='badt'>{esc(error)}</p>" if error else ""
    return page("Log in", f"<div class='card login'><h2>LPC-AIO firmware updates</h2><p class='hint'>Log in</p>{err}"
                "<form method='post' action='/login'>"
                "<input name='user' placeholder='User' autocomplete='username' required>"
                "<input name='password' type='password' placeholder='Password' autocomplete='current-password' required>"
                "<button>Log in</button></form></div>")


def parse_multipart(ctype, body):
    m = re.search(r'boundary="?([^";]+)"?', ctype)
    if not m:
        return {}
    parts = {}
    for seg in body.split(b"--" + m.group(1).encode())[1:]:
        if seg.startswith(b"--"):
            break
        head, _, data = seg.partition(b"\r\n\r\n")
        if data.endswith(b"\r\n"):
            data = data[:-2]
        name = re.search(rb'name="([^"]*)"', head)
        if name:
            parts[name.group(1).decode()] = data
    return parts


def do_upload(parts):
    data = parts.get("file", b"")
    note = parts.get("note", b"").decode("utf-8", "replace").strip()[:60]
    if not data:
        return "No file chosen.", None
    try:
        profile, version, image_crc, _ = parse_firmware_hex(data)
    except ValueError as e:
        return f"File refused: {e}", None
    image_crc = f"{image_crc:08X}"
    file_crc = f"{zlib.crc32(data) & 0xFFFFFFFF:08X}"
    with _db_lock, db() as con:
        same = con.execute("SELECT id FROM builds WHERE file_crc=? OR image_crc=?", (file_crc, image_crc)).fetchone()
        if same:
            return f"Build #{same['id']} is this same firmware: nothing uploaded.", profile
        cur = con.execute("INSERT INTO builds (profile, version, image_crc, file_crc, size, note, uploaded_at)"
                          " VALUES (?,?,?,?,?,?,?)", (profile, version, image_crc, file_crc, len(data), note, int(time.time())))
        build_id = cur.lastrowid
        tmp = os.path.join(BUILDS_DIR, f"{build_id}.tmp")
        with open(tmp, "wb") as f:
            f.write(data)
        os.replace(tmp, os.path.join(BUILDS_DIR, f"{build_id}.hex"))
        kind = TYPES.get(profile, "CTRL")
        add_event(con, "", f"build #{build_id} uploaded: {kind} {version} ({image_crc}) {note}",
                  None if profile == CTRL_PROFILE else profile)
    return (f"Uploaded {version} for {kind} as #{build_id}. "
            "Nobody gets it yet: send it from the Stations tab."), (None if profile == CTRL_PROFILE else profile)


def do_send(f, stn_keys):
    if not f.get("build", "").isdigit():
        return "No firmware chosen."
    build_id = int(f["build"])
    all_type = f.get("all") == "1"
    with _db_lock, db() as con:
        b = con.execute("SELECT * FROM builds WHERE id=?", (build_id,)).fetchone()
        if b is None:
            return "No such firmware."
        sent, skipped = [], []
        if b["profile"] == CTRL_PROFILE:
            # v6.9.63: a CTRL file, to the ticked stations (or every station of the type shown, as they are now)
            keys = list(stn_keys)
            if all_type and f.get("back", "").startswith("/?tab=stations&type="):
                t = f["back"][-1]
                keys += [r["key"] for r in con.execute("SELECT key FROM stations WHERE profile=?", (int(t),)) if t.isdigit()]
            for key in dict.fromkeys(keys):
                st = con.execute("SELECT * FROM stations WHERE key=?", (key,)).fetchone()
                if st is None:
                    continue
                con.execute("UPDATE stations SET ctrl_target=?, ctrl_result='', ctrl_offered='' WHERE key=?", (str(build_id), key))
                add_event(con, st["shown_id"], f"office set: CTRL gets build #{build_id} {b['version']}")
                sent.append(st["shown_id"])
            if not sent:
                return "Not sent: tick at least one station."
            return (f"CTRL {b['version']} set for {', '.join(sent)}. Each takes it at its next check, after any "
                    "main-board update (main firmware v6.9.63 or later needed).")
        for key in stn_keys:
            st = con.execute("SELECT * FROM stations WHERE key=?", (key,)).fetchone()
            if st is None:
                continue
            if st["profile"] != b["profile"]:
                skipped.append(st["shown_id"])
                continue
            con.execute("UPDATE stations SET target=?, result='', offered_crc=NULL WHERE key=?", (str(build_id), key))
            add_event(con, st["shown_id"], f"office set: gets build #{build_id} {b['version']}")
            sent.append(st["shown_id"])
        if all_type:
            con.execute("INSERT INTO type_targets (profile, build_id) VALUES (?,?) "
                        "ON CONFLICT(profile) DO UPDATE SET build_id=excluded.build_id", (b["profile"], build_id))
            for st in con.execute("SELECT * FROM stations WHERE profile=? AND target IN ('type','hold')", (b["profile"],)).fetchall():
                con.execute("UPDATE stations SET target='type', result='', offered_crc=NULL WHERE key=?", (st["key"],))
            add_event(con, "", f"office set: all {TYPES[b['profile']]} stations get build #{build_id} {b['version']}", b["profile"])
    if not sent and not all_type:
        return "Not sent: tick at least one station of this type." + (f" Skipped (other type): {', '.join(skipped)}." if skipped else "")
    parts = []
    if sent:
        parts.append(f"{b['version']} set for {', '.join(sent)}")
    if all_type:
        parts.append(f"{b['version']} set for every {TYPES[b['profile']]} station")
    msg = ". ".join(parts) + ". Each station takes it at its next check."
    if skipped:
        msg += f" Skipped (other type): {', '.join(skipped)}."
    return msg


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "lpc-ota"
    sys_version = ""
    timeout = 30

    def log_message(self, fmt, *args):
        pass

    def client_ip(self):
        ip = self.client_address[0]
        if ip == "127.0.0.1" and self.headers.get("X-Real-IP"):
            return self.headers["X-Real-IP"]
        return ip

    def send(self, status, body, ctype="text/html; charset=utf-8", headers=()):
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        if ctype.startswith("text/html"):
            self.send_header("Cache-Control", "no-store")
        for k, v in headers:
            self.send_header(k, v)
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def redirect(self, where, headers=()):
        self.send(303, b"", headers=(("Location", where),) + tuple(headers))

    def session(self):
        m = re.search(r"(?:^|;\s*)lpcota=([0-9a-f]{48})", self.headers.get("Cookie", ""))
        if m and SESSIONS.get(m.group(1), 0) > time.time():
            return m.group(1)
        return None

    def form(self):
        """Returns (first value of each field, all values of each field), or (None, None) if too large."""
        length = int(self.headers.get("Content-Length", "0") or 0)
        if length > MAX_UPLOAD:
            return None, None
        body = self.rfile.read(length)
        ctype = self.headers.get("Content-Type", "")
        if ctype.startswith("multipart/form-data"):
            return parse_multipart(ctype, body), {}
        lists = parse_qs(body.decode("utf-8", "replace"))
        return {k: v[0] for k, v in lists.items()}, lists

    def do_GET(self):
        url = urlparse(self.path)
        q = {k: v[0] for k, v in parse_qs(url.query).items()}
        if url.path == "/lpc/check":
            return self.send(200, unit_check(q, self.client_ip()).encode(), "text/plain")
        if url.path == "/lpc/fw":
            status, body = unit_piece(q)
            return self.send(status, body, "application/octet-stream")
        if url.path.startswith("/lpc/"):
            return self.send(404, b"OTA BAD\n", "text/plain")
        if url.path == "/login":
            return self.send(200, login_page())
        if url.path == "/logout":
            tok = self.session()
            if tok:
                SESSIONS.pop(tok, None)
            return self.redirect("/login", (("Set-Cookie", "lpcota=; Path=/; Max-Age=0"),))
        if not self.session():
            return self.redirect("/login")
        if url.path == "/":
            return self.send(200, office_page(q.get("tab", "stations"), q.get("m", ""), q))
        if url.path == "/s":
            body = station_page(q.get("k", ""), q.get("m", ""))
            if body:
                return self.send(200, body)
        self.send(404, page("Not found", "<div class='card'>Not found. <a href='/'>Back</a></div>"))

    do_HEAD = do_GET

    def do_POST(self):
        url = urlparse(self.path)
        if url.path.startswith("/lpc/"):
            return self.send(404, b"OTA BAD\n", "text/plain")
        f, lists = self.form()
        if f is None:
            return self.send(413, page("Too large", "<div class='card'>File too large. <a href='/'>Back</a></div>"))
        if url.path == "/login":
            ip = self.client_ip()
            count, since = FAILED_LOGINS.get(ip, (0, 0))
            if count >= 8 and time.time() - since < 900:
                return self.send(429, login_page("Too many wrong tries. Wait 15 minutes."))
            if not password_ok(f.get("user", ""), f.get("password", "")):
                FAILED_LOGINS[ip] = (count + 1 if time.time() - since < 900 else 1, time.time())
                time.sleep(1)
                return self.send(401, login_page("Wrong user or password."))
            FAILED_LOGINS.pop(ip, None)
            token = secrets.token_hex(24)
            SESSIONS[token] = time.time() + 12 * 3600
            return self.redirect("/", (("Set-Cookie", f"lpcota={token}; Path=/; HttpOnly; SameSite=Strict; Max-Age=43200"),))
        token = self.session()
        if not token:
            return self.redirect("/login")
        message, tab = "", "stations"
        if url.path == "/upload":
            parts = {k: (v if isinstance(v, bytes) else v.encode()) for k, v in f.items()}
            message, up_type = do_upload(parts)
            tab = "firmware"
            f = {"back": f"/?tab=firmware&type={up_type}" if up_type is not None else
                 parts.get("back", b"").decode("utf-8", "replace")}
        elif url.path == "/send":
            message = do_send(f, lists.get("stn", []))
        elif url.path == "/station":
            target = f.get("target", "type")
            if target in ("type", "hold") or target.isdigit():
                with _db_lock, db() as con:
                    st = con.execute("SELECT * FROM stations WHERE key=?", (f.get("key", ""),)).fetchone()
                    if st:
                        con.execute("UPDATE stations SET target=?, result='', offered_crc=NULL WHERE key=?", (target, st["key"]))
                        add_event(con, st["shown_id"], f"office set: {'updates resumed (follows its type)' if target == 'type' else 'updates stopped' if target == 'hold' else 'gets build #' + target}")
                        message = (f"Updates stopped for {st['shown_id']}." if target == "hold" else
                                   f"Updates resumed for {st['shown_id']}." if target == "type" else
                                   f"Station {st['shown_id']} set. It acts on this at its next check.")
        elif url.path == "/station/ctrl":
            target = f.get("target", "")
            if target == "" or target.isdigit():
                with _db_lock, db() as con:
                    st = con.execute("SELECT * FROM stations WHERE key=?", (f.get("key", ""),)).fetchone()
                    if st:
                        con.execute("UPDATE stations SET ctrl_target=?, ctrl_result='', ctrl_offered='' WHERE key=?", (target, st["key"]))
                        add_event(con, st["shown_id"], "office set: " + (f"CTRL gets build #{target}" if target else "no CTRL update"))
                        message = (f"Station {st['shown_id']}: CTRL set. It acts on this at its next check." if target else
                                   f"Station {st['shown_id']}: no CTRL update.")
        elif url.path == "/station/add":
            stn = f.get("id", "")
            if re.fullmatch(r"[0-9A-Za-z]{1,12}", stn) and f.get("profile", "") in ("0", "1", "2"):
                with _db_lock, db() as con:
                    con.execute("INSERT OR IGNORE INTO stations (key, shown_id, profile, version, running_crc, unit_status,"
                                " tried_crc, last_seen, last_ip) VALUES (?,?,?,'','','0','',0,'')",
                                (station_key(stn), stn, int(f["profile"])))
                message = f"Station {stn} added."
        elif url.path == "/type":
            if f.get("profile", "") in ("0", "1", "2") and (f.get("build", "") == "" or f.get("build", "").isdigit()):
                with _db_lock, db() as con:
                    build = int(f["build"]) if f.get("build") else None
                    con.execute("INSERT INTO type_targets (profile, build_id) VALUES (?,?) "
                                "ON CONFLICT(profile) DO UPDATE SET build_id=excluded.build_id", (int(f["profile"]), build))
                    add_event(con, "", f"office set: all {TYPES[int(f['profile'])]} stations get "
                                       f"{'build #' + str(build) if build else 'nothing'}", int(f["profile"]))
                    message = (f"Every {TYPES[int(f['profile'])]} station set to get build #{build}." if build else
                               f"Whole-type update stopped for {TYPES[int(f['profile'])]}.")
        elif url.path == "/build/delete":
            tab = "firmware"
            if f.get("id", "").isdigit():
                build_id = int(f["id"])
                with _db_lock, db() as con:
                    b = con.execute("SELECT profile FROM builds WHERE id=?", (build_id,)).fetchone()
                    used = con.execute("SELECT 1 FROM type_targets WHERE build_id=? UNION SELECT 1 FROM stations WHERE target=?"
                                       " UNION SELECT 1 FROM stations WHERE ctrl_target=?",
                                       (build_id, str(build_id), str(build_id))).fetchone()
                    if used:
                        message = f"Build #{build_id} is still set for a station or a type: stop that first on the Stations tab."
                    else:
                        con.execute("DELETE FROM builds WHERE id=?", (build_id,))
                        try:
                            os.remove(os.path.join(BUILDS_DIR, f"{build_id}.hex"))
                        except OSError:
                            pass
                        add_event(con, "", f"build #{build_id} deleted", b["profile"] if b else None)
                        message = f"Build #{build_id} deleted."
        elif url.path == "/password":
            tab = "password"
            new = f.get("new", "")
            if not password_ok(ADMIN_USER, f.get("current", "")):
                time.sleep(1)
                message = "Wrong current password: not changed."
            elif len(new) < 8:
                message = "The new password must be at least 8 characters: not changed."
            elif new != f.get("new2", ""):
                message = "The new password was not the same both times: not changed."
            else:
                with _db_lock, db() as con:
                    con.execute("INSERT INTO settings (key, value) VALUES ('admin_pw', ?) "
                                "ON CONFLICT(key) DO UPDATE SET value=excluded.value", (hash_password(new),))
                    add_event(con, "", "office password changed")
                for t in list(SESSIONS):
                    if t != token:
                        SESSIONS.pop(t, None)
                message = "Password changed. Use the new one from now on."
        from urllib.parse import quote
        back = f.get("back", "") if isinstance(f.get("back", ""), str) else ""
        if not re.fullmatch(r"/(\?tab=(stations|firmware|history)&type=[0-2]|s\?k=[0-9A-Za-z]{1,12})", back):
            back = f"/?tab={tab}"
        self.redirect(back + ("&m=" + quote(message) if message else ""))


if __name__ == "__main__":
    init_db()
    if not ADMIN_PASSWORD and not get_setting("admin_pw"):
        print("No password set (LPCOTA_PASSWORD in ota.env): nobody can log in to the office page")
    server = ThreadingHTTPServer((BIND, PORT), Handler)
    server.daemon_threads = True
    server.block_on_close = False
    server.serve_forever()
