#!/bin/sh
# Install the refinery serial bridge on the RPi. Run from this directory: sudo ./install.sh
# Reboot afterwards if it says the serial console was disabled.
set -e
cd "$(dirname "$0")"

# The UART is wired to the STM32: no login shell or kernel messages on it.
if grep -q "console=serial0" /boot/firmware/cmdline.txt || systemctl is-enabled -q serial-getty@ttyAMA0 2>/dev/null; then
    raspi-config nonint do_serial_cons 1   # disable serial console
    raspi-config nonint do_serial_hw 0     # keep the UART hardware enabled
    systemctl disable --now serial-getty@ttyAMA0.service 2>/dev/null || true
    echo "Serial console disabled - REBOOT REQUIRED"
fi

install -m 755 refinery-bridge refinery /usr/local/bin/
install -m 644 refinery-bridge.service refinery-screen.service /etc/systemd/system/
systemctl daemon-reload
systemctl disable getty@tty1.service 2>/dev/null || true
systemctl enable --now refinery-bridge.service
systemctl enable refinery-screen.service
systemctl restart refinery-screen.service

# Interactive SSH logins as pi drop straight into the console (Ctrl-] for a shell).
BASHRC=/home/pi/.bashrc
if ! grep -q "refinery console autostart" "$BASHRC"; then
    cat >> "$BASHRC" <<'EOF'

# refinery console autostart (SSH only; Ctrl-] detaches to this shell)
if [ -n "$SSH_TTY" ] && [ -z "$REFINERY_NO_AUTO" ] && [ -z "$TMUX$STY" ]; then
    echo "Refinery console - press Ctrl-] for a shell, type 'refinery' to reattach."
    sleep 1
    refinery
fi
EOF
fi
echo "Installed."
