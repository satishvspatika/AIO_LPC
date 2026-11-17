#!/usr/bin/env python3
"""LPC-AIO firmware-over-the-air server. One file, Python standard library only.

Units (port 80, through nginx, paths under /lpc/):
  GET /lpc/check?id=&p=&v=&c=&s=&t=   -> "OTA NONE" or "OTA FW <file bytes> <file crc> <image crc>"
  GET /lpc/fw?id=&c=<file crc>&o=<offset>&n=<bytes>
                                      -> "@<crc32 of the piece>\n" + the piece of the .hex file
Office (port 5002, login): upload a firmware file, choose which stations get it, see each station's
version and last result.

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


# ---------------------------------------------------------------- office pages

SESSIONS = {}
FAILED_LOGINS = {}
PAGE_CSS = """
:root{--bg:#f6f7f9;--card:#fff;--ink:#1c2430;--mute:#667085;--line:#e3e6ea;--acc:#1f6feb;--ok:#18794e;--bad:#b42318}
@media (prefers-color-scheme: dark){:root{--bg:#12161c;--card:#1a2029;--ink:#e6e9ee;--mute:#98a2b3;--line:#2b3340;--acc:#6ea8fe;--ok:#4ac48a;--bad:#f97066}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--ink);font:15px/1.45 system-ui,-apple-system,Segoe UI,Roboto,sans-serif}
header{display:flex;justify-content:space-between;align-items:center;padding:14px 20px;border-bottom:1px solid var(--line);background:var(--card)}
header b{font-size:17px}main{max-width:1100px;margin:0 auto;padding:16px}
section{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:16px;margin-bottom:16px}
h2{margin:0 0 4px;font-size:16px}p.h{margin:0 0 12px;color:var(--mute);font-size:13px}
.scroll{overflow-x:auto}table{border-collapse:collapse;width:100%;font-size:14px}
th,td{text-align:left;padding:7px 10px;border-bottom:1px solid var(--line);white-space:nowrap}
th{color:var(--mute);font-weight:600;font-size:12px;text-transform:uppercase;letter-spacing:.03em}
td.w{white-space:normal;min-width:220px}code{font:13px ui-monospace,Menlo,Consolas,monospace}
input,select,button{font:inherit;padding:6px 9px;border:1px solid var(--line);border-radius:7px;background:var(--card);color:var(--ink)}
button{background:var(--acc);color:#fff;border-color:var(--acc);cursor:pointer}button.q{background:transparent;color:var(--bad);border-color:var(--line)}
form.row{display:flex;gap:8px;flex-wrap:wrap;align-items:center}
.ok{color:var(--ok)}.bad{color:var(--bad)}.mute{color:var(--mute)}
.msg{padding:10px 14px;border-radius:8px;margin-bottom:16px;border:1px solid var(--line);background:var(--card)}
.login{max-width:340px;margin:12vh auto;}.login input{width:100%;margin-bottom:10px}
a{color:var(--acc)}
"""


def esc(v):
    return html.escape(str(v if v is not None else ""))


def when(ts):
    if not ts:
        return ""
    age = int(time.time()) - ts
    text = time.strftime("%d %b %H:%M", time.localtime(ts))
    if age < 3600:
        return f"{text} ({age // 60} min ago)"
    if age < 172800:
        return f"{text} ({age // 3600} h ago)"
    return f"{text} ({age // 86400} d ago)"


def page(title, body, logged_in=True):
    out = "<a href='/logout'>Log out</a>" if logged_in else ""
    return (f"<!doctype html><html lang='en'><head><meta charset='utf-8'>"
            f"<meta name='viewport' content='width=device-width,initial-scale=1'><title>{esc(title)}</title>"
            f"<style>{PAGE_CSS}</style></head><body><header><b>LPC-AIO firmware updates</b>{out}</header>"
            f"<main>{body}</main></body></html>").encode()


def build_label(b):
    note = f" · {b['note']}" if b["note"] else ""
    return f"#{b['id']} {TYPES.get(b['profile'], '?')} {b['version']} ({b['image_crc']}){note}"


def dashboard(message=""):
    with db() as con:
        builds = con.execute("SELECT * FROM builds ORDER BY id DESC").fetchall()
        targets = {r["profile"]: r["build_id"] for r in con.execute("SELECT * FROM type_targets")}
        stations = con.execute("SELECT * FROM stations ORDER BY CAST(key AS INTEGER), key").fetchall()
        events = con.execute("SELECT * FROM events ORDER BY id DESC LIMIT 60").fetchall()
    by_crc = {b["image_crc"]: b for b in builds}
    h = []
    if message:
        h.append(f"<div class='msg'>{esc(message)}</div>")

    h.append("<section><h2>Stations</h2><p class='h'>A station appears here after its first daily check. "
             "\"Follow its type\" gives it whatever is set for the whole type below.</p><div class='scroll'><table>"
             "<tr><th>Station</th><th>Type</th><th>Running</th><th>Last check</th><th>Unit says</th>"
             "<th>Gets</th><th>Result</th></tr>")
    for s in stations:
        running = by_crc.get(s["running_crc"])
        run_text = f"{esc(s['version'])} <code>{esc(s['running_crc'])}</code>" + (f" · build #{running['id']}" if running else "")
        opts = [("type", "Follow its type"), ("hold", "Nothing (hold)")] + [
            (str(b["id"]), build_label(b)) for b in builds if b["profile"] == s["profile"]]
        sel = "".join(f"<option value='{esc(v)}'{' selected' if v == s['target'] else ''}>{esc(t)}</option>" for v, t in opts)
        res = esc(s["result"])
        cls = "bad" if res.startswith("FAILED") else "ok" if "running the target" in res else ""
        h.append(f"<tr><td><b>{esc(s['shown_id'])}</b></td><td>{esc(TYPES.get(s['profile'], '?'))}</td><td>{run_text}</td>"
                 f"<td>{esc(when(s['last_seen']))}</td><td>{esc(UNIT_STATUS.get(s['unit_status'], s['unit_status']))}</td>"
                 f"<td><form class='row' method='post' action='/station'><input type='hidden' name='key' value='{esc(s['key'])}'>"
                 f"<select name='target'>{sel}</select><button>Set</button></form></td><td class='w {cls}'>{res}</td></tr>")
    if not stations:
        h.append("<tr><td colspan='7' class='mute'>No station has checked in yet.</td></tr>")
    h.append("</table></div><form class='row' method='post' action='/station/add' style='margin-top:12px'>"
             "<input name='id' placeholder='Station ID' size='12' required pattern='[0-9A-Za-z]{1,12}'>"
             "<select name='profile'>" + "".join(f"<option value='{p}'>{t}</option>" for p, t in TYPES.items()) +
             "</select><button>Add a station before it checks in</button></form></section>")

    h.append("<section><h2>Whole type</h2><p class='h'>Every station of the type that follows its type gets this build. "
             "Try a build on one station first.</p><div class='scroll'><table><tr><th>Type</th><th>Build for all its stations</th></tr>")
    for p, tname in TYPES.items():
        opts = "<option value=''>None</option>" + "".join(
            f"<option value='{b['id']}'{' selected' if targets.get(p) == b['id'] else ''}>{esc(build_label(b))}</option>"
            for b in builds if b["profile"] == p)
        h.append(f"<tr><td>{tname}</td><td><form class='row' method='post' action='/type'>"
                 f"<input type='hidden' name='profile' value='{p}'><select name='build'>{opts}</select>"
                 f"<button>Set</button></form></td></tr>")
    h.append("</table></div></section>")

    h.append("<section><h2>Firmware files</h2><p class='h'>Upload the same file that goes on the SD card "
             "(fw_ksn_trg.hex, fw_bihar.hex or fw_ksn_tws.hex). The type and version are read from the file.</p>"
             "<form class='row' method='post' action='/upload' enctype='multipart/form-data'>"
             "<input type='file' name='file' accept='.hex' required><input name='note' placeholder='Note, e.g. build 22' size='24' maxlength='60'>"
             "<button>Upload</button></form><div class='scroll' style='margin-top:12px'><table>"
             "<tr><th>Build</th><th>Type</th><th>Version</th><th>Image CRC</th><th>File</th><th>Uploaded</th><th>Note</th><th></th></tr>")
    for b in builds:
        h.append(f"<tr><td>#{b['id']}</td><td>{esc(TYPES.get(b['profile'], '?'))}</td><td>{esc(b['version'])}</td>"
                 f"<td><code>{esc(b['image_crc'])}</code></td><td>{b['size'] // 1024} KB</td><td>{esc(when(b['uploaded_at']))}</td>"
                 f"<td>{esc(b['note'])}</td><td><form method='post' action='/build/delete' "
                 f"onsubmit=\"return confirm('Delete build #{b['id']}?')\"><input type='hidden' name='id' value='{b['id']}'>"
                 f"<button class='q'>Delete</button></form></td></tr>")
    if not builds:
        h.append("<tr><td colspan='8' class='mute'>No firmware uploaded yet.</td></tr>")
    h.append("</table></div></section>")

    h.append("<section><h2>Recent events</h2><div class='scroll'><table><tr><th>When</th><th>Station</th><th>What</th></tr>")
    for e in events:
        h.append(f"<tr><td>{esc(when(e['at']))}</td><td>{esc(e['station'])}</td><td class='w'>{esc(e['text'])}</td></tr>")
    if not events:
        h.append("<tr><td colspan='3' class='mute'>Nothing yet.</td></tr>")
    h.append("</table></div></section>")
    return page("LPC-AIO firmware updates", "".join(h))


def login_page(error=""):
    err = f"<p class='bad'>{esc(error)}</p>" if error else ""
    return page("Log in", f"<section class='login'><h2>Log in</h2>{err}<form method='post' action='/login'>"
                "<input name='user' placeholder='User' autocomplete='username' required>"
                "<input name='password' type='password' placeholder='Password' autocomplete='current-password' required>"
                "<button>Log in</button></form></section>", logged_in=False)


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
            return f"This firmware is already here as build #{same['id']}."
        cur = con.execute("INSERT INTO builds (profile, version, image_crc, file_crc, size, note, uploaded_at)"
                          " VALUES (?,?,?,?,?,?,?)", (profile, version, image_crc, file_crc, len(data), note, int(time.time())))
        build_id = cur.lastrowid
        tmp = os.path.join(BUILDS_DIR, f"{build_id}.tmp")
        with open(tmp, "wb") as f:
            f.write(data)
        os.replace(tmp, os.path.join(BUILDS_DIR, f"{build_id}.hex"))
        add_event(con, "", f"build #{build_id} uploaded: {TYPES[profile]} {version} ({image_crc}) {note}")
    return f"Uploaded as build #{build_id}: {TYPES[profile]} {version}, image CRC {image_crc}. No station gets it until you choose one."


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
        for k, v in headers:
            self.send_header(k, v)
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def redirect(self, where, headers=()):
        self.send(303, b"", headers=(("Location", where),) + tuple(headers))

    def logged_in(self):
        m = re.search(r"(?:^|;\s*)lpcota=([0-9a-f]{48})", self.headers.get("Cookie", ""))
        if not m:
            return False
        expires = SESSIONS.get(m.group(1), 0)
        return expires > time.time()

    def form(self):
        length = int(self.headers.get("Content-Length", "0") or 0)
        if length > MAX_UPLOAD:
            return None
        body = self.rfile.read(length)
        ctype = self.headers.get("Content-Type", "")
        if ctype.startswith("multipart/form-data"):
            return parse_multipart(ctype, body)
        return {k: v[0] for k, v in parse_qs(body.decode("utf-8", "replace")).items()}

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
            return self.redirect("/login", (("Set-Cookie", "lpcota=; Path=/; Max-Age=0"),))
        if not self.logged_in():
            return self.redirect("/login")
        if url.path == "/":
            return self.send(200, dashboard(q.get("m", "")))
        self.send(404, page("Not found", "<section>Not found. <a href='/'>Back</a></section>"))

    do_HEAD = do_GET

    def do_POST(self):
        url = urlparse(self.path)
        if url.path.startswith("/lpc/"):
            return self.send(404, b"OTA BAD\n", "text/plain")
        f = self.form()
        if f is None:
            return self.send(413, page("Too large", "<section>File too large. <a href='/'>Back</a></section>"))
        if url.path == "/login":
            ip = self.client_ip()
            count, since = FAILED_LOGINS.get(ip, (0, 0))
            if count >= 8 and time.time() - since < 900:
                return self.send(429, login_page("Too many wrong tries. Wait 15 minutes."))
            ok = bool(ADMIN_PASSWORD) and hmac.compare_digest(
                hashlib.sha256(f"{f.get('user', '')}\0{f.get('password', '')}".encode()).digest(),
                hashlib.sha256(f"{ADMIN_USER}\0{ADMIN_PASSWORD}".encode()).digest())
            if not ok:
                FAILED_LOGINS[ip] = (count + 1 if time.time() - since < 900 else 1, time.time())
                time.sleep(1)
                return self.send(401, login_page("Wrong user or password."))
            FAILED_LOGINS.pop(ip, None)
            token = secrets.token_hex(24)
            SESSIONS[token] = time.time() + 12 * 3600
            return self.redirect("/", (("Set-Cookie", f"lpcota={token}; Path=/; HttpOnly; SameSite=Strict; Max-Age=43200"),))
        if not self.logged_in():
            return self.redirect("/login")
        message = ""
        if url.path == "/upload":
            message = do_upload({k: (v if isinstance(v, bytes) else v.encode()) for k, v in f.items()})
        elif url.path == "/station":
            target = f.get("target", "type")
            if isinstance(target, bytes):
                target = target.decode()
            if target in ("type", "hold") or target.isdigit():
                with _db_lock, db() as con:
                    st = con.execute("SELECT * FROM stations WHERE key=?", (f.get("key", ""),)).fetchone()
                    if st:
                        con.execute("UPDATE stations SET target=?, result='', offered_crc=NULL WHERE key=?", (target, st["key"]))
                        add_event(con, st["shown_id"], f"office set: gets {'its type build' if target == 'type' else 'nothing (hold)' if target == 'hold' else 'build #' + target}")
                        message = f"Station {st['shown_id']} set. It acts on this at its next check."
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
                    message = f"{TYPES[int(f['profile'])]} set."
        elif url.path == "/build/delete":
            if f.get("id", "").isdigit():
                build_id = int(f["id"])
                with _db_lock, db() as con:
                    used = con.execute("SELECT 1 FROM type_targets WHERE build_id=? UNION SELECT 1 FROM stations WHERE target=?",
                                       (build_id, str(build_id))).fetchone()
                    if used:
                        message = f"Build #{build_id} is still set for a station or a type: change that first."
                    else:
                        con.execute("DELETE FROM builds WHERE id=?", (build_id,))
                        try:
                            os.remove(os.path.join(BUILDS_DIR, f"{build_id}.hex"))
                        except OSError:
                            pass
                        add_event(con, "", f"build #{build_id} deleted")
                        message = f"Build #{build_id} deleted."
        from urllib.parse import quote
        self.redirect("/?m=" + quote(message) if message else "/")


if __name__ == "__main__":
    init_db()
    if not ADMIN_PASSWORD:
        print("LPCOTA_PASSWORD is not set: nobody can log in to the office page")
    server = ThreadingHTTPServer((BIND, PORT), Handler)
    server.daemon_threads = True
    server.block_on_close = False
    server.serve_forever()
