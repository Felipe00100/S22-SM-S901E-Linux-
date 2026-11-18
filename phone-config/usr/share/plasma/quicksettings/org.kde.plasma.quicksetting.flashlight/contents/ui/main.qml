// SPDX-License-Identifier: LGPL-2.0-or-later
import QtQuick
import org.kde.plasma.private.mobileshell as MobileShell
import org.kde.plasma.private.mobileshell.quicksettingsplugin as QS

QS.QuickSetting {
    id: root
    text: i18n("Flashlight")
    icon: torchOn ? "flashlight-on" : "flashlight-off"
    status: torchOn ? "Ligada" : ""
    enabled: torchOn
    available: true

    property bool torchOn: false

    function refreshState() {
        const xhr = new XMLHttpRequest();
        xhr.open("GET", "file:///sys/class/leds/s2mpb02:white:flash/brightness", true);
        xhr.onreadystatechange = function() {
            if (xhr.readyState === XMLHttpRequest.DONE && xhr.responseText !== "") {
                const v = parseInt(xhr.responseText.trim(), 10);
                if (!isNaN(v)) {
                    root.torchOn = (v > 0);
                }
            }
        };
        xhr.send();
    }

    function toggle() {
        const next = !root.torchOn;
        root.torchOn = next;
        MobileShell.ShellUtil.executeCommand("/usr/local/bin/s22-flash " + (next ? "torch 12" : "off"));
        syncTimer.restart();
    }

    Timer {
        id: syncTimer
        interval: 350
        repeat: false
        onTriggered: root.refreshState()
    }

    Component.onCompleted: refreshState()
}
