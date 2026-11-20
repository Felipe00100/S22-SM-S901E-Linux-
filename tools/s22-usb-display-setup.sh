#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 cammo1997 & contributors
#
# Install Samsung Galaxy S22 (sm8450-samsung-r0q) userspace USB-C mode
# notification helper (`usb-moded-notify`, skips device-mode popups while
# operating in USB host / DisplayPort Alt-Mode role).
#
# Run as root on the phone:
#   sudo sh tools/s22-usb-display-setup.sh

set -eu

REPO="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
PC="$REPO/phone-config"

if [ -f /usr/bin/usb-moded-notify ] && [ ! -f /usr/bin/usb-moded-notify.orig ]; then
	cp -a /usr/bin/usb-moded-notify /usr/bin/usb-moded-notify.orig
fi
install -D -m 0755 "$PC/usr/bin/usb-moded-notify" /usr/bin/usb-moded-notify

echo "S22 USB/DisplayPort userspace helper installed (/usr/bin/usb-moded-notify)."
