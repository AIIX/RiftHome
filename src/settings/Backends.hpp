// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QFileSystemWatcher>
#include <QJsonObject>
#include <QObject>
#include <QProcess>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

#include <functional>

namespace RiftHome::Settings
{

/*!
 * The settings app's view of the system: each page's data and actions, through the distro's own tools
 * (nmcli, pactl, timedatectl, systemctl), run without blocking. Reading never changes anything; changes happen only
 * from the user's own taps.
 */

//! Runs @p program with @p arguments; @p done gets its exit code, its output and its errors.
void run(const QString &program, const QStringList &arguments, std::function<void(int, const QString &, const QString &)> done);

//! Wi-Fi and wired networks through NetworkManager's nmcli.
class Network : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool loaded READ loaded NOTIFY changed)
    Q_PROPERTY(bool available READ available NOTIFY changed)
    Q_PROPERTY(bool wifiHardware READ wifiHardware NOTIFY changed)
    Q_PROPERTY(bool wifiEnabled READ wifiEnabled NOTIFY changed)
    Q_PROPERTY(QVariantList wifiNetworks READ wifiNetworks NOTIFY changed)
    Q_PROPERTY(QVariantList devices READ devices NOTIFY changed)
    Q_PROPERTY(QVariantList vpns READ vpns NOTIFY changed)
    Q_PROPERTY(QString busy READ busy NOTIFY changed)
    Q_PROPERTY(QString message READ message NOTIFY changed)
    Q_PROPERTY(QVariantMap details READ details NOTIFY detailsChanged)

public:
    using QObject::QObject;
    //! NetworkManager answered at least once (until then nothing is said about it).
    bool loaded() const
    {
        return m_loaded;
    }
    bool available() const
    {
        return m_available;
    }
    bool wifiHardware() const
    {
        return m_wifiHardware;
    }
    bool wifiEnabled() const
    {
        return m_wifiEnabled;
    }
    QVariantList wifiNetworks() const
    {
        return m_wifiNetworks;
    }
    QVariantList devices() const
    {
        return m_devices;
    }
    QVariantList vpns() const
    {
        return m_vpns;
    }
    QString busy() const
    {
        return m_busy;
    }
    QString message() const
    {
        return m_message;
    }

    //! Reads it all again; @p rescan: the Wi-Fi list from a fresh scan (when the page opens).
    Q_INVOKABLE void refresh(bool rescan = false);
    Q_INVOKABLE void setWifiEnabled(bool on);
    //! Joins @p ssid (a known network: its saved password; an open one: none needed).
    Q_INVOKABLE void join(const QString &ssid, const QString &password);
    Q_INVOKABLE void disconnectWifi();
    //! A VPN on or off (by its NetworkManager uuid).
    Q_INVOKABLE void setVpn(const QString &uuid, bool on);
    //! Deletes the saved network named @p ssid (its password with it).
    Q_INVOKABLE void forget(const QString &ssid);
    QVariantMap details() const
    {
        return m_details;
    }
    /*!
     * A connection's IPv4 settings (Ethernet needs settings too, to change DNS or set a manual
     * IP): how it gets its address (automatic or manual: address, prefix, gateway), its DNS servers, and what the
     * device has now.
     */
    Q_INVOKABLE void loadDetails(const QString &connection, const QString &device);
    /*!
     * Changes @p connection's IPv4 settings and reconnects it. Returns what is wrong with the entries (nothing changed),
     * else an empty string (the outcome then comes as message()).
     */
    Q_INVOKABLE QString applyDetails(const QString &connection, bool manual, const QString &address, int prefix, const QString &gateway,
                                     bool automaticDns, const QString &dns);

Q_SIGNALS:
    void changed();
    void detailsChanged();

private:
    void setMessage(const QString &message);
    QVariantMap m_details;
    bool m_loaded = false;
    bool m_available = false;
    bool m_wifiHardware = false;
    bool m_wifiEnabled = false;
    QVariantList m_wifiNetworks;
    QVariantList m_devices;
    QVariantList m_vpns;
    QStringList m_known; //!< saved Wi-Fi connections' names
    QString m_wifiDevice;
    QString m_busy;
    QString m_message;
};

//! Bluetooth through BlueZ's bluetoothctl: power, the known devices (connect, disconnect, forget), a search to pair.
class Bluetooth : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool loaded READ loaded NOTIFY changed)
    Q_PROPERTY(bool available READ available NOTIFY changed)
    Q_PROPERTY(bool powered READ powered NOTIFY changed)
    Q_PROPERTY(bool scanning READ scanning NOTIFY changed)
    Q_PROPERTY(QVariantList devices READ devices NOTIFY changed)
    Q_PROPERTY(QString busy READ busy NOTIFY changed)
    Q_PROPERTY(QString message READ message NOTIFY changed)

public:
    using QObject::QObject;
    bool loaded() const
    {
        return m_loaded;
    }
    bool available() const
    {
        return m_available;
    }
    bool powered() const
    {
        return m_powered;
    }
    bool scanning() const
    {
        return m_scanning;
    }
    QVariantList devices() const
    {
        return m_devices;
    }
    QString busy() const
    {
        return m_busy;
    }
    QString message() const
    {
        return m_message;
    }

    Q_INVOKABLE void refresh();
    Q_INVOKABLE void setPowered(bool on);
    //! Looks for devices nearby for 12 s (they appear as they are found).
    Q_INVOKABLE void scan();
    Q_INVOKABLE void connectDevice(const QString &address);
    Q_INVOKABLE void disconnectDevice(const QString &address);
    //! Pairs, trusts and connects (devices that need no code: headphones, controllers, speakers).
    Q_INVOKABLE void pair(const QString &address);
    Q_INVOKABLE void forget(const QString &address);

Q_SIGNALS:
    void changed();

private:
    void act(const QString &address, const QStringList &arguments, const QString &doing, std::function<void(bool)> then = {});
    bool m_loaded = false;
    bool m_available = false;
    bool m_powered = false;
    bool m_scanning = false;
    QVariantList m_devices;
    QString m_busy;
    QString m_message;
};

//! Outputs and inputs through PipeWire (pactl's JSON; the runtime makes the headset the default output).
class Sound : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantList outputs READ outputs NOTIFY changed)
    Q_PROPERTY(QVariantList inputs READ inputs NOTIFY changed)
    Q_PROPERTY(QString defaultOutput READ defaultOutput NOTIFY changed)
    Q_PROPERTY(QString defaultInput READ defaultInput NOTIFY changed)

public:
    using QObject::QObject;
    QVariantList outputs() const
    {
        return m_outputs;
    }
    QVariantList inputs() const
    {
        return m_inputs;
    }
    QString defaultOutput() const
    {
        return m_defaultOutput;
    }
    QString defaultInput() const
    {
        return m_defaultInput;
    }

    Q_INVOKABLE void refresh();
    Q_INVOKABLE void setDefault(bool output, const QString &name);
    Q_INVOKABLE void setVolume(bool output, const QString &name, int percent);
    Q_INVOKABLE void setMuted(bool output, const QString &name, bool muted);

Q_SIGNALS:
    void changed();

private:
    QVariantList m_outputs;
    QVariantList m_inputs;
    QString m_defaultOutput;
    QString m_defaultInput;
};

//! Time zone and automatic time through systemd's timedatectl (changes ask for an administrator's password).
class DateTime : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString timeZone READ timeZone NOTIFY changed)
    Q_PROPERTY(bool automatic READ automatic NOTIFY changed)
    Q_PROPERTY(bool synchronized READ synchronized NOTIFY changed)
    Q_PROPERTY(QStringList timeZones READ timeZones NOTIFY timeZonesChanged)
    Q_PROPERTY(QString message READ message NOTIFY changed)

public:
    using QObject::QObject;
    QString timeZone() const
    {
        return m_timeZone;
    }
    bool automatic() const
    {
        return m_automatic;
    }
    bool synchronized() const
    {
        return m_synchronized;
    }
    QStringList timeZones() const
    {
        return m_timeZones;
    }
    QString message() const
    {
        return m_message;
    }

    Q_INVOKABLE void refresh();
    Q_INVOKABLE void loadTimeZones();
    Q_INVOKABLE void setTimeZone(const QString &zone);
    Q_INVOKABLE void setAutomatic(bool on);

Q_SIGNALS:
    void changed();
    void timeZonesChanged();

private:
    QString m_timeZone;
    bool m_automatic = false;
    bool m_synchronized = false;
    QStringList m_timeZones;
    QString m_message;
};

/*!
 * Rift Home's own settings: `<state>/settings.json` (RIFT_HOME_STATE_DIR, given by Rift Home), which Rift Home watches
 * and applies at once. Other keys in the file are kept.
 */
class RiftSettings : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString frameTheme READ frameTheme WRITE setFrameTheme NOTIFY changed)
    Q_PROPERTY(bool keyboardAuto READ keyboardAuto WRITE setKeyboardAuto NOTIFY changed)
    Q_PROPERTY(QString roomPhoto READ roomPhoto WRITE setRoomPhoto NOTIFY changed)
    Q_PROPERTY(double roomBrightness READ roomBrightness WRITE setRoomBrightness NOTIFY changed)
    Q_PROPERTY(double windowCurve READ windowCurve WRITE setWindowCurve NOTIFY changed)
    /*!
     * What the controllers' buttons do, a row per action as in KDE's Shortcuts: {action, label, defaultText, custom (differs
     * from the default), bindings: [{name, label}]}.
     */
    Q_PROPERTY(QVariantList controlRows READ controlRows NOTIFY changed)
    //! The action whose binding Rift Home is recording (its name), empty when none; a note on the last one (moved from, failed).
    Q_PROPERTY(QString recordingAction READ recordingAction NOTIFY recordingChanged)
    Q_PROPERTY(QString recordNote READ recordNote NOTIFY recordingChanged)
    //! Why the lens test did not show (Rift Home not running in the headset), empty when it did.
    Q_PROPERTY(QString lensTestNote READ lensTestNote NOTIFY lensTestNoteChanged)
    Q_PROPERTY(bool use24Hour READ use24Hour WRITE setUse24Hour NOTIFY changed)
    //! The room while a film plays in cinema mode: "dark" (off), "dim" (15 %), "soft" (40 %).
    Q_PROPERTY(QString cinemaRoom READ cinemaRoom WRITE setCinemaRoom NOTIFY changed)
    //! How far a 360-degree room is turned, degrees (-180..180; positive: to the right).
    Q_PROPERTY(int roomTurn READ roomTurn WRITE setRoomTurn NOTIFY changed)
    //! The controllers' rays: "ice" (default), "white", "green", "amber", "pink"; "thin", "normal", "thick"; hidden after
    //! so many seconds without movement (0: never).
    Q_PROPERTY(QString pointerColour READ pointerColour WRITE setPointerColour NOTIFY changed)
    Q_PROPERTY(QString pointerThickness READ pointerThickness WRITE setPointerThickness NOTIFY changed)
    Q_PROPERTY(int pointerHideAfter READ pointerHideAfter WRITE setPointerHideAfter NOTIFY changed)
    Q_PROPERTY(double x11Scale READ x11Scale WRITE setX11Scale NOTIFY changed)
    Q_PROPERTY(double appScale READ appScale WRITE setAppScale NOTIFY changed)
    Q_PROPERTY(int composition READ composition WRITE setComposition NOTIFY changed)
    //! Apps started in Rift Home use the user's own settings and profiles (else Rift Home's separate folder).
    Q_PROPERTY(bool ownAppData READ ownAppData WRITE setOwnAppData NOTIFY changed)
    //! The apps opened from Home that were open when Rift Home ended open again when it starts.
    Q_PROPERTY(bool reopenApps READ reopenApps WRITE setReopenApps NOTIFY changed)
    Q_PROPERTY(QVariantList photos READ photos NOTIFY photosChanged)
    Q_PROPERTY(QString userRoomsName READ userRoomsName CONSTANT)
    Q_PROPERTY(QString file READ file CONSTANT)

public:
    explicit RiftSettings(QObject *parent = nullptr);
    QString frameTheme() const;
    bool keyboardAuto() const;
    QString roomPhoto() const;
    double roomBrightness() const;
    double windowCurve() const;
    QVariantList controlRows() const;
    QString recordingAction() const
    {
        return m_recordingAction;
    }
    QString recordNote() const
    {
        return m_recordNote;
    }
    //! Asks Rift Home (D-Bus) to record the next button, or two together, pressed in the headset and adds it to @p action.
    Q_INVOKABLE void recordBinding(const QString &action);
    Q_INVOKABLE void cancelRecording();
    QString lensTestNote() const
    {
        return m_lensTestNote;
    }
    //! Asks Rift Home (D-Bus) to show the lens test in the headset until a button is pressed.
    Q_INVOKABLE void showLensTest();
    //! @p action without bindings, and back to its defaults; every action back to its defaults.
    Q_INVOKABLE void clearControl(const QString &action);
    Q_INVOKABLE void resetControl(const QString &action);
    Q_INVOKABLE void resetControls();
    bool use24Hour() const;
    QString cinemaRoom() const;
    int roomTurn() const;
    QString pointerColour() const;
    QString pointerThickness() const;
    int pointerHideAfter() const;
    double x11Scale() const;
    double appScale() const; //!< apps.scale: how big apps draw (HiDPI), 1-3, default 2
    int composition() const; //!< display.composition: the runtime's picture, percent of the panel's pixels a side (165-200)
    bool ownAppData() const;
    bool reopenApps() const;
    QVariantList photos() const
    {
        return m_photos;
    }
    QString file() const
    {
        return m_file;
    }
    void setFrameTheme(const QString &theme);
    void setKeyboardAuto(bool on);
    void setRoomPhoto(const QString &path);
    void setRoomBrightness(double brightness);
    void setWindowCurve(double curve);
    void setUse24Hour(bool on);
    void setCinemaRoom(const QString &room);
    void setRoomTurn(int degrees);
    void setPointerColour(const QString &colour);
    void setPointerThickness(const QString &thickness);
    void setPointerHideAfter(int seconds);
    void setX11Scale(double scale);
    void setAppScale(double scale);
    void setComposition(int percent);
    void setOwnAppData(bool on);
    void setReopenApps(bool on);

    //! The user's rooms folder as shown ("Pictures/RiftHome").
    QString userRoomsName() const;

    /*!
     * Rooms to choose from, each with its group (folders of their own, as desktops have for wallpapers):
     * "riftHome" Rift Home's own (RIFT_HOME_ROOMS: the photo rooms, the HDR panoramas), "yours" the user's
     * (Pictures/RiftHome, followed while open; the room in use when it is anywhere else), "wallpapers" the desktop's
     * (Plasma's and GNOME's wallpaper folders: Plasma's packages as one picture each).
     */
    Q_INVOKABLE void findPhotos();
    //! Opens the user's rooms folder in the file manager, made first if missing.
    Q_INVOKABLE void openUserRooms();

Q_SIGNALS:
    void changed();
    void photosChanged();
    void recordingChanged();
    void lensTestNoteChanged();

private Q_SLOTS:
    void onBindingRecorded(const QString &binding);

private:
    void load();
    void saveControls(const QJsonValue &controls);
    void endRecording(const QString &note);
    void set(const QString &group, const QString &key, const QJsonValue &value);
    QString m_file;
    QJsonObject m_settings;
    QString userRoomsFolder() const;
    QVariantList m_photos;
    QString m_recordingAction;
    QString m_recordNote;
    QString m_lensTestNote;
    QTimer m_recordTimer; //!< Rift Home ends a recording after 10 s: this one after a little more, when it says nothing
    QFileSystemWatcher m_watcher;
    QFileSystemWatcher m_roomsWatcher; //!< the user's rooms folder: pictures put there show at once
};

//! About this system, and leaving: Rift Home itself, restart, shut down.
class System : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantList about READ about NOTIFY changed)

public:
    explicit System(QObject *parent = nullptr);
    QVariantList about() const
    {
        return m_about;
    }
    //! Rift Home ends (its apps with it): SIGTERM to RIFT_HOME_PID.
    Q_INVOKABLE void leaveRiftHome();
    Q_INVOKABLE void restart();
    Q_INVOKABLE void shutDown();

Q_SIGNALS:
    void changed();

private:
    QVariantList m_about;
};

} // namespace RiftHome::Settings
