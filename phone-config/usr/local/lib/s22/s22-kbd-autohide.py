#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Auto-hide the Plasma Mobile virtual keyboard when tapping above its panel.

Opens the FTS2BA61Y touchscreen evdev read-only (non-grabbing), drops privileges
to the active Wayland user so dbus-broker accepts session bus calls, and hides
org.kde.KWin /VirtualKeyboard when a touch ends above KEYBOARD_TOP_FRACTION.
"""
import fcntl
import glob
import os
import shutil
import struct
import subprocess
import time

EV_SYN = 0
EV_KEY = 1
EV_ABS = 3
BTN_TOUCH = 0x14A
ABS_Y = 0x01
ABS_MT_POSITION_Y = 0x36  # 0x36 = Y (0x35 is X!)
ABS_MT_TRACKING_ID = 0x39
KEYBOARD_TOP_FRACTION = 0.56
DBUS_TIMEOUT = 1.5


def read_text(path):
    try:
        with open(path, "r", encoding="utf-8", errors="ignore") as f:
            return f.read().strip()
    except OSError:
        return ""


def find_touchscreen():
    for event in sorted(glob.glob("/dev/input/event*")):
        name = read_text(f"/sys/class/input/{os.path.basename(event)}/device/name").lower()
        if "fts2ba61y" in name or name.startswith("fts") or "touchscreen" in name:
            return event
    return None


def wait_for_session_runtime():
    while True:
        for runtime in sorted(glob.glob("/run/user/[0-9]*")):
            if os.path.exists(os.path.join(runtime, "bus")) and glob.glob(os.path.join(runtime, "wayland-[0-9]*")):
                return runtime
        time.sleep(2.0)


def axis_max(fd, code):
    try:
        buf = bytearray(24)
        fcntl.ioctl(fd, 0x80184540 + code, buf, True)
        _val, _min, maximum, _fuzz, _flat, _res = struct.unpack("6i", buf)
        return maximum if maximum > 0 else None
    except Exception:
        return None


def kwin_visible(env):
    try:
        r = subprocess.run(
            ["qdbus6", "org.kde.KWin", "/VirtualKeyboard",
             "org.freedesktop.DBus.Properties.Get",
             "org.kde.kwin.VirtualKeyboard", "visible"],
            env=env, capture_output=True, text=True, timeout=DBUS_TIMEOUT,
        )
        return r.returncode == 0 and r.stdout.strip() == "true"
    except Exception:
        return False


def kwin_set_active(env, active):
    try:
        return subprocess.run(
            ["qdbus6", "org.kde.KWin", "/VirtualKeyboard",
             "org.freedesktop.DBus.Properties.Set",
             "org.kde.kwin.VirtualKeyboard", "active",
             "true" if active else "false"],
            env=env, capture_output=True, text=True, timeout=DBUS_TIMEOUT,
        ).returncode == 0
    except Exception:
        return False


def kwin_hide(env):
    if kwin_set_active(env, False):
        time.sleep(0.12)
        kwin_set_active(env, True)


def main():
    if not shutil.which("qdbus6"):
        raise SystemExit("qdbus6 not found")
    event = find_touchscreen()
    while not event:
        time.sleep(2.0)
        event = find_touchscreen()

    fd = os.open(event, os.O_RDONLY)
    max_y = axis_max(fd, ABS_MT_POSITION_Y) or axis_max(fd, ABS_Y) or 4095
    threshold = int(max_y * KEYBOARD_TOP_FRACTION)

    runtime = wait_for_session_runtime()
    st = os.stat(runtime)
    if os.geteuid() == 0 and st.st_uid != 0:
        os.setgid(st.st_gid)
        os.setuid(st.st_uid)

    env = os.environ.copy()
    env["XDG_RUNTIME_DIR"] = runtime
    env["DBUS_SESSION_BUS_ADDRESS"] = f"unix:path={runtime}/bus"
    sockets = sorted(glob.glob(os.path.join(runtime, "wayland-[0-9]*")))
    if sockets:
        env["WAYLAND_DISPLAY"] = os.path.basename(sockets[0])

    print(f"s22-kbd-autohide active: {event} max_y={max_y} threshold={threshold} uid={os.geteuid()}", flush=True)

    last_y = None
    while True:
        data = os.read(fd, 24)
        if len(data) < 24:
            continue
        _sec, _usec, etype, code, value = struct.unpack("llHHi", data)
        if etype == EV_ABS:
            if code in (ABS_Y, ABS_MT_POSITION_Y):
                last_y = value
            elif code == ABS_MT_TRACKING_ID and value < 0:
                if last_y is not None and last_y < threshold and kwin_visible(env):
                    kwin_hide(env)
                last_y = None
        elif etype == EV_KEY and code == BTN_TOUCH and value == 0:
            if last_y is not None and last_y < threshold and kwin_visible(env):
                kwin_hide(env)
            last_y = None


if __name__ == "__main__":
    main()
