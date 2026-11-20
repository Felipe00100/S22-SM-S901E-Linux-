#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 cammo1997 & contributors
#
# Configure Samsung Galaxy S22 (sm8450-samsung-r0q) idle battery & deep sleep:
#   1. Remove legacy bring-up AllowSuspend=no drop-ins (systemd & elogind)
#   2. Install /etc/xdg/powerdevilrc (deep sleep after 60s on Battery, 30s on
#      LowBattery, stay awake on AC/USB)
#   3. Install /usr/local/bin/s22-power-save + s22-power-save.service +
#      90-s22-pcie-aspm.rules for PCIe ASPM L1.1/L1.2 (powersupersave)
#
# Run as root on the phone:
#   sudo sh tools/s22-idle-power-setup.sh

set -eu

REPO="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
PC="$REPO/phone-config"

rm -f /etc/systemd/sleep.conf.d/90-s22-no-suspend.conf \
      /etc/elogind/sleep.conf.d/90-s22-no-suspend.conf

install -D -m 0644 "$PC/etc/xdg/powerdevilrc" /etc/xdg/powerdevilrc
install -D -m 0755 "$PC/usr/local/bin/s22-power-save" /usr/local/bin/s22-power-save
install -D -m 0644 "$PC/etc/udev/rules.d/90-s22-pcie-aspm.rules" /etc/udev/rules.d/90-s22-pcie-aspm.rules

if [ -d /run/systemd/system ] || command -v systemctl >/dev/null 2>&1; then
	install -D -m 0644 "$PC/etc/systemd/system/s22-power-save.service" /etc/systemd/system/s22-power-save.service
	systemctl daemon-reload 2>/dev/null || true
	systemctl enable --now s22-power-save.service 2>/dev/null || true
fi

if command -v udevadm >/dev/null 2>&1; then
	udevadm control --reload-rules 2>/dev/null || true
fi

/usr/local/bin/s22-power-save || true
echo "S22 idle power & deep sleep configuration installed."
