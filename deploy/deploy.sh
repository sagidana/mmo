#!/bin/sh
# deploys the gridmmo server to a debian/ubuntu host over ssh.
# usage: deploy/deploy.sh root@<server-ip>
# idempotent: run it again to ship updates.
set -e

target="$1"
if [ -z "$target" ]; then
    echo "usage: $0 root@<server-ip>"
    exit 1
fi

cd "$(dirname "$0")/.."

echo "[+] uploading server code"
tar czf - server deploy protocol.md README.md | ssh "$target" '
    mkdir -p /opt/gridmmo && tar xzf - -C /opt/gridmmo
'

echo "[+] installing on the host"
ssh "$target" '
    set -e
    apt-get update -qq
    apt-get install -y -qq python3-venv >/dev/null

    id gridmmo >/dev/null 2>&1 || useradd -r -s /usr/sbin/nologin gridmmo
    mkdir -p /var/lib/gridmmo
    chown gridmmo:gridmmo /var/lib/gridmmo

    cd /opt/gridmmo
    [ -d venv ] || python3 -m venv venv
    venv/bin/pip install -q -r server/requirements.txt

    cp deploy/gridmmo.service /etc/systemd/system/gridmmo.service
    systemctl daemon-reload
    systemctl enable gridmmo >/dev/null 2>&1
    systemctl restart gridmmo
    sleep 1
    systemctl --no-pager --lines=5 status gridmmo
'

echo "[+] deployed. logs: ssh $target journalctl -u gridmmo -f"
