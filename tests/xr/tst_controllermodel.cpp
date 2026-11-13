// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
#include "xr/ControllerModel.hpp"

#include <QLoggingCategory>
#include <QTest>

#include <cmath>

Q_LOGGING_CATEGORY(lcXr, "rifthome.xr")

using namespace RiftHome::Xr;

namespace
{

QString model(const char *name)
{
    return QStringLiteral(RIFT_HOME_SOURCE_DIR "/data/controllers/") + QLatin1String(name);
}

//! The buttons' material ('04 | Buttons': the trigger and the grip button) by its base colour.
bool isButton(const ControllerVertex &v)
{
    return std::abs(v.colour[0] - 0.023f) < 0.002f && std::abs(v.colour[2] - 0.030f) < 0.002f;
}

} // namespace

class TestControllerModel : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void fittedIntoTheGrip_data()
    {
        QTest::addColumn<QString>("path");
        QTest::addColumn<bool>("left");
        QTest::newRow("right") << model("ControllerRight.glb") << false;
        QTest::newRow("left") << model("ControllerLeft.glb") << true;
    }

    void fittedIntoTheGrip()
    {
        QFETCH(QString, path);
        QFETCH(bool, left);
        const std::optional<ControllerMesh> mesh = loadControllerModel(path);
        QVERIFY(mesh);
        QVERIFY(mesh->indices.size() > 3000);
        // Along the handle (from its foot to the head): in grip space 30 degrees back from -Z (the front turned to the aim).
        const float c = std::cos(30.0f * float(M_PI) / 180.0f), s = std::sin(30.0f * float(M_PI) / 180.0f);
        float minZ = 1e9f, maxZ = -1e9f;
        double headY = 0, headN = 0, gripX = 0, gripN = 0;
        for (const ControllerVertex &v : mesh->vertices) {
            const float up = v.position[1] * s - v.position[2] * c;
            minZ = std::min(minZ, -up);
            maxZ = std::max(maxZ, -up);
            if (up > 0.015f) { // the head, above the grip point
                headY += v.position[1] * c + v.position[2] * s; // ahead is -Y turned with the model
                ++headN;
            }
            if (isButton(v) && -up > -0.005f && -up < 0.04f) { // the grip button, on the handle
                gripX += v.position[0];
                ++gripN;
            }
        }
        // A Touch's size: about 94 mm along the handle, the grip point 31 % below the top.
        QVERIFY2(std::abs((maxZ - minZ) - 0.094f) < 0.006f, qPrintable(QString::number(maxZ - minZ)));
        QVERIFY2(minZ < -0.024f && minZ > -0.036f, qPrintable(QString::number(minZ)));
        // The head leans forward (-Y ahead).
        QVERIFY(headN > 0 && headY / headN < 0.0);
        // The grip button is on the handle's inner side: left of a right controller (-X), right of a left one (+X).
        QVERIFY(gripN > 0);
        if (left) {
            QVERIFY2(gripX / gripN > 0.0, qPrintable(QString::number(gripX / gripN)));
        } else {
            QVERIFY2(gripX / gripN < 0.0, qPrintable(QString::number(gripX / gripN)));
        }
        // Normals unit length, colours as the materials have them.
        const ControllerVertex &v = mesh->vertices.front();
        QVERIFY(std::abs(std::sqrt(v.normal[0] * v.normal[0] + v.normal[1] * v.normal[1] + v.normal[2] * v.normal[2]) - 1.0f) < 1e-3f);
    }

    void notAModel()
    {
        QVERIFY(!loadControllerModel(QStringLiteral(RIFT_HOME_SOURCE_DIR "/data/controllers/README.md")));
        QVERIFY(!loadControllerModel(QStringLiteral("/nonexistent.glb")));
    }
};

QTEST_GUILESS_MAIN(TestControllerModel)
#include "tst_controllermodel.moc"
