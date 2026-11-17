#!/usr/bin/env python3
"""LPC-AIO firmware-over-the-air server. One file, Python standard library only.

Units (port 80, through nginx, paths under /lpc/):
  GET /lpc/check?id=&p=&v=&c=&s=&t=   -> "OTA NONE" or "OTA FW <file bytes> <file crc> <image crc>"
  GET /lpc/fw?id=&c=<file crc>&o=<offset>&n=<bytes>
                                      -> "@<crc32 of the piece>\n" + the piece of the .hex file
Office (port 5002, login): tabs Stations (send an update, see each station's state), Firmware files
(upload), History, Password (change the admin password; kept hashed in the database, ota.env is the fallback).

A file is accepted only if it is a complete main-firmware .hex whose image header and CRC are right:
the same checks the unit makes before it installs anything.
"""
import contextlib
import hashlib
import hmac
import html
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
    if lo != SLOT_A_BASE or hi >= SLOT_A_BASE + SLOT_SIZE:
        raise ValueError("not a main firmware file (fw_ksn_trg.hex, fw_bihar.hex or fw_ksn_tws.hex): "
                         f"its data is at 0x{lo:05X}-0x{hi:05X}")
    if len(mem) != hi - lo + 1:
        raise ValueError("lines missing in the file")
    img = bytearray(mem[a] for a in range(lo, hi + 1))
    off = struct.unpack_from("<I", img, 8 * 4)[0] - SLOT_A_BASE
    if off < 0 or off + 36 > len(img):
        raise ValueError("no image header")
    magic, board, length, crc, profile = struct.unpack_from("<IIIII", img, off)
    if magic != IMAGE_MAGIC or board != BOARD_ID:
        raise ValueError("no image header for the main board")
    if length != len(img):
        raise ValueError(f"image length {length} in the header, {len(img)} in the file")
    struct.pack_into("<I", img, off + 12, 0)
    if zlib.crc32(bytes(img)) & 0xFFFFFFFF != crc:
        raise ValueError("image CRC is wrong: a damaged file")
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
        """)


def add_event(con, station, text):
    con.execute("INSERT INTO events (at, station, text) VALUES (?,?,?)", (int(time.time()), station, text))
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
    key = station_key(stn)
    now = int(time.time())
    with _db_lock, db() as con:
        st = con.execute("SELECT * FROM stations WHERE key=?", (key,)).fetchone()
        if st is None:
            con.execute("INSERT INTO stations (key, shown_id, profile, version, running_crc, unit_status, tried_crc,"
                        " last_seen, last_ip) VALUES (?,?,?,?,?,?,?,?,?)",
                        (key, stn, profile, version, crc, status, tried, now, ip))
            add_event(con, stn, f"first check-in: {TYPES.get(profile, '?')} {version} ({crc})")
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
        con.execute("UPDATE stations SET progress=?, result=? WHERE key=?",
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


def page(title, body, tab=None, message="", script=""):
    head = (f"<!doctype html><html lang='en'><head><meta charset='utf-8'>"
            f"<meta name='viewport' content='width=device-width,initial-scale=1'><title>{esc(title)}</title>"
            f"<style>{PAGE_CSS}</style></head><body>")
    if tab is None:
        return (head + f"<main>{body}</main></body></html>").encode()
    nav = "".join(f"<a href='/?tab={k}' class='{'on' if k == tab else ''}'>{t}</a>" for k, t in TABS)
    msg = ""
    if message:
        cls = "msg bad" if message.lower().startswith(("file refused", "wrong", "not ", "no ", "the new", "build #")) and "deleted" not in message else "msg"
        msg = f"<div class='{cls}'>{esc(message)}</div>"
    return (head + f"<header><div class='top'><b>LPC-AIO firmware updates</b><a href='/logout'>Log out</a></div>"
            f"<nav>{nav}</nav></header><main>{msg}{body}</main>{script}</body></html>").encode()


def build_name(b):
    return f"{b['version']} · {TYPES.get(b['profile'], '?')} (#{b['id']})"


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


def stations_tab():
    with db() as con:
        builds = con.execute("SELECT * FROM builds ORDER BY id DESC").fetchall()
        targets = {r["profile"]: r["build_id"] for r in con.execute("SELECT * FROM type_targets")}
        stations = con.execute("SELECT * FROM stations ORDER BY CAST(key AS INTEGER), key").fetchall()
        st_builds = {s["key"]: target_build(con, s) for s in stations}
    by_id = {b["id"]: b for b in builds}
    by_crc = {b["image_crc"]: b for b in builds}
    h = []

    # 1. Send an update
    h.append("<div class='card'><h2>Send an update</h2>")
    if not builds:
        h.append("<p class='hint'>First upload a firmware file on the <a href='/?tab=firmware'>Firmware files</a> tab.</p></div>")
    else:
        opts = "".join(f"<option value='{b['id']}' data-p='{b['profile']}'>{esc(build_name(b))}"
                       f"{' · ' + esc(b['note']) if b['note'] else ''}</option>" for b in builds)
        ticks = "".join(
            f"<label class='tick' data-p='{s['profile']}'><input type='checkbox' name='stn' value='{esc(s['key'])}'>"
            f"<span><b>{esc(s['shown_id'])}</b> <span class='mute'>{esc(s['version'] or 'not seen yet')}</span></span></label>"
            for s in stations)
        h.append("<p class='hint'>Choose the firmware, tick the stations, press Send. Try one station first.</p>"
                 "<form method='post' action='/send' class='stack'>"
                 f"<div><label class='f' for='bsel'>1. Firmware</label><select id='bsel' name='build' required>{opts}</select></div>"
                 "<div><label class='f'>2. Stations</label>"
                 f"<div class='ticks' id='ticks'>{ticks}</div>"
                 "<p class='hint' id='noone' style='display:none;margin-top:8px'>No station of this type has checked in yet.</p></div>"
                 "<label class='tick' style='align-self:flex-start'><input type='checkbox' name='all' value='1'>"
                 "<span>Every station of this type, including ones that check in later</span></label>"
                 "<div><button>Send update</button></div></form>")
        h.append("</div>")

    # 2. Station list
    h.append("<div class='card'><h2>Stations</h2><p class='hint'>Updates every 30 seconds. A station appears after its "
             "first check (once a day, or every slot with OTA.NOW on its card).</p><div class='list'>")
    for s in stations:
        build = st_builds[s["key"]]
        cls, label, detail, pct = station_state(s, build)
        running = by_crc.get(s["running_crc"])
        run = esc(s["version"]) or "<span class='mute'>not seen yet</span>"
        bar = f"<div class='bar'><i style='width:{pct}%'></i></div>" if pct is not None else ""
        target_line = ""
        if build is not None and s["target"] != "hold":
            how = "for the whole type" if s["target"] == "type" else "for this station"
            target_line = f"<div class='sub'>Set to get {esc(build['version'])} (#{build['id']}) {how}</div>"
        action = ""
        if s["target"] == "hold":
            action = (f"<form method='post' action='/station'><input type='hidden' name='key' value='{esc(s['key'])}'>"
                      f"<input type='hidden' name='target' value='type'><button class='plain small'>Resume updates</button></form>")
        elif build is not None and not label.startswith(("Up to date", "Updated")):
            action = (f"<form method='post' action='/station'><input type='hidden' name='key' value='{esc(s['key'])}'>"
                      f"<input type='hidden' name='target' value='hold'><button class='del small'>Stop</button></form>")
        h.append(f"<div class='st'><div><div class='id'>{esc(s['shown_id'])} "
                 f"<span class='sub' style='font-weight:400'>{esc(TYPES.get(s['profile'], '?'))}</span></div>"
                 f"<div>Running <b>{run}</b>{' (#' + str(running['id']) + ')' if running else ''} "
                 f"<span class='crc'>{esc(s['running_crc'])}</span></div>"
                 f"<div class='sub'>Last check: {esc(when(s['last_seen']))}</div>{target_line}</div>"
                 f"<div class='right'><span class='pill {cls}'>{esc(label)}</span>{bar}"
                 f"{'<div class=sub style=max-width:300px;margin-top:4px>' + esc(detail) + '</div>' if detail else ''}"
                 f"<div style='margin-top:6px'>{action}</div></div></div>")
    if not stations:
        h.append("<p class='mute'>No station has checked in yet.</p>")
    h.append("</div>")
    whole = [(p, by_id.get(b)) for p, b in targets.items() if b and by_id.get(b)]
    if whole:
        h.append("<p class='hint' style='margin-top:14px'><b>Whole-type updates:</b></p><div class='stack'>")
        for p, b in whole:
            h.append(f"<form method='post' action='/type' class='row'><input type='hidden' name='profile' value='{p}'>"
                     f"<input type='hidden' name='build' value=''><span>Every {esc(TYPES[p])} station gets "
                     f"<b>{esc(b['version'])}</b> (#{b['id']})</span><button class='del small'>Stop for the type</button></form>")
        h.append("</div>")
    h.append("<details style='margin-top:14px'><summary>Add a station that hasn't checked in yet</summary>"
             "<form method='post' action='/station/add' class='row' style='margin-top:10px'>"
             "<input name='id' placeholder='Station ID, e.g. 001922' size='18' required pattern='[0-9A-Za-z]{1,12}'>"
             "<select name='profile'>" + "".join(f"<option value='{p}'>{t}</option>" for p, t in TYPES.items()) +
             "</select><button class='plain'>Add</button></form></details></div>")

    script = """<script>
(function(){
  var sel=document.getElementById('bsel');
  function filt(){ if(!sel) return; var p=sel.options[sel.selectedIndex].getAttribute('data-p'), n=0;
    document.querySelectorAll('#ticks .tick').forEach(function(t){ var on=t.getAttribute('data-p')===p;
      t.classList.toggle('hide',!on); if(!on) t.querySelector('input').checked=false; else n++; });
    document.getElementById('noone').style.display=n?'none':'block'; }
  if(sel){ sel.addEventListener('change',filt); filt(); }
  setInterval(function(){
    var busy=document.querySelector('input[type=checkbox]:checked')||
      (document.activeElement&&/INPUT|SELECT|TEXTAREA/.test(document.activeElement.tagName));
    if(!busy) location.replace('/?tab=stations');
  },30000);
})();
</script>"""
    return "".join(h), script


def firmware_tab():
    with db() as con:
        builds = con.execute("SELECT * FROM builds ORDER BY id DESC").fetchall()
        targets = {r["build_id"] for r in con.execute("SELECT build_id FROM type_targets WHERE build_id IS NOT NULL")}
        used_by = {}
        for s in con.execute("SELECT shown_id, target FROM stations WHERE target NOT IN ('type','hold')"):
            used_by.setdefault(int(s["target"]), []).append(s["shown_id"])
    h = ["<div class='card'><h2>Upload firmware</h2>"
         "<ol class='steps'><li>Use the same file that goes on the SD card: <code>fw_ksn_trg.hex</code>, "
         "<code>fw_bihar.hex</code> or <code>fw_ksn_tws.hex</code>.</li>"
         "<li>The station type and version are read from the file. A damaged or wrong file is refused.</li>"
         "<li>Uploading sends it to nobody. Choose stations on the <a href='/?tab=stations'>Stations</a> tab.</li></ol>"
         "<form method='post' action='/upload' enctype='multipart/form-data' class='stack'>"
         "<div><label class='f'>File</label><input type='file' name='file' accept='.hex' required></div>"
         "<div><label class='f'>Note (optional)</label><input name='note' placeholder='e.g. SMS fix, for test team' size='34' maxlength='60'></div>"
         "<div><button>Upload</button></div></form></div>"]
    h.append("<div class='card'><h2>Uploaded firmware</h2><div class='scroll'><table>"
             "<tr><th>Version</th><th>Type</th><th>Uploaded</th><th>Note</th><th>Set for</th><th></th></tr>")
    for b in builds:
        users = []
        if b["id"] in targets:
            users.append(f"all {TYPES.get(b['profile'], '?')}")
        users += used_by.get(b["id"], [])
        h.append(f"<tr><td class='nw'><b>{esc(b['version'])}</b> <span class='mute'>#{b['id']}</span><br>"
                 f"<span class='crc'>CRC {esc(b['image_crc'])} · {b['size'] // 1024} KB</span></td>"
                 f"<td class='nw'>{esc(TYPES.get(b['profile'], '?'))}</td><td>{esc(when(b['uploaded_at']))}</td>"
                 f"<td>{esc(b['note'])}</td><td>{esc(', '.join(users)) or '<span class=mute>nobody</span>'}</td>"
                 f"<td><form method='post' action='/build/delete' onsubmit=\"return confirm('Delete {esc(b['version'])} (#{b['id']})?')\">"
                 f"<input type='hidden' name='id' value='{b['id']}'><button class='del small'>Delete</button></form></td></tr>")
    if not builds:
        h.append("<tr><td colspan='6' class='mute'>No firmware uploaded yet.</td></tr>")
    h.append("</table></div></div>")
    return "".join(h)


def history_tab():
    with db() as con:
        events = con.execute("SELECT * FROM events ORDER BY id DESC LIMIT 300").fetchall()
    h = ["<div class='card'><h2>History</h2><p class='hint'>The newest first.</p><div class='scroll'><table>"
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


def office_page(tab, message=""):
    if tab == "firmware":
        return page("Firmware files", firmware_tab(), tab, message)
    if tab == "history":
        return page("History", history_tab(), tab, message)
    if tab == "password":
        return page("Password", password_tab(), tab, message)
    body, script = stations_tab()
    return page("LPC-AIO firmware updates", body, "stations", message, script)


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
        return "No file chosen."
    try:
        profile, version, image_crc, _ = parse_firmware_hex(data)
    except ValueError as e:
        return f"File refused: {e}"
    image_crc = f"{image_crc:08X}"
    file_crc = f"{zlib.crc32(data) & 0xFFFFFFFF:08X}"
    with _db_lock, db() as con:
        same = con.execute("SELECT id FROM builds WHERE file_crc=? OR image_crc=?", (file_crc, image_crc)).fetchone()
        if same:
            return f"Build #{same['id']} is this same firmware: nothing uploaded."
        cur = con.execute("INSERT INTO builds (profile, version, image_crc, file_crc, size, note, uploaded_at)"
                          " VALUES (?,?,?,?,?,?,?)", (profile, version, image_crc, file_crc, len(data), note, int(time.time())))
        build_id = cur.lastrowid
        tmp = os.path.join(BUILDS_DIR, f"{build_id}.tmp")
        with open(tmp, "wb") as f:
            f.write(data)
        os.replace(tmp, os.path.join(BUILDS_DIR, f"{build_id}.hex"))
        add_event(con, "", f"build #{build_id} uploaded: {TYPES[profile]} {version} ({image_crc}) {note}")
    return (f"Uploaded {version} for {TYPES[profile]} as #{build_id}. "
            "Nobody gets it yet: send it from the Stations tab.")


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
            add_event(con, "", f"office set: all {TYPES[b['profile']]} stations get build #{build_id} {b['version']}")
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
            return self.send(200, office_page(q.get("tab", "stations"), q.get("m", "")))
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
            message, tab = do_upload({k: (v if isinstance(v, bytes) else v.encode()) for k, v in f.items()}), "firmware"
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
                                       f"{'build #' + str(build) if build else 'nothing'}")
                    message = (f"Every {TYPES[int(f['profile'])]} station set to get build #{build}." if build else
                               f"Whole-type update stopped for {TYPES[int(f['profile'])]}.")
        elif url.path == "/build/delete":
            tab = "firmware"
            if f.get("id", "").isdigit():
                build_id = int(f["id"])
                with _db_lock, db() as con:
                    used = con.execute("SELECT 1 FROM type_targets WHERE build_id=? UNION SELECT 1 FROM stations WHERE target=?",
                                       (build_id, str(build_id))).fetchone()
                    if used:
                        message = f"Build #{build_id} is still set for a station or a type: stop that first on the Stations tab."
                    else:
                        con.execute("DELETE FROM builds WHERE id=?", (build_id,))
                        try:
                            os.remove(os.path.join(BUILDS_DIR, f"{build_id}.hex"))
                        except OSError:
                            pass
                        add_event(con, "", f"build #{build_id} deleted")
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
        self.redirect(f"/?tab={tab}" + ("&m=" + quote(message) if message else ""))


if __name__ == "__main__":
    init_db()
    if not ADMIN_PASSWORD and not get_setting("admin_pw"):
        print("No password set (LPCOTA_PASSWORD in ota.env): nobody can log in to the office page")
    server = ThreadingHTTPServer((BIND, PORT), Handler)
    server.daemon_threads = True
    server.block_on_close = False
    server.serve_forever()
