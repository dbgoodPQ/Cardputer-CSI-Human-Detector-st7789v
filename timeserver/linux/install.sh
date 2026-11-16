#!/bin/sh
# LAN time server installer for Linux / Raspberry Pi (offline, nothing downloaded).
#   sudo sh install.sh            chrony if it is installed, else the Python server
#   sudo sh install.sh --python   always the Python server (stops chrony/ntpd if they use port 123)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
MARK_BEGIN="# >>> lan-timeserver"
MARK_END="# <<< lan-timeserver"

[ "$(id -u)" = 0 ] || { echo "Run as root: sudo sh $0"; exit 1; }

open_firewall() {
  if command -v ufw >/dev/null 2>&1 && ufw status 2>/dev/null | grep -q "Status: active"; then
    ufw allow 123/udp && echo "ufw: UDP 123 opened"
  fi
  if command -v firewall-cmd >/dev/null 2>&1 && firewall-cmd --state >/dev/null 2>&1; then
    firewall-cmd --permanent --add-service=ntp && firewall-cmd --reload && echo "firewalld: ntp opened"
  fi
}

chrony_conf() {
  for f in /etc/chrony/chrony.conf /etc/chrony.conf; do [ -f "$f" ] && { echo "$f"; return; }; done
}

install_chrony() {
  CONF=$(chrony_conf)
  [ -n "$CONF" ] || { echo "chrony config not found"; return 1; }
  if ! grep -q "$MARK_BEGIN" "$CONF"; then
    cp "$CONF" "$CONF.before-lan-timeserver"
    cat >> "$CONF" <<EOF

$MARK_BEGIN
# Serve this machine's clock to the LAN even with no internet source.
local stratum 8
allow all
$MARK_END
EOF
  fi
  SVC=chrony; systemctl list-unit-files | grep -q '^chronyd\.service' && SVC=chronyd
  systemctl enable "$SVC" >/dev/null 2>&1 || true
  systemctl restart "$SVC"
  echo "chrony configured ($CONF) and restarted ($SVC)"
}

install_python() {
  command -v python3 >/dev/null 2>&1 || { echo "python3 not found: install chrony or python3 from your OS media"; exit 1; }
  for s in chrony chronyd ntp ntpd ntpsec; do
    if systemctl is-active --quiet "$s" 2>/dev/null; then
      echo "stopping $s (it holds port 123)"; systemctl disable --now "$s"
    fi
  done
  install -d /opt/lan-timeserver
  install -m 755 "$HERE/ntp_server.py" /opt/lan-timeserver/ntp_server.py
  install -m 644 "$HERE/lan-timeserver.service" /etc/systemd/system/lan-timeserver.service
  systemctl daemon-reload
  systemctl enable --now lan-timeserver
  systemctl restart lan-timeserver
  echo "lan-timeserver service installed and started"
}

if [ "$1" != "--python" ] && command -v chronyd >/dev/null 2>&1; then
  install_chrony
else
  install_python
fi
open_firewall

# This box keeps no internet sync: stop systemd-timesyncd retrying, its clock is the reference.
systemctl is-active --quiet systemd-timesyncd 2>/dev/null && timedatectl set-ntp false 2>/dev/null || true

sleep 1
echo
echo "Self test:"
python3 "$HERE/ntp_server.py" --query 127.0.0.1 || echo "(self test failed, see: journalctl -u lan-timeserver / chrony)"
echo
echo "Server IP address(es) to give to clients:"
hostname -I 2>/dev/null || ip -4 addr show | grep inet
echo
echo "This machine's clock is the reference for the whole LAN:"
timedatectl 2>/dev/null | grep -E 'Local time|Time zone' || date
echo "Fix the time zone:  sudo timedatectl set-timezone America/New_York"
echo "Fix the time:       sudo timedatectl set-time '2026-11-16 14:30:00'   (local time)"
echo "  if refused (chrony mode): sudo date -s '2026-11-16 14:30:00' && sudo hwclock --systohc"
echo "  (don't run 'timedatectl set-ntp false' in chrony mode: it would stop the time server)"
