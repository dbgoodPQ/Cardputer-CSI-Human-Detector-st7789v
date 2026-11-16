#!/bin/sh
# Removes what install.sh added. sudo sh uninstall.sh
[ "$(id -u)" = 0 ] || { echo "Run as root: sudo sh $0"; exit 1; }

if [ -f /etc/systemd/system/lan-timeserver.service ]; then
  systemctl disable --now lan-timeserver
  rm -f /etc/systemd/system/lan-timeserver.service
  rm -rf /opt/lan-timeserver
  systemctl daemon-reload
  echo "lan-timeserver service removed (re-enable chrony/ntpd yourself if install.sh stopped them)"
fi

for CONF in /etc/chrony/chrony.conf /etc/chrony.conf; do
  if [ -f "$CONF" ] && grep -q "# >>> lan-timeserver" "$CONF"; then
    sed -i '/# >>> lan-timeserver/,/# <<< lan-timeserver/d' "$CONF"
    systemctl restart chrony 2>/dev/null || systemctl restart chronyd 2>/dev/null
    echo "chrony block removed from $CONF"
  fi
done
echo "Firewall rule for UDP 123 left in place (ufw delete allow 123/udp to remove)."
