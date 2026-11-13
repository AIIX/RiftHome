// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
import QtQuick

// The virtual keyboard: by itself on text fields, or only from the dock.
SettingsPage {
    title: "Keyboard"
    Card {
        SettingRow {
            label: "Show when a text field is tapped"
            detail: riftSettings.keyboardAuto ? "The keyboard opens by itself under the window you type into."
                                              : "Open it from the dock's keyboard button (with a physical keyboard)."
            separator: false
            Toggle {
                checked: riftSettings.keyboardAuto
                onToggled: on => riftSettings.keyboardAuto = on
            }
        }
    }
}
