// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QRegularExpression>
#include <QString>
#include <QStringList>

namespace RiftHome::Settings
{

/*!
 * What the system tools print, made data (the settings app wraps nmcli, pactl and timedatectl). Pure:
 * no processes, so they are tested on their own.
 */

//! One line of `nmcli -t` (terse): fields split at ':', with nmcli's escapes "\:" and "\\" undone.
inline QStringList splitTerse(const QString &line)
{
    QStringList fields;
    QString field;
    for (int i = 0; i < line.size(); ++i) {
        const QChar c = line[i];
        if (c == QLatin1Char('\\') && i + 1 < line.size()) {
            field += line[++i];
        } else if (c == QLatin1Char(':')) {
            fields << field;
            field.clear();
        } else {
            field += c;
        }
    }
    fields << field;
    return fields;
}

//! A Wi-Fi network as the settings list shows it: one per name (the strongest of its access points).
struct WifiNetwork {
    QString ssid;
    int signal = 0; //!< 0..100
    bool secure = false;
    bool active = false; //!< connected now
};

//! `nmcli -t -f IN-USE,SSID,SIGNAL,SECURITY device wifi list`: hidden networks (no name) left out, strongest first.
inline QList<WifiNetwork> parseWifiList(const QString &text)
{
    QList<WifiNetwork> networks;
    for (const QString &line : text.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        const QStringList f = splitTerse(line);
        if (f.size() < 4 || f[1].isEmpty()) {
            continue;
        }
        WifiNetwork n{f[1], f[2].toInt(), !f[3].isEmpty() && f[3] != QLatin1String("--"), f[0] == QLatin1String("*")};
        bool merged = false;
        for (WifiNetwork &known : networks) {
            if (known.ssid == n.ssid) {
                known.signal = std::max(known.signal, n.signal);
                known.active = known.active || n.active;
                known.secure = known.secure || n.secure;
                merged = true;
                break;
            }
        }
        if (!merged) {
            networks << n;
        }
    }
    std::stable_sort(networks.begin(), networks.end(), [](const WifiNetwork &a, const WifiNetwork &b) {
        return a.active != b.active ? a.active : a.signal > b.signal;
    });
    return networks;
}

/*!
 * `nmcli -t -f GENERAL.DEVICE,GENERAL.TYPE,... device show`: one map per device, keys as nmcli names them, numbered
 * keys ("IP4.ADDRESS[1]") without their number, several values joined by ", ".
 */
inline QList<QMap<QString, QString>> parseDeviceShow(const QString &text)
{
    QList<QMap<QString, QString>> devices;
    QMap<QString, QString> current;
    const auto flush = [&] {
        if (!current.isEmpty()) {
            devices << current;
            current.clear();
        }
    };
    for (const QString &line : text.split(QLatin1Char('\n'))) {
        if (line.trimmed().isEmpty()) {
            flush();
            continue;
        }
        const int colon = line.indexOf(QLatin1Char(':'));
        if (colon <= 0) {
            continue;
        }
        QString key = line.left(colon);
        if (key == QLatin1String("GENERAL.DEVICE")) {
            flush(); // nmcli -t separates devices by blank lines, but a new device always starts here
        }
        if (const int bracket = key.indexOf(QLatin1Char('[')); bracket > 0) {
            key = key.left(bracket);
        }
        const QString value = line.mid(colon + 1);
        current[key] = current.contains(key) && !value.isEmpty() ? current[key] + QStringLiteral(", ") + value : value;
    }
    flush();
    return devices;
}

//! A VPN NetworkManager knows (Proton VPN's and imported ones, in the network page).
struct VpnConnection {
    QString name;
    QString uuid;
    bool active = false;
    bool wireguard = false; //!< else an OpenVPN or other plugin VPN
};

//! `nmcli -t -f NAME,TYPE,UUID,ACTIVE connection show`: the VPN and WireGuard connections, active ones first.
inline QList<VpnConnection> parseVpnConnections(const QString &text)
{
    QList<VpnConnection> vpns;
    for (const QString &line : text.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        const QStringList f = splitTerse(line);
        if (f.size() < 4 || (f[1] != QLatin1String("vpn") && f[1] != QLatin1String("wireguard"))) {
            continue;
        }
        vpns << VpnConnection{f[0], f[2], f[3] == QLatin1String("yes"), f[1] == QLatin1String("wireguard")};
    }
    std::stable_sort(vpns.begin(), vpns.end(), [](const VpnConnection &a, const VpnConnection &b) {
        return a.active && !b.active;
    });
    return vpns;
}

//! `timedatectl show` (and other `key=value` output) as a map.
inline QMap<QString, QString> parseKeyValues(const QString &text)
{
    QMap<QString, QString> values;
    for (const QString &line : text.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        const int eq = line.indexOf(QLatin1Char('='));
        if (eq > 0) {
            values.insert(line.left(eq), line.mid(eq + 1));
        }
    }
    return values;
}

//! A sound device (PipeWire, through `pactl -f json list sinks|sources`).
struct AudioDevice {
    QString name; //!< what pactl calls it (to set it)
    QString description; //!< what people call it
    int volume = 0; //!< percent, the first channel's
    bool muted = false;
};

//! Outputs or inputs from `pactl -f json list sinks|sources`; monitors of outputs are left out of the inputs.
inline QList<AudioDevice> parseAudioDevices(const QByteArray &json)
{
    QList<AudioDevice> devices;
    for (const QJsonValue &v : QJsonDocument::fromJson(json).array()) {
        const QJsonObject o = v.toObject();
        const QString name = o.value(QStringLiteral("name")).toString();
        if (name.isEmpty() || name.endsWith(QLatin1String(".monitor"))) {
            continue;
        }
        AudioDevice d;
        d.name = name;
        d.description = o.value(QStringLiteral("description")).toString(name);
        d.muted = o.value(QStringLiteral("mute")).toBool();
        const QJsonObject volume = o.value(QStringLiteral("volume")).toObject();
        if (!volume.isEmpty()) {
            const QString percent = volume.begin().value().toObject().value(QStringLiteral("value_percent")).toString();
            d.volume = QString(percent).remove(QLatin1Char('%')).trimmed().toInt();
        }
        devices << d;
    }
    return devices;
}

//! A Bluetooth device as `bluetoothctl devices` lists it ("Device AA:BB:CC:DD:EE:FF Name").
struct BluetoothDevice {
    QString address;
    QString name;
};

inline QList<BluetoothDevice> parseBluetoothDevices(const QString &text)
{
    QList<BluetoothDevice> devices;
    static const QRegularExpression line(QStringLiteral("^Device ([0-9A-Fa-f:]{17}) ?(.*)$"));
    for (const QString &l : text.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        const QRegularExpressionMatch m = line.match(l.trimmed());
        if (m.hasMatch()) {
            const QString name = m.captured(2).trimmed();
            devices << BluetoothDevice{m.captured(1).toUpper(), name.isEmpty() ? m.captured(1).toUpper() : name};
        }
    }
    return devices;
}

//! `bluetoothctl show` / `info` ("\tPowered: yes" lines) as a map; the first line's own words are left out.
inline QMap<QString, QString> parseColonFields(const QString &text)
{
    QMap<QString, QString> fields;
    for (const QString &l : text.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        const QString t = l.trimmed();
        const int colon = t.indexOf(QLatin1Char(':'));
        if (colon > 0 && l.startsWith(QLatin1Char('\t'))) {
            fields.insert(t.left(colon).trimmed(), t.mid(colon + 1).trimmed());
        }
    }
    return fields;
}

//! An IPv4 address ("192.168.0.20"): four numbers 0..255.
inline bool validIPv4(const QString &text)
{
    const QStringList parts = text.trimmed().split(QLatin1Char('.'));
    if (parts.size() != 4) {
        return false;
    }
    for (const QString &part : parts) {
        bool ok = false;
        const int n = part.toInt(&ok);
        if (!ok || part.isEmpty() || part.size() > 3 || n < 0 || n > 255) {
            return false;
        }
    }
    return true;
}

//! DNS servers as typed ("1.1.1.1, 9.9.9.9" or spaces): the addresses, or nothing when one is not an address.
inline QStringList parseDnsServers(const QString &text)
{
    QStringList servers;
    for (const QString &part : text.split(QRegularExpression(QStringLiteral("[,;\\s]+")), Qt::SkipEmptyParts)) {
        if (!validIPv4(part)) {
            return {};
        }
        servers << part;
    }
    return servers;
}

} // namespace RiftHome::Settings
