// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
#include "settings/Parse.hpp"

#include <QTest>

using namespace RiftHome::Settings;

class TestParse : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void terseFieldsWithEscapes()
    {
        // nmcli -t escapes ':' and '\' in values (a BSSID, a name with a colon).
        QCOMPARE(splitTerse(QStringLiteral("*:Home\\:5G:72:WPA2")), (QStringList{QStringLiteral("*"), QStringLiteral("Home:5G"), QStringLiteral("72"), QStringLiteral("WPA2")}));
        QCOMPARE(splitTerse(QStringLiteral("a\\\\b::c")), (QStringList{QStringLiteral("a\\b"), QString(), QStringLiteral("c")}));
    }

    void wifiListOnePerNameStrongestFirst()
    {
        const QString out = QStringLiteral(" :Cafe:40:\n"
                                           " :Home:55:WPA2\n"
                                           "*:Home:80:WPA2\n"
                                           " ::90:WPA2\n" // hidden: no name
                                           " :Neighbour:91:WPA1 WPA2\n");
        const QList<WifiNetwork> n = parseWifiList(out);
        QCOMPARE(n.size(), 3);
        QCOMPARE(n[0].ssid, QStringLiteral("Home")); // connected first
        QVERIFY(n[0].active && n[0].secure && n[0].signal == 80);
        QCOMPARE(n[1].ssid, QStringLiteral("Neighbour"));
        QCOMPARE(n[2].ssid, QStringLiteral("Cafe"));
        QVERIFY(!n[2].secure);
    }

    void devicesWithAddresses()
    {
        const QString out = QStringLiteral("GENERAL.DEVICE:enp5s0\n"
                                           "GENERAL.TYPE:ethernet\n"
                                           "GENERAL.STATE:100 (connected)\n"
                                           "GENERAL.CONNECTION:Wired connection 1\n"
                                           "IP4.ADDRESS[1]:192.0.2.47/24\n"
                                           "IP4.ADDRESS[2]:198.51.100.2/24\n"
                                           "\n"
                                           "GENERAL.DEVICE:wlan0\n"
                                           "GENERAL.TYPE:wifi\n"
                                           "GENERAL.STATE:30 (disconnected)\n"
                                           "GENERAL.CONNECTION:\n");
        const auto d = parseDeviceShow(out);
        QCOMPARE(d.size(), 2);
        QCOMPARE(d[0].value(QStringLiteral("GENERAL.CONNECTION")), QStringLiteral("Wired connection 1"));
        QCOMPARE(d[0].value(QStringLiteral("IP4.ADDRESS")), QStringLiteral("192.0.2.47/24, 198.51.100.2/24"));
        QCOMPARE(d[1].value(QStringLiteral("GENERAL.TYPE")), QStringLiteral("wifi"));
    }

    void vpnConnections()
    {
        const QString out = QStringLiteral("Wired connection 1:802-3-ethernet:1b2c:yes\n"
                                           "Home:802-11-wireless:3d4e:no\n"
                                           "ProtonVPN CH#12:wireguard:5f6a:no\n"
                                           "pvpn-killswitch:dummy:7b8c:no\n"
                                           "Work\\: office:vpn:9d0e:yes\n");
        const QList<VpnConnection> v = parseVpnConnections(out);
        QCOMPARE(v.size(), 2);
        QCOMPARE(v[0].name, QStringLiteral("Work: office")); // active first
        QVERIFY(v[0].active && !v[0].wireguard);
        QCOMPARE(v[1].uuid, QStringLiteral("5f6a"));
        QVERIFY(v[1].wireguard && !v[1].active);
    }

    void timedatectlShow()
    {
        const auto v = parseKeyValues(QStringLiteral("Timezone=Australia/Adelaide\nNTP=yes\nNTPSynchronized=yes\n"));
        QCOMPARE(v.value(QStringLiteral("Timezone")), QStringLiteral("Australia/Adelaide"));
        QCOMPARE(v.value(QStringLiteral("NTP")), QStringLiteral("yes"));
    }

    void addressesAndDns()
    {
        QVERIFY(validIPv4(QStringLiteral("192.0.2.47")));
        QVERIFY(validIPv4(QStringLiteral(" 198.51.100.1 ")));
        QVERIFY(!validIPv4(QStringLiteral("192.168.0")));
        QVERIFY(!validIPv4(QStringLiteral("192.0.2.256")));
        QVERIFY(!validIPv4(QStringLiteral("192.168.a.1")));
        QCOMPARE(parseDnsServers(QStringLiteral("1.1.1.1, 9.9.9.9")), (QStringList{QStringLiteral("1.1.1.1"), QStringLiteral("9.9.9.9")}));
        QCOMPARE(parseDnsServers(QStringLiteral("8.8.8.8 8.8.4.4")).size(), 2);
        QVERIFY(parseDnsServers(QStringLiteral("1.1.1.1, nope")).isEmpty());
    }

    void bluetooth()
    {
        const auto d = parseBluetoothDevices(QStringLiteral("Device 00:1A:7D:DA:71:13 WH-1000XM4\nDevice 7C:96:D2:11:22:33\nnot a device\n"));
        QCOMPARE(d.size(), 2);
        QCOMPARE(d[0].name, QStringLiteral("WH-1000XM4"));
        QCOMPARE(d[1].name, QStringLiteral("7C:96:D2:11:22:33")); // no name: its address
        const auto f = parseColonFields(QStringLiteral("Controller 00:1A:7D:DA:71:10 (public)\n\tName: box\n\tPowered: yes\n\tDiscovering: no\n"));
        QCOMPARE(f.value(QStringLiteral("Powered")), QStringLiteral("yes"));
        QCOMPARE(f.value(QStringLiteral("Name")), QStringLiteral("box"));
        QVERIFY(!f.contains(QStringLiteral("Controller 00")));
    }

    void audioDevicesFromPactlJson()
    {
        const QByteArray json = R"([
          {"index":56,"name":"alsa_output.usb-Oculus_Rift_Audio-00.analog-stereo","description":"Rift Audio Analog Stereo","mute":false,
           "volume":{"front-left":{"value":27525,"value_percent":"42%","db":"-22.61 dB"},"front-right":{"value":27525,"value_percent":"42%","db":"-22.61 dB"}}},
          {"index":57,"name":"alsa_output.usb-Oculus_Rift_Audio-00.analog-stereo.monitor","description":"Monitor of Rift Audio","mute":false,"volume":{}},
          {"index":58,"name":"alsa_output.pci.iec958-stereo","description":"Built-in Audio Digital Stereo","mute":true,
           "volume":{"front-left":{"value":65536,"value_percent":"100%","db":"0.00 dB"}}}
        ])";
        const QList<AudioDevice> d = parseAudioDevices(json);
        QCOMPARE(d.size(), 2);
        QCOMPARE(d[0].description, QStringLiteral("Rift Audio Analog Stereo"));
        QCOMPARE(d[0].volume, 42);
        QVERIFY(!d[0].muted && d[1].muted && d[1].volume == 100);
    }
};

QTEST_GUILESS_MAIN(TestParse)
#include "tst_parse.moc"
