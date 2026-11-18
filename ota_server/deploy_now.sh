#!/bin/bash
# Updates the LPC office page / OTA server (lpc-ota) from GitHub. Run as root on the Hostinger server:
#
#   ./deploy_now.sh            newest code on the branch below
#   ./deploy_now.sh 63fc70f    a given commit
#   ./deploy_now.sh back       undo the last update
#
# Touches only /opt/lpc-ota/lpc_ota.py (and its copy lpc_ota.py.before) and restarts only the lpc-ota
# service. nginx, the health app, the BIH dashboard, other directories and the database are not touched
# (the database gains any new columns by itself when the service starts).
set -euo pipefail

REPO="satishvspatika/AIO_LPC"
BRANCH="v6.2.9-untested"
APP="/opt/lpc-ota/lpc_ota.py"
SERVICE="lpc-ota"

[ "$(id -u)" -eq 0 ] || { echo "Run as root"; exit 1; }
[ -f "$APP" ] || { echo "$APP not found: is this the right server?"; exit 1; }
systemctl cat "$SERVICE" > /dev/null 2>&1 || { echo "No $SERVICE service here"; exit 1; }

restart() {
    systemctl restart "$SERVICE"
    sleep 2
    if systemctl is-active --quiet "$SERVICE"; then
        echo "$SERVICE is running"
    else
        echo "$SERVICE did NOT start:"; journalctl -u "$SERVICE" -n 20 --no-pager; return 1
    fi
}

if [ "${1:-}" = "back" ]; then
    [ -f "$APP.before" ] || { echo "No earlier copy ($APP.before)"; exit 1; }
    install -o lpcota -g lpcota -m 644 "$APP.before" "$APP"
    echo "Put back the copy from before the last update"
    restart
    exit
fi

# The commit to fetch: the one given, or the branch's newest (asked of GitHub, as raw files of a branch
# name can be a few minutes old)
REF="${1:-}"
if [ -z "$REF" ]; then
    REF=$(curl -fsSL -H "Accept: application/vnd.github.sha" "https://api.github.com/repos/$REPO/commits/$BRANCH")
fi
[[ "$REF" =~ ^[0-9a-f]{7,40}$ ]] || { echo "Not a commit: $REF"; exit 1; }

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
curl -fsSL -o "$TMP/lpc_ota.py" "https://raw.githubusercontent.com/$REPO/$REF/ota_server/lpc_ota.py"
python3 -m py_compile "$TMP/lpc_ota.py"
echo "Commit ${REF:0:7}, sha256 $(sha256sum "$TMP/lpc_ota.py" | cut -c1-16)"

if cmp -s "$TMP/lpc_ota.py" "$APP"; then
    echo "Already running this code: nothing changed"
    exit
fi
cp -p "$APP" "$APP.before"
install -o lpcota -g lpcota -m 644 "$TMP/lpc_ota.py" "$APP"
echo "Installed (old copy kept as $APP.before; './deploy_now.sh back' puts it back)"
restart
