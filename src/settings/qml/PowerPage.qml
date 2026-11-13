// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
import QtQuick

// Leaving: Rift Home itself, or the computer (each asks first); and whose data the apps use.
SettingsPage {
    id: page
    title: "Power"
    property string action
    property string question
    property string explanation

    function ask(a, q, e) {
        page.action = a
        page.question = q
        page.explanation = e
        confirm.open()
    }

    Card {
        heading: "Apps"
        SettingRow {
            // Apps otherwise start without the user's desktop data (isolated on purpose).
            label: "Use my own app data"
            detail: riftSettings.ownAppData ? "Apps you start now use your own settings and profiles (Firefox's bookmarks, logins). Close an app on the desktop first: two can't share a profile."
                                            : "Apps use Rift Home's own settings, apart from your desktop's."
            Toggle {
                checked: riftSettings.ownAppData
                anchors.verticalCenter: parent.verticalCenter
                onToggled: on => riftSettings.ownAppData = on
            }
        }
        SettingRow {
            label: "Reopen my apps"
            detail: "The apps you opened from Home that were open when Rift Home ended open again when it starts."
            separator: false
            Toggle {
                checked: riftSettings.reopenApps
                anchors.verticalCenter: parent.verticalCenter
                onToggled: on => riftSettings.reopenApps = on
            }
        }
    }

    Card {
        heading: "Session"
        SettingRow {
            label: "Leave Rift Home"
            detail: "Closes Rift Home and the apps in it."
            PillButton {
                text: "Leave"
                anchors.verticalCenter: parent.verticalCenter
                onClicked: page.ask("leave", "Leave Rift Home?", "Your apps in Rift Home close. Window places are kept.")
            }
        }
        SettingRow {
            label: "Restart"
            detail: "Restarts the computer."
            PillButton {
                text: "Restart"
                anchors.verticalCenter: parent.verticalCenter
                onClicked: page.ask("restart", "Restart the computer?", "Everything running on it closes, also outside Rift Home.")
            }
        }
        SettingRow {
            label: "Shut down"
            detail: "Turns the computer off."
            separator: false
            PillButton {
                text: "Shut down"
                destructive: true
                anchors.verticalCenter: parent.verticalCenter
                onClicked: page.ask("shutdown", "Shut down the computer?", "Everything running on it closes, also outside Rift Home.")
            }
        }
    }

    Sheet {
        id: confirm
        title: page.question
        message: page.explanation
        Row {
            spacing: 14
            PillButton {
                text: page.action === "leave" ? "Leave" : page.action === "restart" ? "Restart" : "Shut down"
                destructive: true
                onClicked: {
                    confirm.close()
                    if (page.action === "leave") system.leaveRiftHome()
                    else if (page.action === "restart") system.restart()
                    else system.shutDown()
                }
            }
            PillButton {
                text: "Cancel"
                onClicked: confirm.close()
            }
        }
    }
}
