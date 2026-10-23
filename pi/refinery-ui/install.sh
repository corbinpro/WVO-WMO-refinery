#!/bin/sh
# Build and install refinery-ui on the RPi, replacing the old Python bridge (test_code/pi_bridge).
# Run from this directory: sudo ./install.sh
set -e
cd "$(dirname "$0")"

apt-get install -y -q libjpeg62-turbo-dev zlib1g-dev >/dev/null
sudo -u pi make
install -m 755 refinery-ui /usr/local/bin/refinery-ui
install -m 644 refinery-ui.service /etc/systemd/system/

# The old bridge owned /dev/serial0 and tty1
systemctl disable --now refinery-bridge.service refinery-screen.service 2>/dev/null || true
sed -i '/# refinery console autostart/,/^fi$/d' /home/pi/.bashrc

# Keep the screen from blanking (takes effect after a reboot)
grep -q consoleblank=0 /boot/firmware/cmdline.txt || sed -i '1 s/$/ consoleblank=0/' /boot/firmware/cmdline.txt

systemctl daemon-reload
systemctl disable getty@tty1.service 2>/dev/null || true
systemctl enable --now refinery-ui.service
systemctl restart refinery-ui.service

# Publish on the tailnet over HTTPS: / -> refinery-ui, /video -> ustreamer
tailscale serve --bg --set-path / http://127.0.0.1:8000
tailscale serve --bg --set-path /video http://127.0.0.1:8080
tailscale serve status
echo "Installed."
