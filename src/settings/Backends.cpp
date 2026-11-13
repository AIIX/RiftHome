// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
#include "settings/Backends.hpp"
#include "spatial/Controls.hpp"

#include "settings/Parse.hpp"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcessEnvironment>
#include <QSaveFile>
#include <QStandardPaths>
#include <QSysInfo>
#include <QUrl>
#include <QVariantMap>

#include <algorithm>
#include <cmath>
#include <csignal>

namespace RiftHome::Settings
{

void run(const QString &program, const QStringList &arguments, std::function<void(int, const QString &, const QString &)> done)
{
    auto *process = new QProcess;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("LC_ALL"), QStringLiteral("C")); // the tools' words as parsed, not translated
    process->setProcessEnvironment(env);
    QObject::connect(process, &QProcess::finished, process, [process, done](int code, QProcess::ExitStatus status) {
        done(status == QProcess::NormalExit ? code : -1, QString::fromUtf8(process->readAllStandardOutput()),
             QString::fromUtf8(process->readAllStandardError()).trimmed());
        process->deleteLater();
    });
    QObject::connect(process, &QProcess::errorOccurred, process, [process, program, done](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            done(-1, QString(), QStringLiteral("%1 is not installed").arg(program));
            process->deleteLater();
        }
    });
    process->start(program, arguments);
}

namespace
{

//! nmcli's error line without its "Error: " prefix, for people.
QString nmError(const QString &err)
{
    QString text = err.section(QLatin1Char('\n'), 0, 0);
    if (text.startsWith(QLatin1String("Error: "))) {
        text = text.mid(7);
    }
    return text;
}

} // namespace

// --- Network ------------------------------------------------------------------------------------------------------

void Network::refresh(bool rescan)
{
    run(QStringLiteral("nmcli"), {QStringLiteral("-t"), QStringLiteral("-f"), QStringLiteral("WIFI-HW,WIFI"), QStringLiteral("general")},
        [this, rescan](int code, const QString &out, const QString &) {
            m_loaded = true;
            m_available = code == 0;
            if (!m_available) {
                m_devices.clear();
                m_wifiNetworks.clear();
                Q_EMIT changed();
                return;
            }
            const QStringList f = splitTerse(out.trimmed());
            m_wifiHardware = f.value(0) == QLatin1String("enabled");
            m_wifiEnabled = f.value(1) == QLatin1String("enabled");
            Q_EMIT changed(); // each step shows as it comes (a fresh Wi-Fi scan takes seconds)
            run(QStringLiteral("nmcli"),
                {QStringLiteral("-t"), QStringLiteral("-f"), QStringLiteral("GENERAL.DEVICE,GENERAL.TYPE,GENERAL.STATE,GENERAL.CONNECTION,IP4.ADDRESS"),
                 QStringLiteral("device"), QStringLiteral("show")},
                [this, rescan](int, const QString &out, const QString &) {
                    m_devices.clear();
                    m_wifiDevice.clear();
                    for (const auto &d : parseDeviceShow(out)) {
                        const QString type = d.value(QStringLiteral("GENERAL.TYPE"));
                        if (type != QLatin1String("ethernet") && type != QLatin1String("wifi")) {
                            continue; // loopback, Wi-Fi P2P, bridges: not for people
                        }
                        if (type == QLatin1String("wifi") && m_wifiDevice.isEmpty()) {
                            m_wifiDevice = d.value(QStringLiteral("GENERAL.DEVICE"));
                        }
                        // "100 (connected)" -> "connected"
                        QString state = d.value(QStringLiteral("GENERAL.STATE"));
                        if (const int open = state.indexOf(QLatin1Char('(')); open >= 0) {
                            state = state.mid(open + 1).chopped(state.endsWith(QLatin1Char(')')) ? 1 : 0);
                        }
                        m_devices << QVariantMap{{QStringLiteral("device"), d.value(QStringLiteral("GENERAL.DEVICE"))},
                                                 {QStringLiteral("wifi"), type == QLatin1String("wifi")},
                                                 {QStringLiteral("state"), state},
                                                 {QStringLiteral("connected"), state == QLatin1String("connected")},
                                                 {QStringLiteral("connection"), d.value(QStringLiteral("GENERAL.CONNECTION"))},
                                                 {QStringLiteral("address"), d.value(QStringLiteral("IP4.ADDRESS"))}};
                    }
                    Q_EMIT changed();
                    run(QStringLiteral("nmcli"), {QStringLiteral("-t"), QStringLiteral("-f"), QStringLiteral("NAME,TYPE,UUID,ACTIVE"), QStringLiteral("connection"), QStringLiteral("show")},
                        [this, rescan](int, const QString &out, const QString &) {
                            m_vpns.clear();
                            for (const VpnConnection &v : parseVpnConnections(out)) {
                                m_vpns << QVariantMap{{QStringLiteral("name"), v.name}, {QStringLiteral("uuid"), v.uuid},
                                                      {QStringLiteral("active"), v.active}, {QStringLiteral("wireguard"), v.wireguard}};
                            }
                            m_known.clear();
                            for (const QString &line : out.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
                                const QStringList f = splitTerse(line);
                                if (f.value(1) == QLatin1String("802-11-wireless")) {
                                    m_known << f.value(0);
                                }
                            }
                            if (!m_wifiEnabled || m_wifiDevice.isEmpty()) {
                                m_wifiNetworks.clear();
                                Q_EMIT changed();
                                return;
                            }
                            run(QStringLiteral("nmcli"),
                                {QStringLiteral("-t"), QStringLiteral("-f"), QStringLiteral("IN-USE,SSID,SIGNAL,SECURITY"), QStringLiteral("device"),
                                 QStringLiteral("wifi"), QStringLiteral("list"), QStringLiteral("--rescan"), rescan ? QStringLiteral("auto") : QStringLiteral("no")},
                                [this](int, const QString &out, const QString &) {
                                    m_wifiNetworks.clear();
                                    for (const WifiNetwork &n : parseWifiList(out)) {
                                        m_wifiNetworks << QVariantMap{{QStringLiteral("ssid"), n.ssid},
                                                                      {QStringLiteral("signal"), n.signal},
                                                                      {QStringLiteral("secure"), n.secure},
                                                                      {QStringLiteral("active"), n.active},
                                                                      {QStringLiteral("known"), m_known.contains(n.ssid)}};
                                    }
                                    Q_EMIT changed();
                                });
                        });
                });
        });
}

void Network::setMessage(const QString &message)
{
    m_message = message;
    Q_EMIT changed();
}

void Network::setWifiEnabled(bool on)
{
    run(QStringLiteral("nmcli"), {QStringLiteral("radio"), QStringLiteral("wifi"), on ? QStringLiteral("on") : QStringLiteral("off")},
        [this](int code, const QString &, const QString &err) {
            setMessage(code == 0 ? QString() : nmError(err));
            refresh(true);
        });
}

void Network::join(const QString &ssid, const QString &password)
{
    m_busy = ssid;
    setMessage(QString());
    // The password goes on nmcli's command line for the moment it runs (a known limit: nmcli takes no password on its
    // input for "wifi connect"; fine on a one-person machine).
    QStringList args{QStringLiteral("device"), QStringLiteral("wifi"), QStringLiteral("connect"), ssid};
    if (!password.isEmpty()) {
        args << QStringLiteral("password") << password;
    }
    if (!m_wifiDevice.isEmpty()) {
        args << QStringLiteral("ifname") << m_wifiDevice;
    }
    run(QStringLiteral("nmcli"), args, [this, ssid](int code, const QString &, const QString &err) {
        m_busy.clear();
        setMessage(code == 0 ? QStringLiteral("Connected to %1").arg(ssid) : QStringLiteral("Could not join %1: %2").arg(ssid, nmError(err)));
        refresh();
    });
}

void Network::disconnectWifi()
{
    if (m_wifiDevice.isEmpty()) {
        return;
    }
    run(QStringLiteral("nmcli"), {QStringLiteral("device"), QStringLiteral("disconnect"), m_wifiDevice}, [this](int code, const QString &, const QString &err) {
        setMessage(code == 0 ? QString() : nmError(err));
        refresh();
    });
}

void Network::setVpn(const QString &uuid, bool on)
{
    m_busy = uuid; // as a network being joined: its row shows it is working
    Q_EMIT changed();
    run(QStringLiteral("nmcli"), {QStringLiteral("connection"), on ? QStringLiteral("up") : QStringLiteral("down"), QStringLiteral("uuid"), uuid},
        [this](int code, const QString &, const QString &err) {
            m_busy.clear();
            setMessage(code == 0 ? QString() : nmError(err));
            refresh();
        });
}

void Network::forget(const QString &ssid)
{
    run(QStringLiteral("nmcli"), {QStringLiteral("connection"), QStringLiteral("delete"), QStringLiteral("id"), ssid},
        [this, ssid](int code, const QString &, const QString &err) {
            setMessage(code == 0 ? QStringLiteral("Forgot %1").arg(ssid) : nmError(err));
            refresh();
        });
}

void Network::loadDetails(const QString &connection, const QString &device)
{
    run(QStringLiteral("nmcli"),
        {QStringLiteral("-t"), QStringLiteral("-f"), QStringLiteral("ipv4.method,ipv4.addresses,ipv4.gateway,ipv4.dns,ipv4.ignore-auto-dns"),
         QStringLiteral("connection"), QStringLiteral("show"), QStringLiteral("id"), connection},
        [this, connection, device](int, const QString &out, const QString &) {
            const auto settings = parseDeviceShow(out).value(0);
            run(QStringLiteral("nmcli"),
                {QStringLiteral("-t"), QStringLiteral("-f"), QStringLiteral("IP4.ADDRESS,IP4.GATEWAY,IP4.DNS"), QStringLiteral("device"),
                 QStringLiteral("show"), device},
                [this, connection, device, settings](int, const QString &out, const QString &) {
                    const auto now = parseDeviceShow(out).value(0);
                    // A manual address as "192.168.0.20/24"; else what the device has, to start from.
                    const QString configured = settings.value(QStringLiteral("ipv4.addresses")).section(QLatin1Char(','), 0, 0).trimmed();
                    const QString current = now.value(QStringLiteral("IP4.ADDRESS")).section(QLatin1Char(','), 0, 0).trimmed();
                    const QString address = configured.isEmpty() ? current : configured;
                    const QString dns = settings.value(QStringLiteral("ipv4.dns"));
                    m_details = QVariantMap{
                        {QStringLiteral("connection"), connection},
                        {QStringLiteral("device"), device},
                        {QStringLiteral("manual"), settings.value(QStringLiteral("ipv4.method")) == QLatin1String("manual")},
                        {QStringLiteral("address"), address.section(QLatin1Char('/'), 0, 0)},
                        {QStringLiteral("prefix"), address.contains(QLatin1Char('/')) ? address.section(QLatin1Char('/'), 1).toInt() : 24},
                        {QStringLiteral("gateway"), settings.value(QStringLiteral("ipv4.gateway")).isEmpty() ? now.value(QStringLiteral("IP4.GATEWAY"))
                                                                                                           : settings.value(QStringLiteral("ipv4.gateway"))},
                        {QStringLiteral("automaticDns"), dns.isEmpty() || settings.value(QStringLiteral("ipv4.ignore-auto-dns")) != QLatin1String("yes")},
                        {QStringLiteral("dns"), QString(dns).replace(QLatin1Char(','), QStringLiteral(", "))},
                        {QStringLiteral("currentAddress"), current},
                        {QStringLiteral("currentGateway"), now.value(QStringLiteral("IP4.GATEWAY"))},
                        {QStringLiteral("currentDns"), now.value(QStringLiteral("IP4.DNS"))},
                    };
                    Q_EMIT detailsChanged();
                });
        });
}

QString Network::applyDetails(const QString &connection, bool manual, const QString &address, int prefix, const QString &gateway,
                              bool automaticDns, const QString &dns)
{
    if (manual && !validIPv4(address)) {
        return QStringLiteral("The address is not an IPv4 address (four numbers 0-255, e.g. 192.168.0.20).");
    }
    if (manual && (prefix < 1 || prefix > 32)) {
        return QStringLiteral("The prefix is 1 to 32 (24 for a 255.255.255.0 network).");
    }
    if (manual && !gateway.trimmed().isEmpty() && !validIPv4(gateway)) {
        return QStringLiteral("The gateway is not an IPv4 address.");
    }
    const QStringList servers = parseDnsServers(dns);
    if (!automaticDns && servers.isEmpty()) {
        return QStringLiteral("Give one or more DNS servers (e.g. 1.1.1.1, 9.9.9.9), or turn automatic DNS on.");
    }
    QStringList args{QStringLiteral("connection"), QStringLiteral("modify"), QStringLiteral("id"), connection};
    if (manual) {
        args << QStringLiteral("ipv4.addresses") << QStringLiteral("%1/%2").arg(address.trimmed()).arg(prefix) << QStringLiteral("ipv4.gateway")
             << gateway.trimmed() << QStringLiteral("ipv4.method") << QStringLiteral("manual");
    } else {
        // The manual address goes first: an automatic connection keeps any listed as extra addresses.
        args << QStringLiteral("ipv4.addresses") << QString() << QStringLiteral("ipv4.gateway") << QString() << QStringLiteral("ipv4.method")
             << QStringLiteral("auto");
    }
    args << QStringLiteral("ipv4.dns") << (automaticDns ? QString() : servers.join(QLatin1Char(','))) << QStringLiteral("ipv4.ignore-auto-dns")
         << (automaticDns ? QStringLiteral("no") : QStringLiteral("yes"));
    setMessage(QStringLiteral("Applying to %1…").arg(connection));
    run(QStringLiteral("nmcli"), args, [this, connection](int code, const QString &, const QString &err) {
        if (code != 0) {
            setMessage(QStringLiteral("Could not change %1: %2").arg(connection, nmError(err)));
            return;
        }
        // Reconnected, so the new settings take effect now.
        run(QStringLiteral("nmcli"), {QStringLiteral("connection"), QStringLiteral("up"), QStringLiteral("id"), connection},
            [this, connection](int code, const QString &, const QString &err) {
                setMessage(code == 0 ? QStringLiteral("%1: new settings in use").arg(connection)
                                     : QStringLiteral("Could not reconnect %1: %2").arg(connection, nmError(err)));
                refresh();
            });
    });
    return {};
}

// --- Bluetooth ----------------------------------------------------------------------------------------------------

void Bluetooth::refresh()
{
    run(QStringLiteral("bluetoothctl"), {QStringLiteral("show")}, [this](int code, const QString &out, const QString &) {
        m_loaded = true;
        m_available = code == 0 && out.contains(QLatin1String("Controller"));
        m_powered = parseColonFields(out).value(QStringLiteral("Powered")) == QLatin1String("yes");
        if (!m_available) {
            m_devices.clear();
            Q_EMIT changed();
            return;
        }
        run(QStringLiteral("bluetoothctl"), {QStringLiteral("devices")}, [this](int, const QString &all, const QString &) {
            run(QStringLiteral("bluetoothctl"), {QStringLiteral("devices"), QStringLiteral("Paired")}, [this, all](int, const QString &paired, const QString &) {
                run(QStringLiteral("bluetoothctl"), {QStringLiteral("devices"), QStringLiteral("Connected")},
                    [this, all, paired](int, const QString &connected, const QString &) {
                        QStringList pairedSet;
                        for (const BluetoothDevice &d : parseBluetoothDevices(paired)) {
                            pairedSet << d.address;
                        }
                        QStringList connectedSet;
                        for (const BluetoothDevice &d : parseBluetoothDevices(connected)) {
                            connectedSet << d.address;
                        }
                        m_devices.clear();
                        for (const BluetoothDevice &d : parseBluetoothDevices(all)) {
                            m_devices << QVariantMap{{QStringLiteral("address"), d.address},
                                                     {QStringLiteral("name"), d.name},
                                                     {QStringLiteral("paired"), pairedSet.contains(d.address)},
                                                     {QStringLiteral("connected"), connectedSet.contains(d.address)}};
                        }
                        // Connected first, then paired, then by name.
                        std::stable_sort(m_devices.begin(), m_devices.end(), [](const QVariant &a, const QVariant &b) {
                            const QVariantMap x = a.toMap(), y = b.toMap();
                            const int rx = x.value(QStringLiteral("connected")).toBool() ? 0 : x.value(QStringLiteral("paired")).toBool() ? 1 : 2;
                            const int ry = y.value(QStringLiteral("connected")).toBool() ? 0 : y.value(QStringLiteral("paired")).toBool() ? 1 : 2;
                            return rx != ry ? rx < ry : x.value(QStringLiteral("name")).toString().localeAwareCompare(y.value(QStringLiteral("name")).toString()) < 0;
                        });
                        Q_EMIT changed();
                    });
            });
        });
    });
}

void Bluetooth::act(const QString &address, const QStringList &arguments, const QString &doing, std::function<void(bool)> then)
{
    m_busy = address;
    m_message = doing;
    Q_EMIT changed();
    run(QStringLiteral("bluetoothctl"), QStringList{QStringLiteral("--timeout"), QStringLiteral("20")} + arguments,
        [this, then](int code, const QString &out, const QString &err) {
            // bluetoothctl says how it went on its output ("Connection successful", "Failed to pair: ...").
            const QString said = (out + QLatin1Char('\n') + err).trimmed();
            const bool failed = code != 0 || said.contains(QLatin1String("Failed")) || said.contains(QLatin1String("not available"));
            QString line;
            for (const QString &l : said.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
                if (l.contains(QLatin1String("Failed")) || l.contains(QLatin1String("successful")) || l.contains(QLatin1String("succeeded"))) {
                    line = l.trimmed();
                }
            }
            m_busy.clear();
            m_message = failed ? (line.isEmpty() ? QStringLiteral("It did not work.") : line) : QString();
            if (then) {
                then(!failed);
            } else {
                refresh();
            }
        });
}

void Bluetooth::setPowered(bool on)
{
    act(QString(), {QStringLiteral("power"), on ? QStringLiteral("on") : QStringLiteral("off")}, QString());
}

void Bluetooth::scan()
{
    if (m_scanning) {
        return;
    }
    m_scanning = true;
    Q_EMIT changed();
    run(QStringLiteral("bluetoothctl"), {QStringLiteral("--timeout"), QStringLiteral("12"), QStringLiteral("scan"), QStringLiteral("on")},
        [this](int, const QString &, const QString &) {
            m_scanning = false;
            refresh();
        });
}

void Bluetooth::connectDevice(const QString &address)
{
    act(address, {QStringLiteral("connect"), address}, QStringLiteral("Connecting…"));
}

void Bluetooth::disconnectDevice(const QString &address)
{
    act(address, {QStringLiteral("disconnect"), address}, QStringLiteral("Disconnecting…"));
}

void Bluetooth::pair(const QString &address)
{
    act(address, {QStringLiteral("pair"), address}, QStringLiteral("Pairing…"), [this, address](bool ok) {
        if (!ok) {
            refresh();
            return;
        }
        // Trusted, so it may connect by itself next time; then connected now.
        act(address, {QStringLiteral("trust"), address}, QStringLiteral("Pairing…"), [this, address](bool) {
            connectDevice(address);
        });
    });
}

void Bluetooth::forget(const QString &address)
{
    act(address, {QStringLiteral("remove"), address}, QStringLiteral("Forgetting…"));
}

// --- Sound --------------------------------------------------------------------------------------------------------

namespace
{

QVariantList toVariant(const QList<AudioDevice> &devices)
{
    QVariantList list;
    for (const AudioDevice &d : devices) {
        list << QVariantMap{{QStringLiteral("name"), d.name},
                            {QStringLiteral("description"), d.description},
                            {QStringLiteral("volume"), d.volume},
                            {QStringLiteral("muted"), d.muted}};
    }
    return list;
}

} // namespace

void Sound::refresh()
{
    run(QStringLiteral("pactl"), {QStringLiteral("-f"), QStringLiteral("json"), QStringLiteral("info")}, [this](int code, const QString &out, const QString &) {
        if (code != 0) {
            return;
        }
        const QJsonObject info = QJsonDocument::fromJson(out.toUtf8()).object();
        m_defaultOutput = info.value(QStringLiteral("default_sink_name")).toString();
        m_defaultInput = info.value(QStringLiteral("default_source_name")).toString();
        run(QStringLiteral("pactl"), {QStringLiteral("-f"), QStringLiteral("json"), QStringLiteral("list"), QStringLiteral("sinks")},
            [this](int, const QString &out, const QString &) {
                m_outputs = toVariant(parseAudioDevices(out.toUtf8()));
                run(QStringLiteral("pactl"), {QStringLiteral("-f"), QStringLiteral("json"), QStringLiteral("list"), QStringLiteral("sources")},
                    [this](int, const QString &out, const QString &) {
                        m_inputs = toVariant(parseAudioDevices(out.toUtf8()));
                        Q_EMIT changed();
                    });
            });
    });
}

void Sound::setDefault(bool output, const QString &name)
{
    run(QStringLiteral("pactl"), {output ? QStringLiteral("set-default-sink") : QStringLiteral("set-default-source"), name},
        [this](int, const QString &, const QString &) {
            refresh();
        });
}

void Sound::setVolume(bool output, const QString &name, int percent)
{
    run(QStringLiteral("pactl"),
        {output ? QStringLiteral("set-sink-volume") : QStringLiteral("set-source-volume"), name, QStringLiteral("%1%").arg(std::clamp(percent, 0, 100))},
        [this](int, const QString &, const QString &) {
            refresh();
        });
}

void Sound::setMuted(bool output, const QString &name, bool muted)
{
    run(QStringLiteral("pactl"),
        {output ? QStringLiteral("set-sink-mute") : QStringLiteral("set-source-mute"), name, muted ? QStringLiteral("1") : QStringLiteral("0")},
        [this](int, const QString &, const QString &) {
            refresh();
        });
}

// --- Date and time ------------------------------------------------------------------------------------------------

void DateTime::refresh()
{
    run(QStringLiteral("timedatectl"), {QStringLiteral("show")}, [this](int code, const QString &out, const QString &) {
        if (code != 0) {
            return;
        }
        const auto v = parseKeyValues(out);
        m_timeZone = v.value(QStringLiteral("Timezone"));
        m_automatic = v.value(QStringLiteral("NTP")) == QLatin1String("yes");
        m_synchronized = v.value(QStringLiteral("NTPSynchronized")) == QLatin1String("yes");
        Q_EMIT changed();
    });
}

void DateTime::loadTimeZones()
{
    if (!m_timeZones.isEmpty()) {
        return;
    }
    run(QStringLiteral("timedatectl"), {QStringLiteral("list-timezones")}, [this](int, const QString &out, const QString &) {
        m_timeZones = out.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        Q_EMIT timeZonesChanged();
    });
}

namespace
{

/*!
 * A call to systemd's time service from this process (not through timedatectl): polkit then asks Settings' own agent
 * for the administrator's password (Authentication), so the prompt shows in the headset. interactive = true lets
 * polkit ask.
 */
void callTimedate(DateTime *owner, const QString &method, const QVariantList &arguments, std::function<void(const QString &)> done)
{
    QDBusMessage call = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.timedate1"), QStringLiteral("/org/freedesktop/timedate1"),
                                                       QStringLiteral("org.freedesktop.timedate1"), method);
    call.setArguments(arguments);
    call.setInteractiveAuthorizationAllowed(true);
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(call, 120000), owner);
    QObject::connect(watcher, &QDBusPendingCallWatcher::finished, owner, [watcher, done] {
        const QDBusPendingReply<> reply = *watcher;
        done(reply.isError() ? reply.error().message() : QString());
        watcher->deleteLater();
    });
}

} // namespace

void DateTime::setTimeZone(const QString &zone)
{
    callTimedate(this, QStringLiteral("SetTimezone"), {zone, true}, [this](const QString &error) {
        m_message = error.isEmpty() ? QString() : QStringLiteral("Not changed: %1").arg(error);
        refresh();
    });
}

void DateTime::setAutomatic(bool on)
{
    callTimedate(this, QStringLiteral("SetNTP"), {on, true}, [this](const QString &error) {
        m_message = error.isEmpty() ? QString() : QStringLiteral("Not changed: %1").arg(error);
        refresh();
    });
}

// --- Rift Home's settings -----------------------------------------------------------------------------------------

RiftSettings::RiftSettings(QObject *parent)
    : QObject(parent)
{
    QString dir = qEnvironmentVariable("RIFT_HOME_STATE_DIR");
    if (dir.isEmpty()) {
        dir = QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation) + QStringLiteral("/rift-home");
    }
    m_file = dir + QStringLiteral("/settings.json");
    m_recordTimer.setSingleShot(true);
    connect(&m_recordTimer, &QTimer::timeout, this, [this] {
        endRecording(QStringLiteral("Rift Home did not answer."));
    });
    load();
    // Rift Home may write it too (its defaults): follow the file (written whole, so the folder is watched as well).
    m_watcher.addPath(dir);
    if (QFileInfo::exists(m_file)) {
        m_watcher.addPath(m_file);
    }
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] {
        if (!m_watcher.files().contains(m_file) && QFileInfo::exists(m_file)) {
            m_watcher.addPath(m_file);
        }
        load();
    });
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this] {
        load();
    });
    connect(&m_roomsWatcher, &QFileSystemWatcher::directoryChanged, this, [this] {
        findPhotos();
    });
}

void RiftSettings::load()
{
    QFile file(m_file);
    if (file.open(QIODevice::ReadOnly)) {
        const QJsonObject loaded = QJsonDocument::fromJson(file.readAll()).object();
        if (loaded != m_settings) {
            m_settings = loaded;
            Q_EMIT changed();
        }
    }
}

void RiftSettings::set(const QString &group, const QString &key, const QJsonValue &value)
{
    load(); // the latest file first: keys this app doesn't know are kept
    QJsonObject g = m_settings.value(group).toObject();
    if (g.value(key) == value) {
        return;
    }
    g.insert(key, value);
    m_settings.insert(group, g);
    QDir().mkpath(QFileInfo(m_file).absolutePath());
    QSaveFile file(m_file);
    if (file.open(QIODevice::WriteOnly)) {
        file.write(QJsonDocument(m_settings).toJson());
        file.commit();
    }
    Q_EMIT changed();
}

QString RiftSettings::frameTheme() const
{
    return m_settings.value(QStringLiteral("frame")).toObject().value(QStringLiteral("theme")).toString(QStringLiteral("dark"));
}

bool RiftSettings::keyboardAuto() const
{
    return m_settings.value(QStringLiteral("keyboard")).toObject().value(QStringLiteral("showOnTextField")).toBool(true);
}

QString RiftSettings::roomPhoto() const
{
    return m_settings.value(QStringLiteral("room")).toObject().value(QStringLiteral("photo")).toString();
}

double RiftSettings::roomBrightness() const
{
    return m_settings.value(QStringLiteral("room")).toObject().value(QStringLiteral("brightness")).toDouble(1.0);
}

QVariantList RiftSettings::controlRows() const
{
    using RiftHome::Spatial::ButtonAction;
    using RiftHome::Spatial::ButtonMap;
    const ButtonMap map = ButtonMap::fromJson(m_settings.value(QStringLiteral("controls")).toObject());
    const ButtonMap standard = ButtonMap::defaults();
    const auto join = [](const std::vector<RiftHome::Spatial::Binding> &bindings) {
        QStringList labels;
        for (const auto &binding : bindings) {
            labels << binding.label();
        }
        return labels.isEmpty() ? QStringLiteral("None") : labels.join(QStringLiteral(", "));
    };
    QVariantList rows;
    for (std::size_t a = 0; a < RiftHome::Spatial::kButtonActions; ++a) {
        const ButtonAction action = ButtonAction(a);
        QVariantList bindings;
        for (const auto &binding : map.bindings(action)) {
            bindings.append(QVariantMap{{QStringLiteral("name"), binding.name()}, {QStringLiteral("label"), binding.label()}});
        }
        rows.append(QVariantMap{{QStringLiteral("action"), ButtonMap::name(action)},
                                {QStringLiteral("label"), ButtonMap::label(action)},
                                {QStringLiteral("defaultText"), join(standard.bindings(action))},
                                {QStringLiteral("custom"), !map.isDefault(action)},
                                {QStringLiteral("bindings"), bindings}});
    }
    return rows;
}

void RiftSettings::saveControls(const QJsonValue &controls)
{
    load(); // the latest file first: the rest is kept
    if (controls.isUndefined()) {
        if (!m_settings.contains(QStringLiteral("controls"))) {
            return;
        }
        m_settings.remove(QStringLiteral("controls"));
    } else {
        if (m_settings.value(QStringLiteral("controls")) == controls) {
            return;
        }
        m_settings.insert(QStringLiteral("controls"), controls);
    }
    QDir().mkpath(QFileInfo(m_file).absolutePath());
    QSaveFile file(m_file);
    if (file.open(QIODevice::WriteOnly)) {
        file.write(QJsonDocument(m_settings).toJson());
        file.commit();
    }
    Q_EMIT changed();
}

void RiftSettings::recordBinding(const QString &action)
{
    using RiftHome::Spatial::ButtonAction;
    ButtonAction parsed{};
    if (!RiftHome::Spatial::ButtonMap::actionFromName(action, &parsed)) {
        return;
    }
    if (!m_recordingAction.isEmpty()) {
        cancelRecording();
    }
    m_recordingAction = action;
    m_recordNote.clear();
    Q_EMIT recordingChanged();
    // Rift Home answers with BindingRecorded once all pressed buttons are let go (or after 10 s, empty).
    QDBusConnection::sessionBus().connect(QStringLiteral("org.rifthome.RiftHome"), QStringLiteral("/Controls"), QStringLiteral("org.rifthome.RiftHome.Controls"),
                                          QStringLiteral("BindingRecorded"), this, SLOT(onBindingRecorded(QString)));
    const QDBusMessage call = QDBusMessage::createMethodCall(QStringLiteral("org.rifthome.RiftHome"), QStringLiteral("/Controls"),
                                                             QStringLiteral("org.rifthome.RiftHome.Controls"), QStringLiteral("RecordBinding"));
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(call, 3000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, action] {
        const QDBusPendingReply<> reply = *watcher;
        watcher->deleteLater();
        if (reply.isError() && m_recordingAction == action) {
            endRecording(QStringLiteral("Rift Home cannot record buttons now (it must be running with the headset on)."));
        }
    });
    m_recordTimer.start(13000);
}

void RiftSettings::cancelRecording()
{
    if (m_recordingAction.isEmpty()) {
        return;
    }
    QDBusConnection::sessionBus().asyncCall(QDBusMessage::createMethodCall(QStringLiteral("org.rifthome.RiftHome"), QStringLiteral("/Controls"),
                                                                           QStringLiteral("org.rifthome.RiftHome.Controls"), QStringLiteral("CancelRecording")));
    endRecording(QString());
}

void RiftSettings::showLensTest()
{
    const QDBusMessage call = QDBusMessage::createMethodCall(QStringLiteral("org.rifthome.RiftHome"), QStringLiteral("/Headset"),
                                                             QStringLiteral("org.rifthome.RiftHome.Headset"), QStringLiteral("ShowLensTest"));
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(call, 3000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher] {
        const QDBusPendingReply<> reply = *watcher;
        watcher->deleteLater();
        const QString note = reply.isError() ? QStringLiteral("Rift Home cannot show it now (it must be running with the headset on).") : QString();
        if (note != m_lensTestNote) {
            m_lensTestNote = note;
            Q_EMIT lensTestNoteChanged();
        }
    });
}

void RiftSettings::endRecording(const QString &note)
{
    m_recordTimer.stop();
    m_recordingAction.clear();
    m_recordNote = note;
    Q_EMIT recordingChanged();
}

void RiftSettings::onBindingRecorded(const QString &binding)
{
    using RiftHome::Spatial::ButtonAction;
    using RiftHome::Spatial::ButtonMap;
    ButtonAction action{};
    if (m_recordingAction.isEmpty() || !ButtonMap::actionFromName(m_recordingAction, &action)) {
        return;
    }
    const auto parsed = RiftHome::Spatial::Binding::fromName(binding);
    if (!parsed) {
        endRecording(binding.isEmpty() ? QStringLiteral("Nothing was pressed.") : QString());
        return;
    }
    load();
    ButtonMap map = ButtonMap::fromJson(m_settings.value(QStringLiteral("controls")).toObject());
    const auto from = map.assign(action, *parsed);
    saveControls(map.toJson());
    endRecording(from ? QStringLiteral("%1 moved here from %2.").arg(parsed->label(), ButtonMap::label(*from)) : QString());
}

void RiftSettings::clearControl(const QString &action)
{
    using RiftHome::Spatial::ButtonMap;
    RiftHome::Spatial::ButtonAction parsed{};
    if (!ButtonMap::actionFromName(action, &parsed)) {
        return;
    }
    load();
    ButtonMap map = ButtonMap::fromJson(m_settings.value(QStringLiteral("controls")).toObject());
    map.clear(parsed);
    saveControls(map.toJson());
    m_recordNote.clear();
    Q_EMIT recordingChanged();
}

void RiftSettings::resetControl(const QString &action)
{
    using RiftHome::Spatial::ButtonMap;
    RiftHome::Spatial::ButtonAction parsed{};
    if (!ButtonMap::actionFromName(action, &parsed)) {
        return;
    }
    load();
    ButtonMap map = ButtonMap::fromJson(m_settings.value(QStringLiteral("controls")).toObject());
    // Its defaults, taken back from whichever action has them now.
    map.setBindings(parsed, ButtonMap::defaults().bindings(parsed));
    saveControls(map.toJson());
    m_recordNote.clear();
    Q_EMIT recordingChanged();
}

void RiftSettings::resetControls()
{
    saveControls(QJsonValue(QJsonValue::Undefined));
    m_recordNote.clear();
    Q_EMIT recordingChanged();
}

double RiftSettings::windowCurve() const
{
    return m_settings.value(QStringLiteral("windows")).toObject().value(QStringLiteral("curve")).toDouble(0.5);
}

bool RiftSettings::use24Hour() const
{
    return m_settings.value(QStringLiteral("clock")).toObject().value(QStringLiteral("use24Hour")).toBool(true);
}

QString RiftSettings::cinemaRoom() const
{
    return m_settings.value(QStringLiteral("cinema")).toObject().value(QStringLiteral("room")).toString(QStringLiteral("dim"));
}

void RiftSettings::setCinemaRoom(const QString &room)
{
    set(QStringLiteral("cinema"), QStringLiteral("room"), room == QLatin1String("dark") || room == QLatin1String("soft") ? room : QStringLiteral("dim"));
}

int RiftSettings::roomTurn() const
{
    return m_settings.value(QStringLiteral("room")).toObject().value(QStringLiteral("turn")).toInt(0);
}

void RiftSettings::setRoomTurn(int degrees)
{
    set(QStringLiteral("room"), QStringLiteral("turn"), std::clamp(degrees, -180, 180));
}

QString RiftSettings::pointerColour() const
{
    return m_settings.value(QStringLiteral("pointer")).toObject().value(QStringLiteral("colour")).toString(QStringLiteral("ice"));
}

QString RiftSettings::pointerThickness() const
{
    return m_settings.value(QStringLiteral("pointer")).toObject().value(QStringLiteral("thickness")).toString(QStringLiteral("normal"));
}

int RiftSettings::pointerHideAfter() const
{
    return m_settings.value(QStringLiteral("pointer")).toObject().value(QStringLiteral("hideAfter")).toInt(0);
}

void RiftSettings::setPointerColour(const QString &colour)
{
    static const QStringList known{QStringLiteral("ice"), QStringLiteral("white"), QStringLiteral("green"), QStringLiteral("amber"), QStringLiteral("pink")};
    set(QStringLiteral("pointer"), QStringLiteral("colour"), known.contains(colour) ? colour : QStringLiteral("ice"));
}

void RiftSettings::setPointerThickness(const QString &thickness)
{
    set(QStringLiteral("pointer"), QStringLiteral("thickness"),
        thickness == QLatin1String("thin") || thickness == QLatin1String("thick") ? thickness : QStringLiteral("normal"));
}

void RiftSettings::setPointerHideAfter(int seconds)
{
    set(QStringLiteral("pointer"), QStringLiteral("hideAfter"), std::clamp(seconds, 0, 60));
}

double RiftSettings::x11Scale() const
{
    return m_settings.value(QStringLiteral("apps")).toObject().value(QStringLiteral("x11Scale")).toDouble(2.0);
}

double RiftSettings::appScale() const
{
    return m_settings.value(QStringLiteral("apps")).toObject().value(QStringLiteral("scale")).toDouble(2.0);
}

void RiftSettings::setAppScale(double scale)
{
    set(QStringLiteral("apps"), QStringLiteral("scale"), std::clamp(scale, 1.0, 3.0));
}

int RiftSettings::composition() const
{
    return m_settings.value(QStringLiteral("display")).toObject().value(QStringLiteral("composition")).toInt(165);
}

void RiftSettings::setComposition(int percent)
{
    set(QStringLiteral("display"), QStringLiteral("composition"), std::clamp(percent, 165, 200));
}

void RiftSettings::setX11Scale(double scale)
{
    set(QStringLiteral("apps"), QStringLiteral("x11Scale"), std::clamp(scale, 1.0, 3.0));
}

bool RiftSettings::ownAppData() const
{
    return m_settings.value(QStringLiteral("apps")).toObject().value(QStringLiteral("ownData")).toBool(true);
}

bool RiftSettings::reopenApps() const
{
    return m_settings.value(QStringLiteral("apps")).toObject().value(QStringLiteral("reopen")).toBool(true);
}

void RiftSettings::setReopenApps(bool on)
{
    set(QStringLiteral("apps"), QStringLiteral("reopen"), on);
}

void RiftSettings::setOwnAppData(bool on)
{
    set(QStringLiteral("apps"), QStringLiteral("ownData"), on);
}

void RiftSettings::setFrameTheme(const QString &theme)
{
    set(QStringLiteral("frame"), QStringLiteral("theme"), theme == QLatin1String("light") ? QStringLiteral("light") : QStringLiteral("dark"));
}

void RiftSettings::setKeyboardAuto(bool on)
{
    set(QStringLiteral("keyboard"), QStringLiteral("showOnTextField"), on);
}

void RiftSettings::setRoomPhoto(const QString &path)
{
    set(QStringLiteral("room"), QStringLiteral("photo"), path);
}

void RiftSettings::setRoomBrightness(double brightness)
{
    set(QStringLiteral("room"), QStringLiteral("brightness"), std::round(std::clamp(brightness, 0.2, 1.0) * 100.0) / 100.0);
}

void RiftSettings::setWindowCurve(double curve)
{
    set(QStringLiteral("windows"), QStringLiteral("curve"), std::round(std::clamp(curve, 0.0, 1.0) * 100.0) / 100.0);
}

void RiftSettings::setUse24Hour(bool on)
{
    set(QStringLiteral("clock"), QStringLiteral("use24Hour"), on);
}

namespace
{

//! Pictures a room can be made of: Radiance HDR and what Qt reads here (JPEG XL and AVIF where their plugins are).
QStringList roomNameFilters()
{
    QStringList filters{QStringLiteral("*.hdr")};
    const QList<QByteArray> readable = QImageReader::supportedImageFormats();
    for (const char *format : {"jpg", "jpeg", "png", "webp", "jxl", "avif"}) {
        if (readable.contains(QByteArray(format))) {
            filters << QStringLiteral("*.") + QLatin1String(format);
        }
    }
    return filters;
}

//! A picture's file name as a room's name: "living-room.jpg" -> "Living room".
QString roomName(const QFileInfo &file)
{
    QString name = file.completeBaseName().replace(QLatin1Char('_'), QLatin1Char(' ')).replace(QLatin1Char('-'), QLatin1Char(' ')).simplified();
    if (!name.isEmpty()) {
        name[0] = name[0].toUpper();
    }
    return name;
}

/*!
 * A Plasma wallpaper package (<dir>/contents/images/<width>x<height>.<ext>, metadata.json or metadata.desktop): its name
 * and the one picture that suits a room (landscape, nearest 16:9, then the largest); empty without one.
 */
QVariantMap wallpaperPackage(const QString &dir, const QStringList &filters)
{
    QString best;
    double bestMiss = 0.0;
    qint64 bestArea = 0;
    const QFileInfoList images = QDir(dir + QStringLiteral("/contents/images")).entryInfoList(filters, QDir::Files);
    for (const QFileInfo &f : images) {
        QSize size;
        const QStringList wh = f.completeBaseName().split(QLatin1Char('x'));
        if (wh.size() == 2) {
            size = QSize(wh[0].toInt(), wh[1].toInt());
        }
        if (size.width() <= 0 || size.height() <= 0) {
            size = QImageReader(f.filePath()).size();
        }
        if (size.width() < size.height() || size.height() <= 0) {
            continue; // portrait (phones)
        }
        const double miss = std::round(std::fabs(double(size.width()) / size.height() - 16.0 / 9.0) * 100.0);
        const qint64 area = qint64(size.width()) * size.height();
        if (best.isEmpty() || miss < bestMiss || (miss == bestMiss && area > bestArea)) {
            best = f.absoluteFilePath();
            bestMiss = miss;
            bestArea = area;
        }
    }
    if (best.isEmpty()) {
        return {};
    }
    QString name;
    QFile json(dir + QStringLiteral("/metadata.json"));
    if (json.open(QIODevice::ReadOnly)) {
        name = QJsonDocument::fromJson(json.readAll()).object().value(QStringLiteral("KPlugin")).toObject().value(QStringLiteral("Name")).toString();
    }
    QFile desktop(dir + QStringLiteral("/metadata.desktop"));
    if (name.isEmpty() && desktop.open(QIODevice::ReadOnly | QIODevice::Text)) {
        while (!desktop.atEnd() && name.isEmpty()) {
            const QString line = QString::fromUtf8(desktop.readLine()).trimmed();
            if (line.startsWith(QLatin1String("Name="))) {
                name = line.mid(5);
            }
        }
    }
    if (name.isEmpty()) {
        name = QFileInfo(dir).fileName();
    }
    return {{QStringLiteral("path"), best}, {QStringLiteral("name"), name}};
}

} // namespace

QString RiftSettings::userRoomsFolder() const
{
    const QString folder = qEnvironmentVariable("RIFT_HOME_USER_ROOMS");
    return folder.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::PicturesLocation) + QStringLiteral("/RiftHome") : folder;
}

QString RiftSettings::userRoomsName() const
{
    const QString folder = userRoomsFolder();
    return folder.startsWith(QDir::homePath() + QLatin1Char('/')) ? QDir::home().relativeFilePath(folder) : folder;
}

void RiftSettings::openUserRooms()
{
    const QString folder = userRoomsFolder();
    QDir().mkpath(folder);
    findPhotos(); // follows it from now on
    QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
}

void RiftSettings::findPhotos()
{
    const QStringList filters = roomNameFilters();
    m_photos.clear();
    QStringList seen;
    const auto add = [this, &seen](const QString &path, const QString &name, const QString &group) {
        if (seen.contains(path)) {
            return;
        }
        seen << path;
        m_photos << QVariantMap{{QStringLiteral("path"), path},
                                {QStringLiteral("name"), name},
                                {QStringLiteral("group"), group},
                                {QStringLiteral("hdr"), path.endsWith(QLatin1String(".hdr"), Qt::CaseInsensitive)}};
    };
    const auto addFolder = [&](const QString &dir, const QString &group) {
        const QFileInfoList files = QDir(dir).entryInfoList(filters, QDir::Files, QDir::Name | QDir::IgnoreCase);
        for (const QFileInfo &f : files.mid(0, 200)) {
            add(f.absoluteFilePath(), roomName(f), group);
        }
    };
    for (const QString &dir : qEnvironmentVariable("RIFT_HOME_ROOMS").split(QLatin1Char(':'), Qt::SkipEmptyParts)) {
        addFolder(dir, QStringLiteral("riftHome"));
    }
    const QString yours = userRoomsFolder();
    addFolder(yours, QStringLiteral("yours"));
    if (QFileInfo(yours).isDir() && !m_roomsWatcher.directories().contains(yours)) {
        m_roomsWatcher.addPath(yours);
    }
    // The desktop's wallpapers: Plasma's packages, folders of pictures (GNOME's backgrounds/<name>/), pictures. Each
    // once by name, packages first (CachyOS ships its wallpapers as packages and again, smaller, in a folder), by name.
    QStringList roots = qEnvironmentVariable("RIFT_HOME_WALLPAPER_DIRS").split(QLatin1Char(':'), Qt::SkipEmptyParts);
    if (roots.isEmpty()) {
        for (const QString &kind : {QStringLiteral("wallpapers"), QStringLiteral("backgrounds")}) {
            roots << QStandardPaths::locateAll(QStandardPaths::GenericDataLocation, kind, QStandardPaths::LocateDirectory);
        }
    }
    QList<std::pair<QString, QString>> wallpapers; // name, path
    QStringList names;
    const auto keyOf = [](const QString &name) {
        QString key = name.toLower();
        key.removeIf([](QChar c) {
            return !c.isLetterOrNumber();
        });
        return key;
    };
    // A package also counts under its folder's name: CachyOS's "Skyscraper" folder is named "Skyscrapers" inside, and
    // its loose copy is Skyscraper.png (both were offered).
    const auto addWallpaper = [&](const QString &path, const QString &name, const QString &alsoKnownAs = QString()) {
        const QString key = keyOf(name);
        if (names.contains(key) || (!alsoKnownAs.isEmpty() && names.contains(keyOf(alsoKnownAs)))) {
            return;
        }
        names << key;
        if (!alsoKnownAs.isEmpty()) {
            names << keyOf(alsoKnownAs);
        }
        wallpapers.append({name, path});
    };
    QFileInfoList folders;
    QFileInfoList files;
    for (const QString &root : std::as_const(roots)) {
        const QFileInfoList entries = QDir(root).entryInfoList(QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot, QDir::Name | QDir::IgnoreCase);
        for (const QFileInfo &entry : entries) {
            if (entry.isDir() && QFileInfo(entry.filePath() + QStringLiteral("/contents/images")).isDir()) {
                const QVariantMap package = wallpaperPackage(entry.absoluteFilePath(), filters);
                if (!package.isEmpty()) {
                    addWallpaper(package.value(QStringLiteral("path")).toString(), package.value(QStringLiteral("name")).toString(), entry.fileName());
                }
            } else if (entry.isDir()) {
                folders << entry;
            } else if (QDir::match(filters, entry.fileName())) {
                files << entry;
            }
        }
    }
    for (const QFileInfo &folder : std::as_const(folders)) {
        files << QDir(folder.absoluteFilePath()).entryInfoList(filters, QDir::Files, QDir::Name | QDir::IgnoreCase).mid(0, 200);
    }
    for (const QFileInfo &f : std::as_const(files)) {
        addWallpaper(f.absoluteFilePath(), roomName(f));
    }
    std::sort(wallpapers.begin(), wallpapers.end(), [](const auto &a, const auto &b) {
        return QString::localeAwareCompare(a.first, b.first) < 0;
    });
    for (const auto &[name, path] : std::as_const(wallpapers)) {
        add(path, name, QStringLiteral("wallpapers"));
    }
    // The room in use, wherever it is (chosen before these folders, or given on the command line).
    if (!roomPhoto().isEmpty() && QFileInfo(roomPhoto()).isFile()) {
        add(roomPhoto(), roomName(QFileInfo(roomPhoto())), QStringLiteral("yours"));
    }
    Q_EMIT photosChanged();
}

// --- System -------------------------------------------------------------------------------------------------------

System::System(QObject *parent)
    : QObject(parent)
{
    const auto row = [](const QString &label, const QString &value) {
        return QVariantMap{{QStringLiteral("label"), label}, {QStringLiteral("value"), value}};
    };
    m_about << row(QStringLiteral("Rift Home"), QStringLiteral(RIFT_HOME_VERSION)) << row(QStringLiteral("Computer"), QSysInfo::machineHostName())
            << row(QStringLiteral("System"), QSysInfo::prettyProductName()) << row(QStringLiteral("Kernel"), QSysInfo::kernelVersion())
            << row(QStringLiteral("Qt"), QString::fromLatin1(qVersion()));
    // The headset's runtime (RiftLinux's package, where pacman knows it).
    run(QStringLiteral("pacman"), {QStringLiteral("-Q"), QStringLiteral("riftlinux")}, [this, row](int code, const QString &out, const QString &) {
        if (code == 0) {
            m_about.insert(1, row(QStringLiteral("Headset runtime"), out.trimmed()));
            Q_EMIT changed();
        }
    });
}

void System::leaveRiftHome()
{
    const qint64 pid = qEnvironmentVariable("RIFT_HOME_PID").toLongLong();
    if (pid > 1) {
        ::kill(pid_t(pid), SIGTERM); // Rift Home asks its apps to quit, then ends
    }
}

void System::restart()
{
    run(QStringLiteral("systemctl"), {QStringLiteral("reboot")}, [](int, const QString &, const QString &) {});
}

void System::shutDown()
{
    run(QStringLiteral("systemctl"), {QStringLiteral("poweroff")}, [](int, const QString &, const QString &) {});
}

} // namespace RiftHome::Settings
