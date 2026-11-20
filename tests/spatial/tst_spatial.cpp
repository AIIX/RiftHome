// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
#include "spatial/Controls.hpp"
#include "spatial/PointerFilter.hpp"
#include "spatial/Recline.hpp"
#include "spatial/Sharpness.hpp"
#include "spatial/SpatialLayout.hpp"
#include "spatial/Dock.hpp"
#include "spatial/Gamepad.hpp"
#include "spatial/LauncherGrid.hpp"
#include "spatial/NotificationPanel.hpp"
#include "spatial/WindowFrame.hpp"

#include <QJsonArray>
#include <QJsonObject>
#include <QTest>

#include <cmath>

using namespace RiftHome::Spatial;

namespace
{

Pose headAt(float azimuthDegrees = 0.0f)
{
    // Facing azimuth a: forward (sin a, 0, -cos a) is a rotation about +Y by -a.
    return {Quat::fromYaw(-radians(azimuthDegrees)), {0.0f, 0.0f, 0.0f}};
}

bool near(float a, float b, float eps = 1e-3f)
{
    return std::fabs(a - b) <= eps;
}

bool near(Vec3 a, Vec3 b, float eps = 1e-3f)
{
    return (a - b).length() <= eps;
}

//! Lying on one's back, the feet to the front (-Z): looking straight up, the top of the head to the back (+Z).
Pose lyingBack()
{
    return {Quat::fromBasis({1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, -1.0f, 0.0f}), {0.0f, 0.4f, 0.0f}};
}

//! Turned about the view: positive to the right.
Pose rolled(Pose head, float degrees)
{
    const float a = radians(degrees) / 2.0f;
    head.orientation = head.orientation * Quat{0.0f, 0.0f, -std::sin(a), std::cos(a)};
    return head;
}

} // namespace

class TestSpatial : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void grabbedWindowGoesUpByAngleNearOrFar()
    {
        // How high a window goes follows the ray up to 45 degrees from the eyes, near or far (2 m would be 29 degrees at
        // 3.56 m and 14 at 8 m), and that for its middle wherever it is held. A hand below and in front of the eyes.
        const auto aimAt = [](Vec3 from, Vec3 to) {
            const Vec3 d = to - from;
            return Pose{Quat::fromYaw(-azimuthOf(d)) * Quat::fromPitch(std::atan2(d.y, std::sqrt(d.x * d.x + d.z * d.z))), from};
        };
        const Pose head = headAt();
        const Vec3 hand{0.2f, -0.35f, -0.3f};
        for (const float curve : {0.5f, 0.0f}) {
            for (const float distance : {1.6f, 3.56f, 8.0f}) {
                for (const float heldBelowMiddle : {0.0f, 0.4f}) { // at its middle, and low on the window
                    SpatialLayout layout;
                    layout.setArrangementCurve(curve);
                    layout.sync({{1, "dolphin", 1862, 1042, false, 2.0f}}, head);
                    const auto centre = [&] {
                        return layout.windows().at(1).pose.position;
                    };
                    const auto flat = [&] {
                        return std::sqrt(centre().x * centre().x + centre().z * centre().z);
                    };
                    const Vec3 held = centre() - Vec3{0.0f, heldBelowMiddle, 0.0f};
                    QVERIFY(layout.beginGrab(1, aimAt(hand, held), (held - hand).length()));
                    for (int i = 0; i < 2000 && flat() < distance; ++i) { // pushed back with the thumbstick
                        layout.updateGrab(aimAt(hand, centre() - Vec3{0.0f, heldBelowMiddle, 0.0f}), head, 1.0f, 0.0f, 0.011f);
                    }
                    const float d = flat();
                    QVERIFY(std::fabs(d - distance) < 0.15f);
                    const auto aimUp = [&](float degrees) { // at where the held point goes for the middle this many degrees up
                        return aimAt(hand, {0.0f, d * std::tan(radians(degrees)) - heldBelowMiddle, -d});
                    };
                    const auto elevation = [&] {
                        return std::atan2(centre().y, flat()) * 180.0f / float(M_PI);
                    };
                    for (const float degrees : {30.0f, 44.0f, -30.0f, -44.0f}) {
                        layout.updateGrab(aimUp(degrees), head, 0.0f, 0.0f, 0.011f);
                        QVERIFY(std::fabs(elevation() - degrees) < 2.0f);
                    }
                    // Beyond the limit it stays there, and comes back with the ray.
                    layout.updateGrab(aimUp(60.0f), head, 0.0f, 0.0f, 0.011f);
                    QVERIFY(std::fabs(elevation() - 45.0f) < 1.5f);
                    layout.updateGrab(Pose{Quat::fromYaw(0.0f) * Quat::fromPitch(radians(89.0f)), hand}, head, 0.0f, 0.0f, 0.011f);
                    QVERIFY(std::fabs(elevation() - 45.0f) < 1.5f);
                    QVERIFY(flat() < d + 1.0f);
                    layout.updateGrab(aimUp(-60.0f), head, 0.0f, 0.0f, 0.011f);
                    QVERIFY(std::fabs(elevation() + 45.0f) < 1.5f);
                    layout.updateGrab(aimUp(20.0f), head, 0.0f, 0.0f, 0.011f);
                    QVERIFY(std::fabs(elevation() - 20.0f) < 2.0f);
                    layout.endGrab();
                }
            }
        }
    }

    void newWindowOpensInFront()
    {
        SpatialLayout layout;
        layout.sync({{1, "mpv", 1920, 1080}}, headAt());
        const SpatialWindow &w = layout.windows().at(1);
        QVERIFY(near(w.pose.position.x, 0.0f));
        QVERIFY(near(w.pose.position.y, -0.1f));
        QVERIFY(near(w.pose.position.z, -1.6f));
        // Its front faces the user.
        const Vec3 normal = w.pose.orientation.rotate({0, 0, 1});
        QVERIFY(near(normal.z, 1.0f));
        QVERIFY(near(layout.widthMeters(w), 1.92f));
    }

    void veryWideNewWindowOpensAtAComfortableWidth()
    {
        // Steam at the X screen's 2560 pixels: 2 m wide, not 2.56 m; a remembered scale is the user's.
        SpatialLayout layout;
        layout.sync({{1, "steam", 2560, 1440}}, headAt());
        QVERIFY(near(layout.widthMeters(layout.windows().at(1)), 2.0f));
        layout.sync({{1, "steam", 2560, 1440}, {2, "dolphin", 1280, 800}}, headAt());
        QVERIFY(near(layout.widthMeters(layout.windows().at(2)), 1.28f));
    }

    void placesKeepTheirSizeAcrossAppScales()
    {
        // HiDPI: Dolphin made 4x big when it drew 931 pixels a row (placements without pixelScale,
        // from before) comes back as wide in the room when it draws twice the pixels (apps at 200 %): scale 2. Steam
        // (X11, pixel scale 1) keeps its own. A new window of 1461 units at 200 % (2922 pixels) opens at most 2 m wide.
        QJsonObject windows;
        windows.insert(QStringLiteral("dolphin"), QJsonObject{{QStringLiteral("position"), QJsonArray{0.0, -0.1, -2.5}}, {QStringLiteral("scale"), 4.0}});
        windows.insert(QStringLiteral("steam"), QJsonObject{{QStringLiteral("position"), QJsonArray{1.0, -0.1, -2.0}}, {QStringLiteral("scale"), 1.5}});
        SpatialLayout layout;
        layout.load(QJsonObject{{QStringLiteral("version"), 2}, {QStringLiteral("windows"), windows}});
        layout.sync({{1, "dolphin", 1862, 1042, false, 2.0f}, {2, "steam", 2560, 1440, false, 1.0f}, {3, "firefox", 2922, 1842, false, 2.0f}},
                    headAt());
        QVERIFY(near(layout.windows().at(1).scale, 2.0f));
        QVERIFY(near(layout.widthMeters(layout.windows().at(1)), 3.724f));
        QVERIFY(near(layout.windows().at(2).scale, 1.5f));
        QVERIFY(near(layout.widthMeters(layout.windows().at(3)), 2.0f));
        // Saved with its pixel scale now: at 100 % again it comes back as wide (scale 4 on 931 pixels).
        SpatialLayout restarted;
        restarted.load(layout.save());
        restarted.sync({{1, "dolphin", 931, 521, false, 1.0f}}, headAt());
        QVERIFY(near(restarted.windows().at(1).scale, 4.0f));
    }

    void goingFullscreenMovesWindowsInTheWayAside()
    {
        // A game started from Steam: it opens in front, where Steam is, and goes fullscreen: its
        // screen is farther away, so Steam moves aside instead of hiding it.
        SpatialLayout layout;
        layout.setOpenInFront(true);
        layout.sync({{1, "steam", 2560, 1440}}, headAt());
        layout.sync({{1, "steam", 2560, 1440}, {2, "game", 1280, 720}}, headAt());
        layout.sync({{1, "steam", 2560, 1440}, {2, "game", 2560, 1440, true}}, headAt());
        const SpatialWindow &steam = layout.windows().at(1);
        const SpatialWindow &game = layout.windows().at(2);
        const auto flat = [](Vec3 v) {
            return std::sqrt(v.x * v.x + v.z * v.z);
        };
        const float steamHalf = std::atan(layout.widthMeters(steam) / 2.0f / flat(steam.pose.position));
        const float gameHalf = std::atan(layout.widthMeters(game) / 2.0f / flat(game.pose.position));
        QVERIFY(std::fabs(angleDifference(azimuthOf(steam.pose.position), azimuthOf(game.pose.position))) >= steamHalf + gameHalf);
        QVERIFY(near(azimuthOf(game.pose.position), 0.0f)); // the game stays in front
    }

    void flatterCurveTurnsWindowsLess()
    {
        // The curve slider: fully curved, a window beside the first turns well toward the user; flat,
        // the two stand side by side like a wall, the same way, as far ahead.
        const auto yawOf = [](const SpatialWindow &w) {
            return azimuthOf(w.pose.orientation.rotate({0.0f, 0.0f, -1.0f}));
        };
        for (const float curve : {1.0f, 0.5f, 0.0f}) {
            SpatialLayout layout;
            layout.setArrangementCurve(curve);
            layout.sync({{1, "chromium", 1600, 1000}}, headAt());
            layout.sync({{1, "chromium", 1600, 1000}, {2, "dolphin", 1200, 800}}, headAt());
            const SpatialWindow &first = layout.windows().at(1);
            const SpatialWindow &second = layout.windows().at(2);
            QVERIFY(near(yawOf(first), 0.0f)); // straight ahead: facing the user whatever the curve
            const float turned = std::fabs(yawOf(second));
            const float edgeGap = std::fabs(second.pose.position.x - first.pose.position.x)
                - (layout.widthMeters(first) + layout.widthMeters(second)) / 2.0f * std::cos(turned);
            QVERIFY2(edgeGap > 0.0f, "side by side, not overlapping");
            if (curve == 1.0f) {
                QVERIFY(turned > radians(40.0f)); // about 50 degrees
            } else if (curve == 0.0f) {
                QVERIFY(turned < radians(10.0f)); // nearly flat: a very wide arc
                QVERIFY(near(second.pose.position.z, first.pose.position.z, 0.2f));
            } else {
                QVERIFY(turned > radians(15.0f) && turned < radians(35.0f)); // about half
            }
        }
    }

    void movingAlongAFlatArrangement()
    {
        // Flat: a window moved to the right slides along the wall, still facing the same way, as far ahead.
        SpatialLayout layout;
        layout.setArrangementCurve(0.0f);
        layout.sync({{1, "mpv", 1920, 1080}}, headAt());
        const Pose start = layout.windows().at(1).pose;
        const Pose controller{{}, {0, -0.1f, 0}};
        const auto hit = layout.hitTest(controller.position, controller.forward());
        QVERIFY(hit);
        QVERIFY(layout.beginGrab(1, controller, (layout.windows().at(1).pose.position - controller.position).length()));
        const Pose turned{Quat::fromYaw(-radians(25.0f)), {0, -0.1f, 0}};
        for (int i = 0; i < 3; ++i) {
            layout.updateGrab(turned, headAt(), 0.0f, 0.0f, 0.016f);
        }
        const SpatialWindow &moved = layout.windows().at(1);
        QVERIFY(moved.pose.position.x > 0.5f);
        QVERIFY(near(moved.pose.position.z, start.position.z, 0.15f));
        QVERIFY(std::fabs(azimuthOf(moved.pose.orientation.rotate({0.0f, 0.0f, -1.0f}))) < radians(8.0f));
        layout.endGrab();
    }

    void movedBackToTheMiddleFacesTheUserAgain()
    {
        // With the curve slider, a window moved aside and back to the middle stayed turned until
        // grabbed a second time (the arc was taken along the window's own direction).
        const auto yawOf = [](const SpatialWindow &w) {
            return azimuthOf(w.pose.orientation.rotate({0.0f, 0.0f, -1.0f}));
        };
        for (const float curve : {0.5f, 0.0f}) {
            SpatialLayout layout;
            layout.setArrangementCurve(curve);
            layout.sync({{1, "chromium", 1600, 1000}}, headAt());
            layout.sync({{1, "chromium", 1600, 1000}, {2, "dolphin", 1200, 800}}, headAt());
            // Grab the side window and bring it to the middle.
            const float side = azimuthOf(layout.windows().at(2).pose.position);
            const Pose start{Quat::fromYaw(-side), {0, -0.1f, 0}};
            const auto hit = layout.hitTest(start.position, start.forward());
            QVERIFY(hit && hit->windowId == 2);
            QVERIFY(layout.beginGrab(2, start, (layout.windows().at(2).pose.position - start.position).length()));
            for (float a = side; a > 0.0f; a -= 0.05f) {
                layout.updateGrab({Quat::fromYaw(-a), {0, -0.1f, 0}}, headAt(), 0.0f, 0.0f, 0.016f);
            }
            layout.updateGrab({Quat::fromYaw(0.0f), {0, -0.1f, 0}}, headAt(), 0.0f, 0.0f, 0.016f);
            layout.endGrab();
            const SpatialWindow &moved = layout.windows().at(2);
            QVERIFY2(std::fabs(yawOf(moved)) < radians(1.0f), "faces the user squarely in the middle");
            QVERIFY(std::fabs(moved.pose.position.x) < 0.05f);
        }
    }

    //! The inputs of a frame as a mask.
    static InputMask down(std::initializer_list<ControllerInput> inputs)
    {
        InputMask mask = 0;
        for (const ControllerInput input : inputs) {
            mask |= inputBit(input);
        }
        return mask;
    }
    static Binding bind(ControllerInput a, std::optional<ControllerInput> b = std::nullopt)
    {
        return *Binding::make(a, b);
    }

    void bindingNamesAndLabels()
    {
        QCOMPARE(bind(ControllerInput::LeftSecondary).name(), QStringLiteral("leftSecondary"));
        QCOMPARE(bind(ControllerInput::LeftSecondary).label(), QStringLiteral("Y"));
        QCOMPARE(bind(ControllerInput::LeftMenu).label(), QString::fromUtf8("☰"));
        // A chord is one binding whatever the order; grips first.
        const Binding chord = bind(ControllerInput::RightSecondary, ControllerInput::LeftGrip);
        QCOMPARE(chord, bind(ControllerInput::LeftGrip, ControllerInput::RightSecondary));
        QCOMPARE(chord.name(), QStringLiteral("leftGrip+rightSecondary"));
        QCOMPARE(chord.label(), QStringLiteral("Left grip + B"));
        QCOMPARE(Binding::fromName(QStringLiteral("rightSecondary+leftGrip")), std::optional<Binding>(chord));
        // A grip alone grabs, a chord of one input is none, and the triggers are not inputs.
        QVERIFY(!Binding::make(ControllerInput::LeftGrip));
        QVERIFY(!Binding::make(ControllerInput::RightPrimary, ControllerInput::RightPrimary));
        QVERIFY(!Binding::fromName(QStringLiteral("leftTrigger")));
        QVERIFY(!Binding::fromName(QStringLiteral("rightGrip")));
        QVERIFY(!Binding::fromName(QStringLiteral("leftPrimary+rightPrimary+leftMenu")));
        QVERIFY(!Binding::fromName(QString()));
        ControllerInput input{};
        for (std::size_t i = 0; i < kControllerInputs; ++i) {
            QVERIFY(ButtonMap::inputFromName(ButtonMap::inputName(ControllerInput(i)), &input));
            QCOMPARE(input, ControllerInput(i));
        }
        ButtonAction parsed = ButtonAction::Home;
        for (std::size_t i = 0; i < kButtonActions; ++i) {
            QVERIFY(ButtonMap::actionFromName(ButtonMap::name(ButtonAction(i)), &parsed));
            QCOMPARE(parsed, ButtonAction(i));
        }
        QVERIFY(!ButtonMap::actionFromName(QStringLiteral("teleport"), &parsed));
    }

    void buttonMapDefaults()
    {
        // Only what exists today: everything else is for the user to give.
        const ButtonMap map = ButtonMap::defaults();
        QCOMPARE(map.bindings(ButtonAction::Home), (std::vector<Binding>{bind(ControllerInput::LeftSecondary), bind(ControllerInput::LeftMenu)}));
        QCOMPARE(map.bindings(ButtonAction::Dock), (std::vector<Binding>{bind(ControllerInput::LeftPrimary)}));
        QCOMPARE(map.bindings(ButtonAction::RightClick).size(), std::size_t(3));
        QCOMPARE(map.bindings(ButtonAction::Recline).size(), std::size_t(2));
        QVERIFY(map.bindings(ButtonAction::Recline)[0].isChord());
        for (const ButtonAction none : {ButtonAction::LeftClick, ButtonAction::MiddleClick, ButtonAction::Keyboard, ButtonAction::Screenshot,
                                        ButtonAction::FollowAll, ButtonAction::GameMode, ButtonAction::Settings, ButtonAction::Notifications,
                                        ButtonAction::VolumeUp, ButtonAction::VolumeDown, ButtonAction::Mute}) {
            QVERIFY(map.bindings(none).empty());
        }
        QCOMPARE(ButtonMap::fromJson({}), map);
        QVERIFY(map.isDefault(ButtonAction::Home));
        // No binding twice among the defaults.
        for (std::size_t a = 0; a < kButtonActions; ++a) {
            for (const Binding &binding : map.bindings(ButtonAction(a))) {
                QCOMPARE(map.actionOf(binding), std::optional<ButtonAction>(ButtonAction(a)));
            }
        }
    }

    void buttonMapJsonRoundTrip()
    {
        // Only the differences are written; the rest is by default, and [] is none.
        ButtonMap map = ButtonMap::defaults();
        QCOMPARE(map.toJson().value(QStringLiteral("bindings")).toObject().size(), 0);
        map.clear(ButtonAction::Dock);
        map.assign(ButtonAction::Screenshot, bind(ControllerInput::LeftGrip, ControllerInput::LeftSecondary));
        const QJsonObject json = map.toJson();
        QCOMPARE(json.value(QStringLiteral("version")).toInt(), 2);
        const QJsonObject bindings = json.value(QStringLiteral("bindings")).toObject();
        QVERIFY(bindings.value(QStringLiteral("dock")).toArray().isEmpty());
        QVERIFY(bindings.contains(QStringLiteral("dock")));
        QVERIFY(bindings.contains(QStringLiteral("screenshot")));
        QVERIFY(!bindings.contains(QStringLiteral("rightClick")));
        QCOMPARE(ButtonMap::fromJson(json), map);
        // The chord is its own binding: Y alone is Home's still.
        QVERIFY(map.isDefault(ButtonAction::Home));
        QVERIFY(!map.isDefault(ButtonAction::Dock));
        // Hand-written: names in any order, unknown ones skipped, a binding twice or in two actions kept by the first.
        const ButtonMap own = ButtonMap::fromJson(QJsonObject{
            {QStringLiteral("version"), 2},
            {QStringLiteral("bindings"),
             QJsonObject{{QStringLiteral("keyboard"), QJsonArray{QStringLiteral("rightSecondary+leftGrip"), QStringLiteral("teleport"), QStringLiteral("rightGrip")}},
                         {QStringLiteral("mute"), QJsonArray{QStringLiteral("leftGrip+rightSecondary")}}}}});
        QCOMPARE(own.bindings(ButtonAction::Keyboard), (std::vector<Binding>{bind(ControllerInput::LeftGrip, ControllerInput::RightSecondary)}));
        QVERIFY(own.bindings(ButtonAction::Mute).empty());
        // Taken from recline, which had it by default: it has the other one left.
        QCOMPARE(own.bindings(ButtonAction::Recline).size(), std::size_t(1));
    }

    void oldButtonFormatIsConverted()
    {
        // The first format: button -> action, no version. Not listed: as it was by default.
        const ButtonMap defaults = ButtonMap::defaults();
        QCOMPARE(ButtonMap::fromJson(QJsonObject{}), defaults);
        // A file of the old format: B a click.
        const ButtonMap own = ButtonMap::fromJson(QJsonObject{{QStringLiteral("rightB"), QStringLiteral("leftClick")}});
        QCOMPARE(own.bindings(ButtonAction::LeftClick), (std::vector<Binding>{bind(ControllerInput::RightSecondary)}));
        QCOMPARE(own.bindings(ButtonAction::Home), defaults.bindings(ButtonAction::Home));
        QCOMPARE(own.bindings(ButtonAction::Dock), defaults.bindings(ButtonAction::Dock));
        QVERIFY(own.isDefault(ButtonAction::RightClick)); // the same three, in the order of its buttons
        QVERIFY(own.toJson().value(QStringLiteral("bindings")).toObject().contains(QStringLiteral("leftClick")));
        QVERIFY(!own.toJson().value(QStringLiteral("bindings")).toObject().contains(QStringLiteral("rightClick")));
        QCOMPARE(own.bindings(ButtonAction::Recline), defaults.bindings(ButtonAction::Recline)); // the combo stays
        // Moved and cleared ones; "none" is none, an unknown name is the default.
        const ButtonMap moved = ButtonMap::fromJson(QJsonObject{{QStringLiteral("leftY"), QStringLiteral("keyboard")},
                                                                {QStringLiteral("leftX"), QStringLiteral("none")},
                                                                {QStringLiteral("rightA"), QStringLiteral("teleport")},
                                                                {QStringLiteral("rightB"), QStringLiteral("screenshot")}});
        QCOMPARE(moved.bindings(ButtonAction::Keyboard), (std::vector<Binding>{bind(ControllerInput::LeftSecondary)}));
        QCOMPARE(moved.bindings(ButtonAction::Home), (std::vector<Binding>{bind(ControllerInput::LeftMenu)}));
        QVERIFY(moved.bindings(ButtonAction::Dock).empty());
        QCOMPARE(moved.bindings(ButtonAction::Screenshot), (std::vector<Binding>{bind(ControllerInput::RightSecondary)}));
        QVERIFY(moved.actionOf(bind(ControllerInput::RightPrimary)) == ButtonAction::RightClick);
        // Saved again it is the new format.
        QCOMPARE(ButtonMap::fromJson(moved.toJson()), moved);
        QCOMPARE(moved.toJson().value(QStringLiteral("version")).toInt(), 2);
    }

    void aBindingBelongsToOneAction()
    {
        ButtonMap map = ButtonMap::defaults();
        // X is the dock's: given to the keyboard it moves, and Settings can tell from where.
        QCOMPARE(map.assign(ButtonAction::Keyboard, bind(ControllerInput::LeftPrimary)), std::optional<ButtonAction>(ButtonAction::Dock));
        QVERIFY(map.bindings(ButtonAction::Dock).empty());
        QCOMPARE(map.actionOf(bind(ControllerInput::LeftPrimary)), std::optional<ButtonAction>(ButtonAction::Keyboard));
        // To the action that has it: nothing changes.
        QVERIFY(!map.assign(ButtonAction::Keyboard, bind(ControllerInput::LeftPrimary)));
        QCOMPARE(map.bindings(ButtonAction::Keyboard).size(), std::size_t(1));
        // A chord and a button of it are two bindings.
        QVERIFY(!map.assign(ButtonAction::Mute, bind(ControllerInput::LeftPrimary, ControllerInput::RightPrimary)));
        map.setBindings(ButtonAction::Screenshot, {bind(ControllerInput::LeftMenu)});
        QCOMPARE(map.actionOf(bind(ControllerInput::LeftMenu)), std::optional<ButtonAction>(ButtonAction::Screenshot));
        QCOMPARE(map.bindings(ButtonAction::Home), (std::vector<Binding>{bind(ControllerInput::LeftSecondary)}));
    }

    void homeCanAlwaysBeReached()
    {
        // Nothing leads to Home or the dock: the menu button opens Home, whatever it was given.
        ButtonMap map = ButtonMap::defaults();
        map.clear(ButtonAction::Home);
        map.clear(ButtonAction::Dock);
        map.assign(ButtonAction::Screenshot, bind(ControllerInput::LeftMenu));
        const ButtonMap effective = map.effective();
        QCOMPARE(effective.bindings(ButtonAction::Home), (std::vector<Binding>{bind(ControllerInput::LeftMenu)}));
        QVERIFY(effective.bindings(ButtonAction::Screenshot).empty());
        QCOMPARE(effective.bindings(ButtonAction::RightClick), map.bindings(ButtonAction::RightClick));
        // The dock leads home too: then it is as set.
        map.assign(ButtonAction::Dock, bind(ControllerInput::RightSecondary));
        QCOMPARE(map.effective(), map);
        // And Home itself.
        map.clear(ButtonAction::Dock);
        map.assign(ButtonAction::Home, bind(ControllerInput::RightPrimary));
        QCOMPARE(map.effective(), map);
        // Through the tracker: the menu button alone opens Home.
        ButtonMap bare = ButtonMap::defaults();
        bare.clear(ButtonAction::Home);
        bare.clear(ButtonAction::Dock);
        BindingTracker tracker;
        QVERIFY(tracker.update(bare.effective(), down({ControllerInput::LeftMenu})).isPressed(ButtonAction::Home));
    }

    void buttonPressFiresOnce()
    {
        const ButtonMap map = ButtonMap::defaults();
        BindingTracker tracker;
        QVERIFY(!tracker.update(map, 0).pressed);
        auto r = tracker.update(map, down({ControllerInput::LeftPrimary}));
        QVERIFY(r.isPressed(ButtonAction::Dock));
        QVERIFY(!r.isPressed(ButtonAction::Home));
        r = tracker.update(map, down({ControllerInput::LeftPrimary}));
        QVERIFY(!r.pressed); // held: not again
        r = tracker.update(map, 0);
        QVERIFY(!r.pressed);
        // Two actions on two buttons at once.
        r = tracker.update(map, down({ControllerInput::LeftSecondary, ControllerInput::LeftMenu, ControllerInput::LeftPrimary}));
        QVERIFY(r.isPressed(ButtonAction::Home));
        QVERIFY(r.isPressed(ButtonAction::Dock));
    }

    void clicksAreHeldWhileTheirBindingIs()
    {
        const ButtonMap map = ButtonMap::defaults();
        BindingTracker tracker;
        auto r = tracker.update(map, down({ControllerInput::RightPrimary}));
        QVERIFY(r.isHeld(ButtonAction::RightClick));
        QVERIFY(!r.isPressed(ButtonAction::RightClick));
        QVERIFY(tracker.update(map, down({ControllerInput::RightPrimary})).isHeld(ButtonAction::RightClick));
        QVERIFY(!tracker.update(map, down({ControllerInput::LeftStick})).isPressed(ButtonAction::RightClick));
        QVERIFY(tracker.update(map, down({ControllerInput::LeftStick})).isHeld(ButtonAction::RightClick));
        QVERIFY(!tracker.update(map, 0).isHeld(ButtonAction::RightClick));
        QVERIFY(!tracker.update(map, down({ControllerInput::LeftSecondary})).isHeld(ButtonAction::RightClick));
    }

    void chordFiresOnItsSecondInput()
    {
        // The defaults' recline: a grip held and B.
        const ButtonMap map = ButtonMap::defaults();
        BindingTracker tracker;
        QVERIFY(!tracker.update(map, down({ControllerInput::LeftGrip})).pressed); // a grip alone does nothing
        auto r = tracker.update(map, down({ControllerInput::LeftGrip, ControllerInput::RightSecondary}));
        QVERIFY(r.isPressed(ButtonAction::Recline));
        QVERIFY(!tracker.update(map, down({ControllerInput::LeftGrip, ControllerInput::RightSecondary})).pressed);
        QVERIFY(!tracker.update(map, down({ControllerInput::LeftGrip})).pressed); // B let go
        QVERIFY(!tracker.update(map, 0).pressed);
        // The other hand's grip.
        QVERIFY(tracker.update(map, down({ControllerInput::RightGrip})).pressed == 0);
        QVERIFY(tracker.update(map, down({ControllerInput::RightGrip, ControllerInput::RightSecondary})).isPressed(ButtonAction::Recline));
        tracker.update(map, 0);
        // B alone is not the chord, and a grip squeezed while B is held is not either: the grip is a modifier, held first.
        QVERIFY(!tracker.update(map, down({ControllerInput::RightSecondary})).pressed);
        QVERIFY(!tracker.update(map, down({ControllerInput::RightSecondary, ControllerInput::LeftGrip})).isPressed(ButtonAction::Recline));
        // Both in one frame.
        tracker.update(map, 0);
        QVERIFY(tracker.update(map, down({ControllerInput::LeftGrip, ControllerInput::RightSecondary})).isPressed(ButtonAction::Recline));
    }

    void buttonAndChordShareTheirFirstInput()
    {
        // X is the dock, X and A together the keyboard: both work.
        ButtonMap map = ButtonMap::defaults();
        map.assign(ButtonAction::Keyboard, bind(ControllerInput::LeftPrimary, ControllerInput::RightPrimary));
        map.clear(ButtonAction::RightClick);
        BindingTracker tracker;
        // X alone: the dock, on release (not on press: a chord may follow).
        QVERIFY(!tracker.update(map, down({ControllerInput::LeftPrimary})).pressed);
        QVERIFY(!tracker.update(map, down({ControllerInput::LeftPrimary})).pressed);
        QVERIFY(tracker.update(map, 0).isPressed(ButtonAction::Dock));
        QVERIFY(!tracker.update(map, 0).pressed);
        // X held, A pressed: the keyboard; neither's release does anything else.
        QVERIFY(!tracker.update(map, down({ControllerInput::LeftPrimary})).pressed);
        auto r = tracker.update(map, down({ControllerInput::LeftPrimary, ControllerInput::RightPrimary}));
        QVERIFY(r.isPressed(ButtonAction::Keyboard));
        QVERIFY(!r.isPressed(ButtonAction::Dock));
        QVERIFY(!tracker.update(map, down({ControllerInput::RightPrimary})).pressed); // X first
        QVERIFY(!tracker.update(map, 0).pressed);
        // X pressed again: a new hold, the dock on its release.
        tracker.update(map, down({ControllerInput::LeftPrimary}));
        QVERIFY(tracker.update(map, 0).isPressed(ButtonAction::Dock));
        // A alone is nothing (no action has it).
        tracker.update(map, down({ControllerInput::RightPrimary}));
        QVERIFY(!tracker.update(map, 0).pressed);
    }

    void aChordPressDoesNothingElse()
    {
        // B is the screenshot and a grip with B recline: the chord's B press and release take no screenshot.
        ButtonMap map = ButtonMap::defaults();
        map.assign(ButtonAction::Screenshot, bind(ControllerInput::RightSecondary));
        BindingTracker tracker;
        auto r = tracker.update(map, down({ControllerInput::LeftGrip}));
        r = tracker.update(map, down({ControllerInput::LeftGrip, ControllerInput::RightSecondary}));
        QVERIFY(r.isPressed(ButtonAction::Recline));
        QVERIFY(!r.isPressed(ButtonAction::Screenshot));
        QVERIFY(!tracker.update(map, down({ControllerInput::LeftGrip})).isPressed(ButtonAction::Screenshot));
        tracker.update(map, 0);
        // B alone: the screenshot at once (only a grip held first makes the chord, so nothing waits for the release).
        QVERIFY(tracker.update(map, down({ControllerInput::RightSecondary})).isPressed(ButtonAction::Screenshot));
        QVERIFY(!tracker.update(map, 0).isPressed(ButtonAction::Screenshot));
        // A click on B (a file of the old format) is held alone, and not while it is the chord's second input.
        ButtonMap click = ButtonMap::defaults();
        click.assign(ButtonAction::LeftClick, bind(ControllerInput::RightSecondary));
        BindingTracker clicks;
        QVERIFY(clicks.update(click, down({ControllerInput::RightSecondary})).isHeld(ButtonAction::LeftClick));
        clicks.update(click, 0);
        clicks.update(click, down({ControllerInput::LeftGrip}));
        r = clicks.update(click, down({ControllerInput::LeftGrip, ControllerInput::RightSecondary}));
        QVERIFY(r.isPressed(ButtonAction::Recline));
        QVERIFY(!r.isHeld(ButtonAction::LeftClick));
        QVERIFY(!clicks.update(click, down({ControllerInput::LeftGrip, ControllerInput::RightSecondary})).isHeld(ButtonAction::LeftClick));
    }

    void aChordCanBeAClick()
    {
        ButtonMap map = ButtonMap::defaults();
        map.assign(ButtonAction::MiddleClick, bind(ControllerInput::LeftGrip, ControllerInput::RightPrimary));
        BindingTracker tracker;
        tracker.update(map, down({ControllerInput::LeftGrip}));
        auto r = tracker.update(map, down({ControllerInput::LeftGrip, ControllerInput::RightPrimary}));
        QVERIFY(r.isHeld(ButtonAction::MiddleClick));
        QVERIFY(!r.isHeld(ButtonAction::RightClick)); // A is the chord's now: not its right click as well
        QVERIFY(tracker.update(map, down({ControllerInput::LeftGrip, ControllerInput::RightPrimary})).isHeld(ButtonAction::MiddleClick));
        QVERIFY(!tracker.update(map, down({ControllerInput::LeftGrip})).isHeld(ButtonAction::MiddleClick));
    }

    void recordingEndsWhenEverythingIsLetGo()
    {
        BindingRecorder recorder;
        QVERIFY(!recorder.update(0));
        // One button.
        QVERIFY(!recorder.update(down({ControllerInput::RightSecondary})));
        QCOMPARE(recorder.update(0), std::optional<Binding>(bind(ControllerInput::RightSecondary)));
        QVERIFY(!recorder.update(0)); // and nothing again
        // Two together, in either order, the first let go before the second.
        QVERIFY(!recorder.update(down({ControllerInput::RightSecondary})));
        QVERIFY(!recorder.update(down({ControllerInput::RightSecondary, ControllerInput::LeftGrip})));
        QVERIFY(!recorder.update(down({ControllerInput::LeftGrip})));
        QCOMPARE(recorder.update(0), std::optional<Binding>(bind(ControllerInput::LeftGrip, ControllerInput::RightSecondary)));
        // A grip alone is no binding: nothing, and the next press is recorded.
        QVERIFY(!recorder.update(down({ControllerInput::LeftGrip})));
        QVERIFY(!recorder.update(0));
        QVERIFY(!recorder.update(down({ControllerInput::LeftMenu})));
        QCOMPARE(recorder.update(0), std::optional<Binding>(bind(ControllerInput::LeftMenu)));
        // More than two: the first two pressed.
        recorder.update(down({ControllerInput::LeftPrimary}));
        recorder.update(down({ControllerInput::LeftPrimary, ControllerInput::RightStick}));
        recorder.update(down({ControllerInput::LeftPrimary, ControllerInput::RightStick, ControllerInput::LeftMenu}));
        QCOMPARE(recorder.update(0), std::optional<Binding>(bind(ControllerInput::LeftPrimary, ControllerInput::RightStick)));
    }

    void turnDependsOnlyOnWhereTheWindowIs()
    {
        // Moved far left it turned half, grabbed again it turned fully, brought back to the middle it
        // stayed turned until grabbed again. Now: the same place, the same turn, whatever the grabs.
        const auto yawOf = [](const SpatialWindow &w) {
            return azimuthOf(w.pose.orientation.rotate({0.0f, 0.0f, -1.0f}));
        };
        const auto aimAt = [](Vec3 from, Vec3 to) {
            const Vec3 d = to - from;
            return Pose{Quat::fromYaw(-azimuthOf(d)) * Quat::fromPitch(std::atan2(d.y, std::sqrt(d.x * d.x + d.z * d.z))), from};
        };
        for (const float curve : {1.0f, 0.5f, 0.0f}) {
            SpatialLayout layout;
            layout.setArrangementCurve(curve);
            const Pose head = headAt();
            layout.sync({{1, "chromium", 1600, 1000}}, head);
            const Vec3 hand{0.2f, -0.35f, -0.3f}; // a hand below and in front of the eyes
            const auto grab = [&] {
                const Vec3 c = layout.windows().at(1).pose.position;
                QVERIFY(layout.beginGrab(1, aimAt(hand, c), (c - hand).length()));
            };
            const auto sweep = [&](float toDegrees) {
                const Vec3 p = layout.windows().at(1).pose.position;
                const float from = azimuthOf(p);
                const float d = std::sqrt(p.x * p.x + p.z * p.z);
                for (int i = 1; i <= 60; ++i) {
                    const float a = from + (radians(toDegrees) - from) * float(i) / 60.0f;
                    layout.updateGrab(aimAt(hand, {d * std::sin(a), p.y, -d * std::cos(a)}), head, 0.0f, 0.0f, 0.011f);
                }
            };
            const auto regrabbed = [&] { // grabbed again and held still: nothing moves or turns
                const Pose before = layout.windows().at(1).pose;
                grab();
                layout.updateGrab(aimAt(hand, layout.windows().at(1).pose.position), head, 0.0f, 0.0f, 0.011f);
                layout.endGrab();
                QVERIFY(std::fabs(angleDifference(yawOf(layout.windows().at(1)), azimuthOf(before.orientation.rotate({0.0f, 0.0f, -1.0f}))))
                        < radians(0.5f));
                QVERIFY((layout.windows().at(1).pose.position - before.position).length() < 0.02f);
            };
            grab();
            sweep(-75.0f);
            layout.endGrab();
            QVERIFY(azimuthOf(layout.windows().at(1).pose.position) < radians(-60.0f));
            regrabbed();
            grab();
            sweep(0.0f);
            layout.endGrab();
            const SpatialWindow &back = layout.windows().at(1);
            QVERIFY(std::fabs(azimuthOf(back.pose.position)) < radians(6.0f)); // back near the middle
            QVERIFY2(std::fabs(yawOf(back)) <= std::fabs(azimuthOf(back.pose.position)) + radians(0.5f), "turned no more than it is off the middle");
            regrabbed();
        }
    }

    void windowsMovedAsideComeBack()
    {
        // Settings brought in front (Make Main) pushed Chromium aside; once Settings closed it stayed there.
        // Now it goes back.
        SpatialLayout layout;
        layout.sync({{1, "chromium", 1600, 1000}}, headAt());
        const Vec3 home = layout.windows().at(1).pose.position;
        layout.sync({{1, "chromium", 1600, 1000}, {2, "settings", 1200, 900}}, headAt());
        layout.makeMain(2, headAt());
        QVERIFY((layout.windows().at(1).pose.position - home).length() > 0.1f);
        layout.sync({{1, "chromium", 1600, 1000}}, headAt());
        QVERIFY((layout.windows().at(1).pose.position - home).length() < 0.001f);
        // Minimizing the window in front brings it back too.
        layout.sync({{1, "chromium", 1600, 1000}, {3, "settings", 1200, 900}}, headAt());
        layout.makeMain(3, headAt());
        QVERIFY((layout.windows().at(1).pose.position - home).length() > 0.1f);
        layout.setMinimized(3, true);
        QVERIFY((layout.windows().at(1).pose.position - home).length() < 0.001f);
        layout.sync({{1, "chromium", 1600, 1000}}, headAt());
        // Put somewhere by the user after being pushed: it stays where they put it.
        layout.sync({{1, "chromium", 1600, 1000}, {4, "settings", 1200, 900}}, headAt());
        layout.makeMain(4, headAt());
        const float aside = azimuthOf(layout.windows().at(1).pose.position);
        const Pose controller{Quat::fromYaw(-aside), {0, 0, 0}};
        QVERIFY(layout.beginGrab(1, controller, 1.0f));
        const Pose turned{Quat::fromYaw(-(aside - 0.2f)), {0, 0, 0}};
        layout.updateGrab(turned, headAt(), 0.0f, 0.0f, 0.016f);
        layout.endGrab();
        const Vec3 put = layout.windows().at(1).pose.position;
        layout.sync({{1, "chromium", 1600, 1000}}, headAt());
        QVERIFY((layout.windows().at(1).pose.position - put).length() < 0.001f);
    }

    void newWindowOpensWhereTheUserLooks()
    {
        SpatialLayout layout;
        layout.sync({{1, "konsole", 800, 500}}, headAt(90.0f));
        const SpatialWindow &w = layout.windows().at(1);
        QVERIFY(near(w.pose.position.x, 1.6f));
        QVERIFY(near(w.pose.position.z, 0.0f));
    }

    void recentreBringsTheArrangementToTheFront()
    {
        // Before the put-on recentre the head can be metres from LOCAL's origin and look anywhere:
        // after the recentre (LOCAL = the head, facing its forward) the arrangement is straight in front.
        SpatialLayout layout;
        const Pose farHead{Quat::fromYaw(-radians(120.0f)), {-0.6f, 0.2f, 3.45f}};
        layout.sync({{1, "firefox", 1332, 1024}}, farHead);
        layout.sync({{1, "firefox", 1332, 1024}, {2, "dolphin", 760, 629}}, farHead);
        const float sideBefore = layout.windows().at(2).pose.position.x;
        layout.recentered();
        const SpatialWindow &main = layout.windows().at(1);
        QVERIFY(near(main.pose.position.x, 0.0f));
        QVERIFY(near(main.pose.position.y, -0.1f));
        QVERIFY(near(main.pose.position.z, -1.6f));
        QVERIFY(near(main.pose.orientation.rotate({0, 0, 1}).z, 1.0f)); // facing the user
        // The second window is still beside the first, at the same distance.
        const Vec3 side = layout.windows().at(2).pose.position;
        QVERIFY(near(std::sqrt(side.x * side.x + side.z * side.z), 1.6f));
        QVERIFY(std::fabs(side.x) > 0.5f);
        Q_UNUSED(sideBefore);
        // A recentre after turning: LOCAL turns with the user, the arrangement keeps its LOCAL coordinates.
        layout.recentered();
        QVERIFY(near(layout.windows().at(1).pose.position.z, -1.6f));
    }

    void secondWindowGoesBesideWithoutOverlap()
    {
        SpatialLayout layout;
        layout.sync({{1, "mpv", 1920, 1080}}, headAt());
        layout.sync({{1, "mpv", 1920, 1080}, {2, "konsole", 800, 500}}, headAt());
        const SpatialWindow &a = layout.windows().at(1);
        const SpatialWindow &b = layout.windows().at(2);
        const float azA = azimuthOf(a.pose.position);
        const float azB = azimuthOf(b.pose.position);
        const float halfA = std::atan(layout.widthMeters(a) / 2.0f / 1.6f);
        const float halfB = std::atan(layout.widthMeters(b) / 2.0f / 1.6f);
        QVERIFY(std::fabs(azB - azA) >= halfA + halfB);
        QVERIFY(near(std::sqrt(b.pose.position.x * b.pose.position.x + b.pose.position.z * b.pose.position.z), 1.6f));
        // Turned to face the user.
        QVERIFY(near(facingYaw(b.pose.position, {}), 2.0f * std::atan2(b.pose.orientation.y, b.pose.orientation.w)));
    }

    void rayHitsThePixelItPointsAt()
    {
        SpatialLayout layout;
        layout.sync({{1, "mpv", 1920, 1080}}, headAt());
        // Straight at the centre (window centre is 0.1 m below the eyes).
        const auto hit = layout.hitTest({0, -0.1f, 0}, {0, 0, -1});
        QVERIFY(hit);
        QCOMPARE(hit->windowId, 1);
        QVERIFY(near(hit->x, 960.0f, 0.5f));
        QVERIFY(near(hit->y, 540.0f, 0.5f));
        // 0.48 m right and 0.27 m up from the centre: a quarter in from the top right.
        const auto corner = layout.hitTest({0.48f, 0.17f, 0}, {0, 0, -1});
        QVERIFY(corner);
        QVERIFY(near(corner->x, 1440.0f, 0.5f));
        QVERIFY(near(corner->y, 270.0f, 0.5f));
        QVERIFY(!layout.hitTest({0, 0, 0}, {0, 1, 0}));
        QVERIFY(!layout.hitTest({0, -0.1f, 0}, {0, 0, 1}));
    }

    void grabbedWindowFollowsTheRay()
    {
        SpatialLayout layout;
        layout.sync({{1, "mpv", 1920, 1080}}, headAt());
        const Pose controller{{}, {0, -0.1f, 0}};
        QVERIFY(layout.beginGrab(1, controller, 1.6f));
        // Turn the controller 20 degrees to the right.
        const Pose turned{Quat::fromYaw(-radians(20.0f)), {0, -0.1f, 0}};
        layout.updateGrab(turned, headAt(), 0.0f, 0.0f, 0.016f);
        const auto hit = layout.hitTest(turned.position, turned.forward());
        QVERIFY(hit);
        QVERIFY(near(hit->x, 960.0f, 1.0f));
        QVERIFY(near(hit->y, 540.0f, 1.0f));
        QVERIFY(near(azimuthOf(layout.windows().at(1).pose.position), radians(20.0f), 0.01f));
        layout.endGrab();
        QCOMPARE(layout.grabbedWindow(), 0);
    }

    void pushPullAndGrow()
    {
        SpatialLayout layout;
        layout.sync({{1, "mpv", 1920, 1080}}, headAt());
        const Pose controller{{}, {0, -0.1f, 0}};
        layout.beginGrab(1, controller, 1.6f);
        for (int i = 0; i < 100; ++i) {
            layout.updateGrab(controller, headAt(), 1.0f, 1.0f, 0.01f);
        }
        const SpatialWindow &w = layout.windows().at(1);
        QVERIFY(near(-w.pose.position.z, 1.6f * std::exp(1.2f), 0.02f));
        QVERIFY(near(w.scale, std::exp(0.8f), 0.01f));
    }

    void recenteringKeepsWindowsInTheRoom()
    {
        SpatialLayout layout;
        layout.sync({{1, "mpv", 1920, 1080}}, headAt());
        const Vec3 before = layout.windows().at(1).pose.position;
        // The new LOCAL origin, in the old LOCAL: 1 m to the right, turned 90 degrees.
        const Pose newOrigin{Quat::fromYaw(radians(90.0f)), {1.0f, 0.0f, 0.0f}};
        layout.spaceChanged(newOrigin);
        const Vec3 after = newOrigin.transform(layout.windows().at(1).pose.position);
        QVERIFY(near(after.x, before.x) && near(after.y, before.y) && near(after.z, before.z));
    }

    void followModeCatchesUp()
    {
        SpatialLayout layout;
        layout.sync({{1, "konsole", 800, 500}}, headAt());
        QVERIFY(layout.toggleFollow(1, headAt()));
        // A small turn: the window stays.
        layout.updateFollow(headAt(10.0f), 0.1f);
        QVERIFY(near(azimuthOf(layout.windows().at(1).pose.position), 0.0f, 0.01f));
        // A big turn: it comes along, slowly.
        for (int i = 0; i < 300; ++i) {
            layout.updateFollow(headAt(60.0f), 0.01f);
        }
        QVERIFY(near(azimuthOf(layout.windows().at(1).pose.position), radians(60.0f), radians(2.0f)));
    }

    void followingWindowsMoveAsOneGroup()
    {
        // Firefox in front of the user, mpv 20 degrees to its right; both follow; the user turns 60 degrees.
        SpatialLayout layout;
        layout.sync({{1, "firefox", 1000, 700}}, headAt());
        layout.sync({{1, "firefox", 1000, 700}, {2, "mpv", 600, 400}}, headAt());
        const float before = angleDifference(azimuthOf(layout.windows().at(2).pose.position), azimuthOf(layout.windows().at(1).pose.position));
        layout.setFollowAll(true, headAt());
        QVERIFY(layout.followAll());
        for (int i = 0; i < 20; ++i) {
            layout.updateFollow(headAt(60.0f), 0.01f);
            // Mid-way too, the two keep their places relative to each other (one group, one moment).
            const float now = angleDifference(azimuthOf(layout.windows().at(2).pose.position), azimuthOf(layout.windows().at(1).pose.position));
            QVERIFY(near(now, before, 1e-3f));
        }
        for (int i = 0; i < 300; ++i) {
            layout.updateFollow(headAt(60.0f), 0.01f);
        }
        QVERIFY(near(azimuthOf(layout.windows().at(1).pose.position), radians(60.0f), radians(2.0f)));
        layout.setFollowAll(false, headAt(60.0f));
        QVERIFY(!layout.followAll() && !layout.windows().at(2).follow);
    }

    void cinemaModeIsALargeScreenInFrontAndComesBack()
    {
        SpatialLayout layout;
        layout.sync({{1, "mpv", 1280, 720}}, headAt());
        layout.sync({{1, "mpv", 1280, 720}, {2, "firefox", 1000, 700}}, headAt());
        const Vec3 before = layout.windows().at(2).pose.position;
        layout.enterCinema(2, headAt(20.0f));
        QCOMPARE(layout.cinemaWindow(), 2);
        // The app goes fullscreen: still the theater screen, 3.6 m wide, 3.5 m ahead where the user looked.
        layout.sync({{1, "mpv", 1280, 720}, {2, "firefox", 1920, 1080, true}}, headAt(20.0f));
        const SpatialWindow &w = layout.windows().at(2);
        QVERIFY(near(layout.widthMeters(w), 3.6f));
        QVERIFY(near(azimuthOf(w.pose.position), radians(20.0f), 1e-3f));
        QVERIFY(near(std::sqrt(w.pose.position.x * w.pose.position.x + w.pose.position.z * w.pose.position.z), 3.5f));
        // Only it takes the ray.
        QVERIFY(!layout.hitTest({0, -0.1f, 0}, {0, 0, -1}, 2) || layout.hitTest({0, -0.1f, 0}, {0, 0, -1}, 2)->windowId == 2);
        layout.leaveCinema();
        layout.sync({{1, "mpv", 1280, 720}, {2, "firefox", 1000, 700, false}}, headAt(20.0f));
        const Vec3 after = layout.windows().at(2).pose.position;
        QVERIFY(near(after.x, before.x) && near(after.y, before.y) && near(after.z, before.z));
        QCOMPARE(layout.cinemaWindow(), 0);

        // Moved and made larger in cinema mode: that is where it comes back next time.
        layout.enterCinema(2, headAt());
        layout.sync({{1, "mpv", 1280, 720}, {2, "firefox", 1920, 1080, true}}, headAt());
        layout.beginScale(2, {0, 0, 0}, (layout.surfacePose(layout.windows().at(2)).transform({1.8f, 0.0f, 0.0f})).normalized());
        layout.updateScale({0, 0, 0}, (layout.surfacePose(layout.windows().at(2)).transform({2.25f, 0.0f, 0.0f})).normalized());
        layout.endDrag();
        const float bigger = layout.widthMeters(layout.windows().at(2));
        QVERIFY(bigger > 4.0f);
        layout.leaveCinema();
        layout.sync({{1, "mpv", 1280, 720}, {2, "firefox", 1000, 700, false}}, headAt(40.0f));
        layout.enterCinema(2, headAt(40.0f)); // looking elsewhere now: still where it was left (20 degrees, its first place)
        layout.sync({{1, "mpv", 1280, 720}, {2, "firefox", 1920, 1080, true}}, headAt(40.0f));
        QVERIFY(near(layout.widthMeters(layout.windows().at(2)), bigger, 0.01f));
        QVERIFY(near(azimuthOf(layout.windows().at(2).pose.position), radians(20.0f), 0.01f));
    }

    //! Maximized: 3840 pixels (twice 1920) as large in the room as 1920 were, twice the detail; saved and remembered at
    //! its normal scale; its own scale again after maximized, fullscreen and normal; resized from maximized, as asked.
    void maximizedIsTwiceTheDetailAsLargeInTheRoom()
    {
        SpatialLayout layout;
        layout.sync({{1, "dolphin", 1000, 700, false, 2.0f}}, headAt());
        QVERIFY(near(layout.windows().at(1).scale, 1.0f));
        layout.sync({{1, "dolphin", 3840, 2160, false, 2.0f, false, true}}, headAt());
        QVERIFY(layout.windows().at(1).maximized);
        QVERIFY(near(layout.windows().at(1).scale, 0.5f));
        QVERIFY(near(layout.widthMeters(layout.windows().at(1)), 1.92f));
        // Saved at its normal scale: opened again normal, as large as before; opened again maximized, half of it.
        SpatialLayout restarted;
        restarted.load(layout.save());
        restarted.sync({{4, "dolphin", 1000, 700, false, 2.0f}}, headAt());
        QVERIFY(near(restarted.windows().at(4).scale, 1.0f));
        SpatialLayout again;
        again.load(layout.save());
        again.sync({{6, "dolphin", 3840, 2160, false, 2.0f, false, true}}, headAt());
        QVERIFY(near(again.windows().at(6).scale, 0.5f));
        // Back to normal: its own scale.
        layout.sync({{1, "dolphin", 1000, 700, false, 2.0f}}, headAt());
        QVERIFY(!layout.windows().at(1).maximized);
        QVERIFY(near(layout.windows().at(1).scale, 1.0f));
        // The size button's round: maximized, fullscreen, normal: its own scale at the end.
        layout.sync({{1, "dolphin", 3840, 2160, false, 2.0f, false, true}}, headAt());
        layout.sync({{1, "dolphin", 3840, 2160, true, 2.0f}}, headAt());
        layout.sync({{1, "dolphin", 1000, 700, false, 2.0f}}, headAt());
        QVERIFY(near(layout.windows().at(1).scale, 1.0f));
        // Maximized, then made smaller with the resize button: the pixels asked at its scale (no jump).
        layout.sync({{1, "dolphin", 3840, 2160, false, 2.0f, false, true}}, headAt());
        layout.askedSize(1, 3000, 1800);
        layout.sync({{1, "dolphin", 3000, 1800, false, 2.0f}}, headAt());
        QVERIFY(near(layout.windows().at(1).scale, 0.5f));
        QVERIFY(near(layout.widthMeters(layout.windows().at(1)), 1.5f));
    }

    //! The cinema screen resized with the resize button: any shape, larger with more pixels (its scale stays), the app's
    //! own changes still keep its width; the next time it comes back as wide and where it was, at the app's own size
    //! (its shape is not remembered: every film has its own).
    void cinemaScreenResizedKeepsItsScaleAndComesBack()
    {
        SpatialLayout layout;
        layout.sync({{1, "mpv", 3840, 2160, false, 2.0f}}, headAt());
        layout.askedSize(1, 1920, 1080); // the window made smaller before cinema: not the screen's size
        layout.sync({{1, "mpv", 1920, 1080, false, 2.0f}}, headAt());
        layout.enterCinema(1, headAt());
        layout.sync({{1, "mpv", 1920, 1080, true, 2.0f}}, headAt());
        QVERIFY(near(layout.widthMeters(layout.windows().at(1)), 3.6f));
        // Wider, for a 2.39:1 film: the pixels asked come, the scale stays, the screen is wider and as high.
        const float scale = layout.windows().at(1).scale;
        layout.askedSize(1, 2580, 1080);
        layout.sync({{1, "mpv", 2582, 1080, true, 2.0f}}, headAt()); // the app's units at 200 %: two pixels off
        QVERIFY(near(layout.windows().at(1).scale, scale));
        QVERIFY(near(layout.widthMeters(layout.windows().at(1)), 3.6f * 2582.0f / 1920.0f, 0.01f));
        const float wide = layout.widthMeters(layout.windows().at(1));
        // A change the user did not ask for (the app's own): the width stays.
        layout.sync({{1, "mpv", 1920, 1080, true, 2.0f}}, headAt());
        QVERIFY(near(layout.widthMeters(layout.windows().at(1)), wide, 0.01f));
        // Scaled larger with the handle, as before.
        layout.beginScale(1, {0, 0, 0}, (layout.surfacePose(layout.windows().at(1)).transform({1.5f, 0.0f, 0.0f})).normalized());
        layout.updateScale({0, 0, 0}, (layout.surfacePose(layout.windows().at(1)).transform({1.8f, 0.0f, 0.0f})).normalized());
        layout.endDrag();
        const float wider = layout.widthMeters(layout.windows().at(1));
        QVERIFY(wider > wide + 0.3f);
        layout.leaveCinema();
        layout.sync({{1, "mpv", 3840, 2160, false, 2.0f}}, headAt());
        // Next time: as wide as it was left, at the app's own fullscreen size (not the 2.39 shape asked before).
        layout.enterCinema(1, headAt());
        layout.sync({{1, "mpv", 1920, 1080, true, 2.0f}}, headAt());
        QVERIFY(near(layout.widthMeters(layout.windows().at(1)), wider, 0.01f));
        QCOMPARE(layout.windows().at(1).widthPx, 1920);
        layout.leaveCinema();
    }

    void fullscreenIsACinemaScreenAndComesBack()
    {
        SpatialLayout layout;
        layout.sync({{1, "firefox", 1332, 1024}}, headAt());
        const Vec3 windowed = layout.windows().at(1).pose.position;
        layout.sync({{1, "firefox", 1920, 1080, true}}, headAt());
        const SpatialWindow &cinema = layout.windows().at(1);
        QVERIFY(near(cinema.pose.position.z, -2.4f));
        QVERIFY(near(layout.widthMeters(cinema), 2.6f));
        // The user scales the fullscreen screen; it keeps that size, also when the app changes its pixels.
        const Vec3 eye{0, 0, 0};
        const Vec3 corner = layout.surfacePose(cinema).transform({1.0f, -0.5f, 0.0f});
        QVERIFY(layout.beginScale(1, eye, corner.normalized()));
        layout.updateScale(eye, layout.surfacePose(cinema).transform({0.8f, -0.4f, 0.0f}).normalized());
        layout.endDrag();
        QVERIFY(near(layout.widthMeters(layout.windows().at(1)), 2.08f, 2e-3f));
        layout.sync({{1, "firefox", 1920, 1080, true}}, headAt());
        QVERIFY(near(layout.widthMeters(layout.windows().at(1)), 2.08f, 2e-3f));
        layout.sync({{1, "firefox", 1280, 720, true}}, headAt());
        QVERIFY(near(layout.widthMeters(layout.windows().at(1)), 2.08f, 2e-3f));
        // Cinema mode and back keeps the screen the user made (it came back at the default size); the
        // cinema screen follows when asked.
        layout.enterCinema(1, headAt());
        layout.sync({{1, "firefox", 1920, 1080, true}}, headAt());
        QVERIFY(layout.toggleFollow(1, headAt()));
        for (int i = 0; i < 300; ++i) {
            layout.updateFollow(headAt(90.0f), 1.0f / 90.0f);
        }
        QVERIFY(layout.windows().at(1).pose.position.x > 3.0f); // went round with the user
        layout.toggleFollow(1, headAt(90.0f));
        layout.leaveCinema();
        layout.sync({{1, "firefox", 1920, 1080, true}}, headAt());
        QVERIFY(near(layout.widthMeters(layout.windows().at(1)), 2.08f, 2e-3f));
        QVERIFY(near(layout.windows().at(1).pose.position.z, -2.4f));
        // A fullscreen screen tilts, and the window gets its own tilt back after.
        layout.adjustTilt(1, radians(20.0f), 0.0f);
        QVERIFY(layout.surfacePose(layout.windows().at(1)).orientation.rotate({0, 0, 1}).y > 0.3f);
        layout.sync({{1, "firefox", 1332, 1024, false}}, headAt());
        const SpatialWindow &back = layout.windows().at(1);
        QVERIFY(near(back.pose.position.z, windowed.z) && near(back.scale, 1.0f) && near(back.tilt, 0.0f));
    }

    void fullscreenGrowsAndShrinksWhereItIs()
    {
        // Fullscreen and back moved the window elsewhere; Make Main did nothing for a fullscreen one.
        SpatialLayout layout;
        layout.sync({{1, "dolphin", 1280, 820}, {2, "firefox", 1280, 820}}, headAt());
        const Vec3 windowed = layout.windows().at(2).pose.position;
        QVERIFY(windowed.x > 0.5f); // beside the first, to the right
        layout.sync({{1, "dolphin", 1280, 820}, {2, "firefox", 1920, 1080, true}}, headAt());
        const Vec3 screen = layout.windows().at(2).pose.position;
        QVERIFY(near(azimuthOf(screen), azimuthOf(windowed), 1e-3f)); // where it was, further away
        QVERIFY(near(std::sqrt(screen.x * screen.x + screen.z * screen.z), 2.4f, 1e-3f));
        layout.sync({{1, "dolphin", 1280, 820}, {2, "firefox", 1280, 820, false}}, headAt());
        const Vec3 back = layout.windows().at(2).pose.position;
        QVERIFY(near(back.x, windowed.x, 1e-3f) && near(back.y, windowed.y, 1e-3f) && near(back.z, windowed.z, 1e-3f));
        // Fullscreen again, then Make Main: in front of the user, at the screen's distance and size.
        layout.sync({{1, "dolphin", 1280, 820}, {2, "firefox", 1920, 1080, true}}, headAt());
        const float width = layout.widthMeters(layout.windows().at(2));
        layout.makeMain(2, headAt());
        const SpatialWindow &main = layout.windows().at(2);
        QVERIFY(near(main.pose.position.x, 0.0f, 1e-3f) && near(main.pose.position.z, -2.4f, 1e-3f));
        QVERIFY(near(layout.widthMeters(main), width) && main.fullscreen);
        // A window nearer than the screen, in front of it, moves aside (it stayed in front).
        layout.makeMain(1, headAt()); // dolphin in front at 1.6 m...
        QVERIFY(std::fabs(layout.windows().at(1).pose.position.x) < 1e-3f);
        layout.makeMain(2, headAt()); // ...then the fullscreen screen behind it is made main
        const Vec3 aside = layout.windows().at(1).pose.position;
        QVERIFY(std::fabs(azimuthOf(aside)) > std::atan(layout.widthMeters(layout.windows().at(2)) / 2.0f / 2.4f));
    }

    void laterWindowsOpenInFront()
    {
        // A game launcher's dialogs opened far left and right; once working, new windows open where
        // the user looks, in front of a window already there.
        SpatialLayout layout;
        layout.sync({{1, "dolphin", 1280, 820}}, headAt());
        layout.setOpenInFront(true);
        layout.sync({{1, "dolphin", 1280, 820}, {2, "zenity", 480, 300}}, headAt(40.0f));
        const Vec3 dialog = layout.windows().at(2).pose.position;
        QVERIFY(near(azimuthOf(dialog), radians(40.0f), 1e-3f)); // where the user looks now, clear of dolphin
        QVERIFY(near(std::sqrt(dialog.x * dialog.x + dialog.z * dialog.z), 1.6f, 1e-3f)); // nothing there: the usual distance
        layout.sync({{1, "dolphin", 1280, 820}, {2, "zenity", 480, 300}, {3, "zenity", 400, 200}}, headAt());
        const Vec3 second = layout.windows().at(3).pose.position;
        QVERIFY(near(second.x, 0.0f, 1e-3f) && std::sqrt(second.x * second.x + second.z * second.z) < 1.6f - 0.2f); // in front of dolphin
    }

    void clickStaysWhereItWasPressed()
    {
        const auto dir = [](float yawDegrees) {
            return Quat::fromYaw(-radians(yawDegrees)).rotate({0, 0, -1});
        };
        ClickLock lock;
        lock.press(100.0, 50.0, dir(0.0f));
        // A quick click: the trigger pull turns the controller 4 degrees within 0.15 s: still the press point.
        QCOMPARE(lock.filter(160.0, 58.0, dir(4.0f), 0.15f), std::make_pair(100.0, 50.0));
        lock.release();
        // A fast drag (8 degrees at once): follows the ray.
        lock.press(100.0, 50.0, dir(0.0f));
        QCOMPARE(lock.filter(220.0, 50.0, dir(8.0f), 0.05f), std::make_pair(220.0, 50.0));
        QCOMPARE(lock.filter(110.0, 50.0, dir(1.0f), 0.05f), std::make_pair(110.0, 50.0));
        lock.release();
        // A slow drag: held past the quick moment, 2 degrees is a drag.
        lock.press(100.0, 50.0, dir(0.0f));
        QCOMPARE(lock.filter(100.0, 50.0, dir(0.5f), 0.4f), std::make_pair(100.0, 50.0));
        QCOMPARE(lock.filter(140.0, 50.0, dir(2.0f), 0.05f), std::make_pair(140.0, 50.0));
    }

    void rayFilterCalmsTremorButFollowsMoves()
    {
        RayFilter filter;
        const Vec3 origin{0, 0, 0};
        Pose out;
        // Tremor: +-0.4 degrees at 90 Hz around straight ahead.
        float maxOff = 0.0f;
        for (int i = 0; i < 180; ++i) {
            const float yaw = radians(((i % 2) ? 0.4f : -0.4f));
            out = filter.filter({Quat::fromYaw(yaw), origin}, 1.0f / 90.0f);
            if (i > 20) {
                maxOff = std::max(maxOff, std::fabs(azimuthOf(out.forward())));
            }
        }
        QVERIFY(maxOff < radians(0.15f));
        // A real turn to 30 degrees: there within a quarter of a second.
        for (int i = 0; i < 23; ++i) {
            out = filter.filter({Quat::fromYaw(-radians(30.0f)), origin}, 1.0f / 90.0f);
        }
        QVERIFY(azimuthOf(out.forward()) > radians(27.0f));
    }

    void tiltedWindowStillHitsThePixel()
    {
        SpatialLayout layout;
        layout.sync({{1, "mpv", 1920, 1080}}, headAt());
        // The hand moves 20 cm up without turning (the ray's point drags up the tilt bar): past 1.5 cm of dead zone,
        // the face turns up 0.185 m x 1.6 rad/m.
        QVERIFY(layout.beginTilt(1, {0, -0.1f, 0}, {0, 0, -1}));
        layout.updateTilt({0, 0.1f, 0}, {0, 0, -1});
        layout.endDrag();
        const SpatialWindow &w = layout.windows().at(1);
        QVERIFY(near(w.tilt, 0.185f * 1.6f, 1e-3f));
        QVERIFY(near(w.turn, 0.0f));
        const Vec3 normal = layout.surfacePose(w).orientation.rotate({0, 0, 1});
        QVERIFY(near(normal.y, std::sin(0.185f * 1.6f)) && normal.z > 0.9f);
        // Straight at the centre still hits the centre; a point 0.2 m up the tilted plane hits 200 px above it.
        const auto centre = layout.hitTest({0, -0.1f, 0}, {0, 0, -1});
        QVERIFY(centre && near(centre->x, 960.0f, 0.5f) && near(centre->y, 540.0f, 0.5f));
        const Vec3 onPlane = layout.surfacePose(w).transform({0.0f, 0.2f, 0.0f});
        const auto upper = layout.hitTest({0, 0, 0}, onPlane.normalized());
        QVERIFY(upper && near(upper->x, 960.0f, 0.5f) && near(upper->y, 340.0f, 0.5f));
    }

    void tiltBarTurnsTheWindowLeftAndRight()
    {
        SpatialLayout layout;
        layout.sync({{1, "dolphin", 760, 629}}, headAt());
        // The ray's point drags 15 cm to the right: the face turns 0.135 m x 1.6 rad/m to the user's right.
        QVERIFY(layout.beginTilt(1, {0, -0.1f, 0}, {0, 0, -1}));
        layout.updateTilt({0.15f, -0.1f, 0}, {0, 0, -1});
        layout.endDrag();
        const SpatialWindow &w = layout.windows().at(1);
        QVERIFY(near(w.turn, 0.135f * 1.6f, 1e-3f));
        QVERIFY(near(w.tilt, 0.0f));
        const Vec3 normal = layout.surfacePose(w).orientation.rotate({0, 0, 1});
        QVERIFY(near(normal.x, std::sin(0.135f * 1.6f)));
        // The reset dot: upright again.
        layout.resetTilt(1);
        QVERIFY(near(layout.windows().at(1).turn, 0.0f) && near(layout.windows().at(1).tilt, 0.0f));
        layout.beginTilt(1, {0, -0.1f, 0}, {0, 0, -1});
        layout.updateTilt({0.15f, -0.1f, 0}, {0, 0, -1});
        layout.endDrag();
        // Still hit where it points; fullscreen starts facing the user, and the window's turn comes back after.
        const auto centre = layout.hitTest({0, -0.1f, 0}, {0, 0, -1});
        QVERIFY(centre && near(centre->x, 380.0f, 0.5f));
        layout.sync({{1, "dolphin", 1920, 1080, true}}, headAt());
        QVERIFY(near(layout.surfacePose(layout.windows().at(1)).orientation.rotate({0, 0, 1}).x, 0.0f));
        layout.sync({{1, "dolphin", 760, 629, false}}, headAt());
        QVERIFY(near(layout.windows().at(1).turn, 0.135f * 1.6f, 1e-3f));
    }

    void thumbstickTiltsAndTurns()
    {
        SpatialLayout layout;
        layout.sync({{1, "mpv", 1280, 720}}, headAt());
        layout.adjustTilt(1, radians(10.0f), radians(-5.0f));
        QVERIFY(near(layout.windows().at(1).tilt, radians(10.0f)) && near(layout.windows().at(1).turn, radians(-5.0f)));
        for (int i = 0; i < 100; ++i) {
            layout.adjustTilt(1, radians(5.0f), 0.0f);
        }
        QVERIFY(near(layout.windows().at(1).tilt, 1.05f)); // 60 degrees at most
        layout.resetTilt(1);
        QVERIFY(near(layout.windows().at(1).tilt, 0.0f));
    }

    void curvedWindowBendsAroundItsAxis()
    {
        // 1.92 m wide at 1.6 m (centre 0.1 m below the eyes), bent through 1.2 rad: radius 1.6 m, its axis through the
        // eye, so every column is 1.6 m away (curved windows).
        SpatialLayout layout;
        layout.sync({{1, "mpv", 1920, 1080}}, headAt());
        layout.setCurvature(1, 1.2f / kMaxCurveAngle);
        const SpatialWindow &w = layout.windows().at(1);
        QVERIFY(near(layout.curveRadius(w), 1.6f));
        // Its right edge: 0.6 rad round, facing the eye.
        const Pose edge = layout.pointOn(w, 0.96f, 0.0f);
        QVERIFY(near(edge.position.x, 1.6f * std::sin(0.6f)) && near(edge.position.y, -0.1f) && near(edge.position.z, -1.6f * std::cos(0.6f)));
        const Vec3 normal = edge.orientation.rotate({0, 0, 1});
        QVERIFY(near(normal.x, -std::sin(0.6f)) && near(normal.z, std::cos(0.6f)));
        // A ray 0.3 rad right of straight ahead meets it 0.48 m of arc right of the centre: pixel 1440 (flat: 1455).
        const Vec3 eye{0, -0.1f, 0};
        const auto hit = layout.hitTest(eye, {std::sin(0.3f), 0, -std::cos(0.3f)});
        QVERIFY(hit && near(hit->x, 1440.0f, 0.5f) && near(hit->y, 540.0f, 0.5f) && near(hit->distance, 1.6f));
        QVERIFY(near(hit->localX, 0.48f));
        // Past its edge: not on the window, but on its curve (the frame's controls are there).
        QVERIFY(!layout.hitTest(eye, {std::sin(0.7f), 0, -std::cos(0.7f)}));
        const auto beyond = layout.projectOnto(1, eye, {std::sin(0.7f), 0, -std::cos(0.7f)});
        QVERIFY(beyond && near(beyond->localX, 1.12f));
        // From behind: nothing.
        QVERIFY(!layout.hitTest({0, -0.1f, -3.0f}, {0, 0, 1}));
        // The point a frame control sits at is where the ray meets it.
        const Pose control = layout.pointOn(w, -1.1f, 0.2f);
        const auto onControl = layout.projectOnto(1, eye, (control.position - eye).normalized());
        QVERIFY(onControl && near(onControl->localX, -1.1f) && near(onControl->localY, 0.2f));
        // Snapping: a little curvature is flat; the thumbstick leaves flat in one step and comes back to it.
        layout.setCurvature(1, 0.03f);
        QVERIFY(near(layout.windows().at(1).curvature, 0.0f) && near(layout.curveRadius(layout.windows().at(1)), 0.0f));
        layout.adjustCurvature(1, 0.01f);
        QVERIFY(near(layout.windows().at(1).curvature, kCurveSnap));
        layout.adjustCurvature(1, -0.01f);
        QVERIFY(near(layout.windows().at(1).curvature, 0.0f));
        // Remembered with the app's placement.
        layout.setCurvature(1, 0.5f);
        SpatialLayout restarted;
        restarted.load(layout.save());
        restarted.sync({{4, "mpv", 1920, 1080}}, headAt());
        QVERIFY(near(restarted.windows().at(4).curvature, 0.5f));
    }

    void minimizedWindowIsOutOfReach()
    {
        SpatialLayout layout;
        layout.sync({{1, "mpv", 1920, 1080}}, headAt());
        layout.setMinimized(1, true);
        QVERIFY(layout.windows().at(1).minimized);
        QVERIFY(!layout.hitTest({0, -0.1f, 0}, {0, 0, -1}));
        layout.setMinimized(1, false);
        QVERIFY(layout.hitTest({0, -0.1f, 0}, {0, 0, -1}));
    }

    void scaleHandleKeepsTheCornerUnderTheRay()
    {
        SpatialLayout layout;
        layout.sync({{1, "konsole", 800, 500}}, headAt());
        // The bottom-right corner (0.4, -0.25 from the centre at y -0.1), from the eye.
        const Vec3 eye{0, 0, 0};
        const Vec3 corner{0.4f, -0.35f, -1.6f};
        QVERIFY(layout.beginScale(1, eye, corner.normalized()));
        const Vec3 dragged{0.6f, -0.475f, -1.6f}; // 1.5 times as far from the centre
        layout.updateScale(eye, dragged.normalized());
        layout.endDrag();
        QVERIFY(near(layout.windows().at(1).scale, 1.5f, 1e-3f));
        QCOMPARE(layout.draggedWindow(), 0);
    }

    void makeMainBringsItInFrontAndMovesOthersAside()
    {
        SpatialLayout layout;
        layout.sync({{1, "konsole", 800, 500}}, headAt());
        layout.sync({{1, "konsole", 800, 500}, {2, "dolphin", 760, 629}}, headAt());
        // Turned 30 degrees right, Dolphin (beside Konsole) becomes the main window.
        layout.makeMain(2, headAt(30.0f));
        const SpatialWindow &main = layout.windows().at(2);
        QVERIFY(near(azimuthOf(main.pose.position), radians(30.0f), 1e-3f));
        QVERIFY(near(std::sqrt(main.pose.position.x * main.pose.position.x + main.pose.position.z * main.pose.position.z), 1.6f));
        QVERIFY(near(layout.widthMeters(main), 1.9f, 0.02f)); // 760 px: scale 2.5 caps it
        QVERIFY(near(main.tilt, 0.0f));
        // Konsole was in the way (at 0 degrees, the main window spans about 6..54): it moved out of it.
        const float konsole = azimuthOf(layout.windows().at(1).pose.position);
        const float halfMain = std::atan(layout.widthMeters(main) / 2.0f / 1.6f);
        const float halfKonsole = std::atan(0.4f / 1.6f);
        QVERIFY(std::fabs(angleDifference(konsole, radians(30.0f))) >= halfMain + halfKonsole);
    }

    void pinnedWindowStaysInTheRoomOnRecentre()
    {
        SpatialLayout layout;
        layout.sync({{1, "konsole", 800, 500}}, headAt());
        layout.sync({{1, "konsole", 800, 500}, {2, "dolphin", 760, 629}}, headAt());
        QVERIFY(layout.togglePin(2));
        const Vec3 pinnedBefore = layout.windows().at(2).pose.position;
        // The user walked 1 m right and turned 90 degrees right, then recentred: new LOCAL = their head then.
        const Pose headOld{Quat::fromYaw(-radians(90.0f)), {1.0f, 0.0f, 0.0f}};
        const Pose headNew{};
        const Pose oldToNew = headNew * headOld.inverse();
        layout.recentered(oldToNew);
        // The pinned window: the same place in the room (in new coordinates).
        const Vec3 expected = oldToNew.transform(pinnedBefore);
        const Vec3 pinnedAfter = layout.windows().at(2).pose.position;
        QVERIFY(near(pinnedAfter.x, expected.x) && near(pinnedAfter.y, expected.y) && near(pinnedAfter.z, expected.z));
        // The unpinned one came to the new front.
        QVERIFY(near(layout.windows().at(1).pose.position.z, -1.6f));
        // Pinning is remembered.
        SpatialLayout restarted;
        restarted.load(layout.save());
        restarted.sync({{5, "dolphin", 760, 629}}, headAt());
        QVERIFY(restarted.windows().at(5).pinned);
    }

    void recentreMovementFromTheHeadJump()
    {
        // The head drifts slowly; at frame 20 LOCAL jumps (the user was 1 m right, turned 90 degrees right).
        const Pose headOld{Quat::fromYaw(-radians(90.0f)), {1.0f, 1.6f, 0.0f}};
        const Pose headNew{Quat::fromYaw(0.0f), {0.0f, 1.6f, 0.0f}};
        std::vector<Pose> heads;
        for (int i = 0; i < 40; ++i) {
            const Vec3 drift{0.0005f * i, 0.0f, 0.0f};
            const Pose h = i < 20 ? headOld : headNew;
            heads.push_back({h.orientation, h.position + drift});
        }
        const auto oldToNew = recentreFromHeads(heads);
        QVERIFY(oldToNew);
        // The head just before the jump, in new coordinates, is the head just after it.
        const Vec3 p = oldToNew->transform(heads[19].position);
        QVERIFY(near(p.x, heads[20].position.x) && near(p.y, heads[20].position.y) && near(p.z, heads[20].position.z));
        QVERIFY(near(azimuthOf(oldToNew->orientation.rotate(headOld.forward())), 0.0f, 0.01f));
        // No jump: nothing measurable.
        heads.resize(19);
        QVERIFY(!recentreFromHeads(heads));
    }

    void lockedWindowStaysPutButStillScales()
    {
        SpatialLayout layout;
        layout.sync({{1, "mpv", 1920, 1080}}, headAt());
        QVERIFY(layout.toggleLock(1));
        QVERIFY(layout.windows().at(1).locked);
        const Vec3 before = layout.windows().at(1).pose.position;
        const Vec3 facingBefore = layout.windows().at(1).pose.forward();
        // A grab by chance: the hand turns 30 degrees, pushes away and grows it.
        const Pose controller{{}, {0, -0.1f, 0}};
        QVERIFY(layout.beginGrab(1, controller, 1.6f));
        const Pose turned{Quat::fromYaw(-radians(30.0f)), {0.2f, 0.1f, 0}};
        for (int i = 0; i < 100; ++i) {
            layout.updateGrab(turned, headAt(), 1.0f, 1.0f, 0.01f);
        }
        layout.endGrab();
        const SpatialWindow &held = layout.windows().at(1);
        QVERIFY(near(held.pose.position.x, before.x) && near(held.pose.position.y, before.y) && near(held.pose.position.z, before.z));
        QVERIFY(near(held.pose.forward().x, facingBefore.x) && near(held.pose.forward().z, facingBefore.z));
        QVERIFY(near(held.scale, std::exp(0.8f), 0.01f)); // its scale still changes
        // Unlocked: the same grab moves it.
        QVERIFY(!layout.toggleLock(1));
        const Pose turnedInPlace{Quat::fromYaw(-radians(30.0f)), {0, -0.1f, 0}};
        layout.beginGrab(1, controller, 1.6f);
        layout.updateGrab(turnedInPlace, headAt(), 0.0f, 0.0f, 0.01f);
        layout.endGrab();
        QVERIFY(near(azimuthOf(layout.windows().at(1).pose.position), radians(30.0f), 0.02f));
        // Locked is remembered for the app, and stays through cinema mode.
        QVERIFY(layout.toggleLock(1));
        SpatialLayout restarted;
        restarted.load(layout.save());
        restarted.sync({{4, "mpv", 1920, 1080}}, headAt());
        QVERIFY(restarted.windows().at(4).locked);
        restarted.enterCinema(4, headAt());
        const Vec3 screen = restarted.windows().at(4).pose.position;
        restarted.beginGrab(4, controller, 3.5f);
        restarted.updateGrab(turned, headAt(), 1.0f, 0.0f, 0.01f);
        restarted.endGrab();
        QVERIFY(near(restarted.windows().at(4).pose.position.x, screen.x) && near(restarted.windows().at(4).pose.position.z, screen.z));
        restarted.leaveCinema();
        QVERIFY(restarted.windows().at(4).locked);
        // A window from before the padlock (no "locked" saved): not locked.
        SpatialLayout old;
        QJsonObject saved = layout.save();
        QJsonObject windows = saved.value(QStringLiteral("windows")).toObject();
        QJsonObject mpv = windows.value(QStringLiteral("mpv")).toObject();
        mpv.remove(QStringLiteral("locked"));
        windows.insert(QStringLiteral("mpv"), mpv);
        saved.insert(QStringLiteral("windows"), windows);
        old.load(saved);
        old.sync({{9, "mpv", 1920, 1080}}, headAt());
        QVERIFY(!old.windows().at(9).locked);
    }

    //! Cinema mode's row of shapes: under the grab bar, only on the cinema screen; each shape's pixels as large as fit a
    //! UHD picture; the screen in the 16:9 frame around it; Original from the size the app chose; one shape lit.
    void cinemaShapes()
    {
        FrameLayout f = FrameLayout::compute(3.6f, 2.025f, 0.05f, 3.5f);
        const FrameRect first = f.aspects[0];
        QVERIFY(f.aspectBar.y < f.grab.y - f.grab.height / 2.0f && near(f.aspectBar.x, 0.0f));
        QVERIFY(f.hit(first.x, first.y) != FrameControl::AspectOriginal); // not on an ordinary window
        f.showAspects();
        QVERIFY(f.aspectsShown);
        QVERIFY(f.near(first.x, first.y - first.height / 2.0f)); // the frame shows near the whole row
        QVERIFY(f.near(f.aspects[kAspects - 1].x, f.aspects[kAspects - 1].y - f.aspects[kAspects - 1].height / 2.0f));
        // Tallest to widest after Original.
        for (int i = 2; i < kAspects; ++i) {
            QVERIFY(kAspectValues[i] > kAspectValues[i - 1]);
        }
        for (int i = 0; i < kAspects; ++i) {
            QCOMPARE(f.hit(f.aspects[i].x, f.aspects[i].y), kAspectOrder[i]);
            QCOMPARE(aspectIndex(kAspectOrder[i]), i);
        }
        QCOMPARE(f.hit(0.0f, f.grab.y), FrameControl::Grab); // the grab bar still grabs
        QCOMPARE(aspectIndex(FrameControl::Grab), -1);
        // Pixels: a 4K film's own, whatever its shape.
        QCOMPARE(screenPixels(16.0f / 9.0f), std::make_pair(3840, 2160));
        QCOMPARE(screenPixels(2.39f), std::make_pair(3840, 1606));
        QCOMPARE(screenPixels(1.85f), std::make_pair(3840, 2076));
        QCOMPARE(screenPixels(2.0f), std::make_pair(3840, 1920));
        QCOMPARE(screenPixels(4.0f / 3.0f), std::make_pair(2880, 2160));
        QCOMPARE(screenPixels(1.90f), std::make_pair(3840, 2022));
        QCOMPARE(screenPixels(1.43f), std::make_pair(3088, 2160));
        // In the 16:9 frame around the screen: a wide shape the frame's width, a narrower one its height.
        QVERIFY(near(shapeWidthInFrame(2.39f, 3.6f, 2.025f), 3.6f));
        QVERIFY(near(shapeWidthInFrame(4.0f / 3.0f, 3.6f, 2.025f), 2.7f));
        QVERIFY(near(shapeWidthInFrame(16.0f / 9.0f, 3.6f, 3.6f / 2.39f), 3.6f)); // from a 2.39 screen: as wide, taller
        QVERIFY(near(shapeWidthInFrame(2.39f, 2.7f, 2.025f), 3.6f)); // from a 4:3 screen: the frame's full width again
        // What is lit: one shape, the one the screen is. A size of the user's own: none (not a wrong one). The app's own
        // shape and a usual one that are the same: Original only.
        QCOMPARE(shapeLit(3500, 1700, 0, 0), -1);
        QCOMPARE(shapeLit(3500, 1700, 1920, 1080), -1);
        QCOMPARE(shapeLit(3840, 1606, 0, 0), 7);
        QCOMPARE(shapeLit(3840, 1606, 3840, 1608), 0); // the film's own: Original alone, not Original and 2.39
        QCOMPARE(shapeLit(3840, 2160, 1920, 1080), 0);
        QCOMPARE(shapeLit(2880, 2160, 3840, 1608), 1);
        QCOMPARE(shapeLit(3840, 2022, 0, 0), 5); // IMAX 1.90
        QCOMPARE(shapeLit(3088, 2160, 0, 0), 2); // IMAX film 1.43
        QCOMPARE(shapeLit(0, 2160, 0, 0), -1);

        // Original: the size the app chose, not one asked with the resize button.
        SpatialLayout layout;
        layout.sync({{1, "mpv", 3840, 1608, false, 2.0f}}, headAt());
        layout.askedSize(1, 2000, 1500);
        layout.sync({{1, "mpv", 2000, 1500, false, 2.0f}}, headAt());
        QCOMPARE(layout.windows().at(1).naturalWidthPx, 3840);
        QCOMPARE(layout.windows().at(1).naturalHeightPx, 1608);
        // In cinema, the 2.39 shape: once the app has its pixels, the screen as wide as asked, and remembered.
        layout.enterCinema(1, headAt());
        layout.sync({{1, "mpv", 3840, 2160, true, 2.0f}}, headAt());
        // Made 8K with the resize button first: the 2.39 shape's 4K pixels at that scale would halve the screen.
        layout.askedSize(1, 7680, 4320);
        layout.sync({{1, "mpv", 7680, 4320, true, 2.0f}}, headAt());
        const float wide = layout.widthMeters(layout.windows().at(1));
        const auto [w, h] = screenPixels(2.39f);
        layout.setCinemaShape(1, w, h, shapeWidthInFrame(2.39f, wide, layout.heightMeters(layout.windows().at(1))));
        layout.askedSize(1, w, h);
        layout.sync({{1, "mpv", w, h, true, 2.0f}}, headAt());
        QVERIFY(near(layout.widthMeters(layout.windows().at(1)), wide, 0.01f));
        QVERIFY(near(layout.heightMeters(layout.windows().at(1)), wide / (float(w) / float(h)), 0.01f));
        const float shaped = layout.widthMeters(layout.windows().at(1));
        layout.leaveCinema();
        layout.sync({{1, "mpv", 3840, 1608, false, 2.0f}}, headAt());
        // Next time: as wide as it was left (its place and width are remembered, not its shape).
        layout.enterCinema(1, headAt());
        layout.sync({{1, "mpv", 3840, 2160, true, 2.0f}}, headAt());
        QVERIFY(near(layout.widthMeters(layout.windows().at(1)), shaped, 0.01f));
        // Not the cinema screen: nothing.
        layout.setCinemaShape(7, 100, 100, 1.0f);
    }

    //! Switching shapes back and forth never shrinks the screen (Original, 16:9, the others, Original again once left
    //! it small): every shape fits the same 16:9 frame around the screen, as a real screen's
    //! masking, so Original is as large again at the end.
    void cinemaShapesComeBackToTheSameSize()
    {
        SpatialLayout layout;
        layout.sync({{1, "mpv", 3840, 1608, false, 2.0f}}, headAt()); // a 2.39:1 film's own window
        layout.enterCinema(1, headAt());
        // As the XR side does for a shape: its pixels, its width in the frame, asked of the app, which then has them.
        const auto shape = [&](float aspect) {
            const SpatialWindow &w = layout.windows().at(1);
            const auto [px, py] = screenPixels(aspect);
            layout.setCinemaShape(1, px, py, shapeWidthInFrame(aspect, layout.widthMeters(w), layout.heightMeters(w)));
            layout.askedSize(1, px, py);
            layout.sync({{1, "mpv", px, py, true, 2.0f}}, headAt());
        };
        const float original = 3840.0f / 1608.0f;
        shape(original); // entering cinema: Original
        const float width = layout.widthMeters(layout.windows().at(1));
        const float height = layout.heightMeters(layout.windows().at(1));
        for (const float aspect : {16.0f / 9.0f, 1.85f, 2.0f, 2.39f, 4.0f / 3.0f, 16.0f / 9.0f, original}) {
            shape(aspect);
        }
        QVERIFY2(near(layout.widthMeters(layout.windows().at(1)), width, 0.01f),
                 qPrintable(QStringLiteral("%1 m wide at the end, %2 m at first").arg(layout.widthMeters(layout.windows().at(1))).arg(width)));
        QVERIFY(near(layout.heightMeters(layout.windows().at(1)), height, 0.01f));
        // 16:9 fills the frame: as wide as the 2.39 screen, taller; 4:3 as high as 16:9, narrower.
        shape(16.0f / 9.0f);
        QVERIFY(near(layout.widthMeters(layout.windows().at(1)), width, 0.01f));
        const float frameHeight = layout.heightMeters(layout.windows().at(1));
        QVERIFY(frameHeight > height + 0.3f);
        shape(4.0f / 3.0f);
        QVERIFY(near(layout.heightMeters(layout.windows().at(1)), frameHeight, 0.01f));
        QVERIFY(near(layout.widthMeters(layout.windows().at(1)), frameHeight * 4.0f / 3.0f, 0.01f));
        layout.leaveCinema();
    }

    //! Windows stay inside the room drawn around the user (a sphere of 4.75 m): beyond it, a window is drawn over the room
    //! while the eyes see it behind (it floats and slides with the head, which can make people feel sick). A push stops inside it,
    //! with the frame's controls; growing stops; a window already beyond comes in looking the same; no room, no limit.
    void windowsStayInsideTheRoom()
    {
        const float limit = 4.75f - SpatialLayout::kRoomMargin;
        SpatialLayout layout;
        layout.sync({{1, "mpv", 1920, 1080}}, headAt());
        layout.setRoomBound({0, 0, 0}, 4.75f);
        const Pose controller{{}, {0, -0.1f, 0}};
        QVERIFY(layout.beginGrab(1, controller, 1.6f));
        for (int i = 0; i < 300; ++i) {
            layout.updateGrab(controller, headAt(), 1.0f, 0.0f, 0.02f); // pushed for 6 s
        }
        layout.endGrab();
        const SpatialWindow &pushed = layout.windows().at(1);
        QVERIFY2(layout.farthestFromRoomCentre(pushed) <= limit + 0.01f, qPrintable(QString::number(layout.farthestFromRoomCentre(pushed))));
        QVERIFY(layout.farthestFromRoomCentre(pushed) > limit - 0.2f); // up to the room, not stopped short
        QVERIFY(pushed.pose.position.z < -2.5f && near(pushed.scale, 1.0f)); // in front, its size kept
        // Pulled back in: it comes nearer as before.
        const float wallZ = pushed.pose.position.z;
        layout.beginGrab(1, controller, -wallZ);
        layout.updateGrab(controller, headAt(), -1.0f, 0.0f, 0.5f);
        layout.endGrab();
        QVERIFY(layout.windows().at(1).pose.position.z > wallZ + 0.3f);

        // Grown with the scale handle: no larger than the room has room for.
        layout.beginScale(1, {0, 0, 0}, (layout.surfacePose(layout.windows().at(1)).transform({0.9f, 0.5f, 0.0f})).normalized());
        layout.updateScale({0, 0, 0}, (layout.surfacePose(layout.windows().at(1)).transform({6.0f, 3.4f, 0.0f})).normalized());
        layout.endDrag();
        QVERIFY(layout.farthestFromRoomCentre(layout.windows().at(1)) <= limit + 0.01f);

        // Already beyond (placed while the room was off): in, looking the same from the room's centre.
        SpatialLayout far;
        far.sync({{2, "dolphin", 1000, 700}}, headAt());
        far.beginGrab(2, controller, 1.6f);
        for (int i = 0; i < 300; ++i) {
            far.updateGrab(controller, headAt(), 1.0f, 0.0f, 0.02f); // no room: as far as 8 m
        }
        far.endGrab();
        const SpatialWindow before = far.windows().at(2);
        QVERIFY(far.farthestFromRoomCentre(before) > 6.0f);
        const float apparent = far.widthMeters(before) / before.pose.position.length();
        far.setRoomBound({0, 0, 0}, 4.75f);
        far.keepWindowsInRoom();
        const SpatialWindow &after = far.windows().at(2);
        QVERIFY(far.farthestFromRoomCentre(after) <= limit + 0.01f);
        QVERIFY(near(far.widthMeters(after) / after.pose.position.length(), apparent, 0.002f));
        // Too large for the room anywhere: smaller as well.
        far.load(QJsonObject{});
        SpatialLayout huge;
        huge.sync({{3, "mpv", 3840, 2160}}, headAt());
        huge.setRoomBound({0, 0, 0}, 4.75f);
        huge.keepWindowsInRoom();
        QVERIFY(huge.farthestFromRoomCentre(huge.windows().at(3)) <= limit + 0.01f);
        // No room drawn: no limit.
        huge.setRoomBound({0, 0, 0}, 0.0f);
        const float scale = huge.windows().at(3).scale;
        huge.keepWindowsInRoom();
        QCOMPARE(huge.windows().at(3).scale, scale);
    }

    void frameControlsAroundTheWindow()
    {
        // A 1.0 x 0.6 m window at 1.6 m (unit 1), corners 0.022 m.
        const FrameLayout f = FrameLayout::compute(1.0f, 0.6f, 0.022f, 1.6f);
        QVERIFY(near(f.unit, 1.0f));
        // On the window: content, not a control.
        QCOMPARE(f.hit(0.0f, 0.0f), FrameControl::None);
        QCOMPARE(f.hit(0.49f, -0.29f), FrameControl::None);
        // The menu pill left of the window: five buttons top to bottom.
        QVERIFY(f.menu.x < -0.5f);
        QCOMPARE(f.hit(f.buttons[0].x, f.buttons[0].y), FrameControl::Follow);
        QCOMPARE(f.hit(f.buttons[1].x, f.buttons[1].y), FrameControl::Main);
        QCOMPARE(f.hit(f.buttons[2].x, f.buttons[2].y), FrameControl::Pin);
        QCOMPARE(f.hit(f.buttons[3].x, f.buttons[3].y), FrameControl::Size);
        QCOMPARE(f.hit(f.buttons[4].x, f.buttons[4].y), FrameControl::Cinema);
        QCOMPARE(f.hit(f.buttons[5].x, f.buttons[5].y), FrameControl::Minimize);
        QCOMPARE(f.hit(f.buttons[6].x, f.buttons[6].y), FrameControl::Close);
        QVERIFY(f.buttons[0].y > f.buttons[6].y);
        // Tilt on the right edge, grab underneath, scale at the bottom-right corner.
        QCOMPARE(f.hit(0.53f, 0.0f), FrameControl::Tilt);
        // The reset dot just above the tilt bar.
        QVERIFY(f.resetTilt.y > f.tilt.y + f.tilt.height / 2.0f);
        QCOMPARE(f.hit(f.resetTilt.x, f.resetTilt.y), FrameControl::ResetTilt);
        QCOMPARE(f.hit(0.0f, -0.33f), FrameControl::Grab);
        QCOMPARE(f.hit(0.52f, -0.32f), FrameControl::Scale);
        // The resize button just outside the top-right corner, clear of the tilt bar's reset dot and the curve's.
        QVERIFY(f.resize.x > 0.5f && f.resize.y > 0.3f);
        QCOMPARE(f.hit(f.resize.x, f.resize.y), FrameControl::Resize);
        QCOMPARE(f.hit(-0.52f, -0.32f), FrameControl::None); // no longer at the bottom-left
        QVERIFY(f.resize.y - f.resize.height / 2.0f > f.resetTilt.y + f.resetTilt.height / 2.0f);
        QVERIFY(f.resize.x - f.resize.width / 2.0f > f.resetCurve.x + f.resetCurve.width / 2.0f);
        // A short, narrow window: the menu pill (on the left) never reaches it.
        const FrameLayout small = FrameLayout::compute(0.4f, 0.25f, 0.022f, 1.6f);
        QVERIFY(small.menu.x + small.menu.width / 2.0f < small.resize.x - small.resize.width / 2.0f);
        QCOMPARE(small.hit(small.resize.x, small.resize.y), FrameControl::Resize);
        QCOMPARE(small.hit(small.resetTilt.x, small.resetTilt.y), FrameControl::ResetTilt);
        // The curvature slider above the window, its reset dot at its right end; the knob's place and back.
        QVERIFY(f.curve.y > 0.3f && near(f.curve.x, 0.0f));
        QCOMPARE(f.hit(0.0f, f.curve.y), FrameControl::Curve);
        QCOMPARE(f.hit(f.resetCurve.x, f.resetCurve.y), FrameControl::ResetCurve);
        QVERIFY(f.resetCurve.x > f.curve.x + f.curve.width / 2.0f);
        QVERIFY(near(f.curvatureAt(f.knobX(0.4f)), 0.4f) && near(f.knobX(0.0f), -f.curve.width / 2.0f));
        // The padlock at the slider's left end, as the flat dot at its right, and clear of the slider.
        QVERIFY(near(f.lock.y, f.curve.y) && f.lock.x + f.lock.width / 2.0f < f.curve.x - f.curve.width / 2.0f);
        QVERIFY(near(f.curve.x - f.lock.x, f.resetCurve.x - f.curve.x, 0.005f));
        QCOMPARE(f.hit(f.lock.x, f.lock.y), FrameControl::Lock);
        QCOMPARE(f.hit(f.curve.x - f.curve.width / 2.0f + 0.002f, f.curve.y), FrameControl::Curve);
        QCOMPARE(small.hit(small.lock.x, small.lock.y), FrameControl::Lock);
        QVERIFY(std::fabs(small.lock.x) + small.lock.width / 2.0f < 0.2f); // above the narrow window, not beside it
        // Far away: nothing; near: the frame shows.
        QCOMPARE(f.hit(0.0f, 0.5f), FrameControl::None);
        QVERIFY(f.near(-0.6f, 0.0f) && f.near(0.0f, -0.36f) && !f.near(0.0f, 0.5f) && !f.near(1.0f, 0.0f));
        // The frame shows near the edges only, not over the middle of the window.
        QVERIFY(!f.nearEdge(0.0f, 0.0f) && !f.nearEdge(0.3f, 0.1f));
        QVERIFY(f.nearEdge(0.48f, 0.0f) && f.nearEdge(0.0f, -0.28f) && f.nearEdge(-0.6f, 0.0f) && f.nearEdge(0.52f, -0.32f));
        // Twice as far: controls twice as large.
        const FrameLayout far = FrameLayout::compute(1.0f, 0.6f, 0.022f, 3.2f);
        QVERIFY(near(far.menu.width, 2.0f * f.menu.width));
    }

    void launcherGridCellsAndPages()
    {
        // Rows of 5, 6, 5 (Vision Pro's shape): 16 a page; 30 apps make two pages.
        QCOMPARE(LauncherGrid::pages(30), 2);
        QCOMPARE(LauncherGrid::pages(33), 3);
        QCOMPARE(LauncherGrid::pages(0), 1);
        for (int cell : {0, 4, 5, 10, 11, 15}) {
            float x = 0.0f;
            float y = 0.0f;
            LauncherGrid::cellCentre(cell, x, y);
            QCOMPARE(LauncherGrid::cellAt(x, y), cell);
        }
        float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
        LauncherGrid::cellCentre(0, x0, y0);
        LauncherGrid::cellCentre(7, x1, y1);
        QVERIFY(x1 > x0 && y1 < y0); // right and down
        // The short rows sit half an icon in from the long one.
        float xs = 0.0f, ys = 0.0f, xl = 0.0f, yl = 0.0f;
        LauncherGrid::cellCentre(0, xs, ys);
        LauncherGrid::cellCentre(5, xl, yl);
        QVERIFY(near(xs - xl, LauncherGrid::kCell / 2.0f));
        // The search field above the apps, not an app.
        float sx, sy, sw, sh;
        LauncherGrid::searchField(sx, sy, sw, sh);
        QVERIFY(LauncherGrid::onSearch(sx, sy) && LauncherGrid::cellAt(sx, sy) == -1);
        float topX = 0.0f, topY = 0.0f;
        LauncherGrid::cellCentre(0, topX, topY);
        QVERIFY(topY < sy && !LauncherGrid::onSearch(topX, topY));
        // The page dots, the running apps and off the grid: no app.
        QCOMPARE(LauncherGrid::cellAt(0.0f, LauncherGrid::footerY()), -1);
        QCOMPARE(LauncherGrid::cellAt(0.0f, -LauncherGrid::height() / 2.0f + 0.02f), -1);
        // Running apps: three cards centred under the grid; the close button in a card's top-right corner.
        float cx, cy, cw, ch;
        LauncherGrid::card(1, 3, cx, cy, cw, ch);
        QVERIFY(near(cx, 0.0f) && cy < LauncherGrid::footerY());
        bool close = true;
        QCOMPARE(LauncherGrid::cardAt(cx, cy, 3, close), 1);
        QVERIFY(!close);
        float bx, by, bs;
        LauncherGrid::closeButton(2, 3, bx, by, bs);
        QCOMPARE(LauncherGrid::cardAt(bx, by, 3, close), 2);
        QVERIFY(close);
        QCOMPARE(LauncherGrid::cardAt(0.0f, 0.0f, 3, close), -1);
        QCOMPARE(LauncherGrid::cellAt(LauncherGrid::width(), 0.0f), -1);
    }

    void placementsAreRememberedByAppId()
    {
        SpatialLayout layout;
        layout.sync({{1, "mpv", 1920, 1080}}, headAt());
        const Pose controller{{}, {0, -0.1f, 0}};
        layout.beginGrab(1, controller, 1.6f);
        layout.updateGrab({Quat::fromYaw(-radians(30.0f)), {0, -0.1f, 0}}, headAt(), 0.0f, 0.0f, 0.01f);
        layout.endGrab();
        QVERIFY(layout.takeDirty());
        const Vec3 placed = layout.windows().at(1).pose.position;

        SpatialLayout restarted;
        restarted.load(layout.save());
        restarted.sync({{7, "mpv", 1920, 1080}}, headAt());
        const Vec3 restored = restarted.windows().at(7).pose.position;
        QVERIFY(near(restored.x, placed.x) && near(restored.y, placed.y) && near(restored.z, placed.z));
    }

    void reclineLyingBackPutsTheFrontOverhead()
    {
        const Pose home{Quat{}, {0.0f, 1.2f, 0.0f}};
        const Pose head = lyingBack();
        const Pose recline = reclineTo(home, head);
        // A window 1.6 m in front of the seat is 1.6 m above the face, facing down at it.
        const Pose window{Quat{}, {0.0f, 1.2f, -1.6f}};
        const Pose shown = recline * window;
        QVERIFY(near(shown.position, {0.0f, 2.0f, 0.0f}));
        QVERIFY(near(shown.orientation.rotate({0.0f, 0.0f, 1.0f}), {0.0f, -1.0f, 0.0f}));
        // Its top towards the top of the head.
        QVERIFY(near(shown.orientation.rotate({0.0f, 1.0f, 0.0f}), {0.0f, 0.0f, 1.0f}));
        // The head, seen from the arrangement, is back in the seat looking ahead: Home and the dock come in front.
        const Pose inLayout = recline.inverse() * head;
        QVERIFY(near(inLayout.position, home.position));
        QVERIFY(near(inLayout.forward(), {0.0f, 0.0f, -1.0f}));
    }

    void reclineLeanKeepsWindowsLevelOnlyASideTurnsThem()
    {
        const Pose home{Quat{}, {0.0f, 1.2f, 0.0f}};
        // Leaning back 45 degrees on a sofa.
        const Pose sofa{Quat::fromPitch(radians(45.0f)), {0.0f, 1.0f, 0.2f}};
        const Vec3 level = Vec3{0.0f, 1.0f, 1.0f}.normalized(); // the room's up, square to the view
        QVERIFY(near(reclineTo(home, sofa).orientation.rotate({0.0f, 1.0f, 0.0f}), level));
        // A slight lean of the head to one side does not skew the windows.
        QVERIFY(near(std::fabs(headRoll(rolled(sofa, 10.0f))), radians(10.0f)));
        QVERIFY(near(reclineTo(home, rolled(sofa, 10.0f)).orientation.rotate({0.0f, 1.0f, 0.0f}), level));
        // Lying on one side: the windows turn with the head.
        const Pose side = rolled(sofa, 70.0f);
        QVERIFY(near(reclineTo(home, side).orientation.rotate({0.0f, 1.0f, 0.0f}), side.orientation.rotate({0.0f, 1.0f, 0.0f})));
        // The view itself is always the arrangement's front.
        QVERIFY(near(reclineTo(home, side).orientation.rotate({0.0f, 0.0f, -1.0f}), side.forward()));
    }

    void reclineFromTheSeatItselfChangesNothing()
    {
        const Pose home{Quat::fromYaw(radians(-30.0f)), {0.3f, 1.2f, -0.1f}};
        const Pose recline = reclineTo(home, home);
        for (const Vec3 p : {Vec3{0.0f, 1.2f, -1.6f}, Vec3{1.0f, 0.5f, 2.0f}}) {
            QVERIFY(near(recline.transform(p), p));
        }
    }

    void reclineTurnsAboutTheEye()
    {
        const Pose home{Quat{}, {0.0f, 1.2f, 0.0f}};
        const Pose full = reclineTo(home, lyingBack());
        const Pose start = reclineBlend(Pose{}, full, 0.0f, home.position);
        const Pose end = reclineBlend(Pose{}, full, 1.0f, home.position);
        const Vec3 window{0.0f, 1.2f, -1.6f};
        QVERIFY(near(start.transform(window), window));
        QVERIFY(near(end.transform(window), full.transform(window)));
        // Halfway: the eye halfway from the seat to the bed, the arrangement turned half as far (45 of 90 degrees).
        const Pose half = reclineBlend(Pose{}, full, 0.5f, home.position);
        QVERIFY(near(half.transform(home.position), (home.position + lyingBack().position) * 0.5f));
        QVERIFY(near(2.0f * std::acos(std::min(1.0f, std::fabs(half.orientation.w))), radians(45.0f)));
        // And back upright the same way.
        QVERIFY(near(reclineBlend(full, Pose{}, 1.0f, home.position).transform(window), window));
        QVERIFY(near(reclineEase(0.0f), 0.0f) && near(reclineEase(1.0f), 1.0f) && near(reclineEase(0.5f), 0.5f));
    }

    void aGripIsAModifierHeldFirst()
    {
        // B is a click (a user's own setting) and a grip with B reclines (the default).
        ButtonMap map = ButtonMap::defaults();
        map.assign(ButtonAction::LeftClick, *Binding::fromName(QStringLiteral("rightSecondary")));
        BindingTracker tracker;
        const InputMask b = inputBit(ControllerInput::RightSecondary);
        const InputMask grip = inputBit(ControllerInput::RightGrip);
        // B alone clicks at once and holds the click (a drag), and squeezing the grip meanwhile changes nothing.
        QVERIFY(tracker.update(map, b).isHeld(ButtonAction::LeftClick));
        const auto squeezed = tracker.update(map, InputMask(b | grip));
        QVERIFY(squeezed.isHeld(ButtonAction::LeftClick));
        QVERIFY(!squeezed.isPressed(ButtonAction::Recline));
        tracker.update(map, 0);
        // The grip first, then B: recline, and no click.
        QVERIFY(!tracker.update(map, grip).isHeld(ButtonAction::LeftClick));
        const auto chord = tracker.update(map, InputMask(grip | b));
        QVERIFY(chord.isPressed(ButtonAction::Recline));
        QVERIFY(!chord.isHeld(ButtonAction::LeftClick));
    }

    void dockHasRecline()
    {
        const auto recline = DockLayout::rect(DockItem::Recline);
        QVERIFY(DockLayout::rect(DockItem::FollowAll).x < recline.x && recline.x < DockLayout::rect(DockItem::Gamepad).x);
        QCOMPARE(DockLayout::itemAt(recline.x, recline.y), DockItem::Recline);
        QVERIFY(DockLayout::rect(DockItem::VolumeUp).x + DockLayout::kButton / 2.0f <= DockLayout::width() / 2.0f);
        // Its binding is a grip held with B, by default.
        QVERIFY(ButtonMap::defaults().bindings(ButtonAction::Recline)[0].isChord());
    }

    void dockBellBetweenSettingsAndNetwork()
    {
        using RiftHome::Spatial::DockItem;
        using RiftHome::Spatial::DockLayout;
        const auto bell = DockLayout::rect(DockItem::Notifications);
        QVERIFY(DockLayout::rect(DockItem::Settings).x < bell.x && bell.x < DockLayout::rect(DockItem::Network).x);
        QCOMPARE(DockLayout::itemAt(bell.x, bell.y), DockItem::Notifications);
        // The gap before the network: nothing between the bell and it.
        const float gapX = (bell.x + bell.width / 2.0f + DockLayout::rect(DockItem::Network).x - DockLayout::rect(DockItem::Network).width / 2.0f) / 2.0f;
        QCOMPARE(DockLayout::itemAt(gapX, 0.0f), DockItem::None);
        QVERIFY(DockLayout::rect(DockItem::VolumeUp).x + DockLayout::kButton / 2.0f <= DockLayout::width() / 2.0f);
    }

    void notificationCardAndList()
    {
        using RiftHome::Spatial::NotificationHit;
        using RiftHome::Spatial::NotificationLayout;
        using Part = NotificationHit::Part;
        // The card: its x in the top-right corner, the rest taps it.
        const auto x = NotificationLayout::closeOf({0.0f, 0.0f, NotificationLayout::kCardWidth, NotificationLayout::kCardHeight});
        QCOMPARE(NotificationLayout::cardHit(x.x, x.y).part, Part::Close);
        QCOMPARE(NotificationLayout::cardHit(-0.1f, 0.0f).part, Part::Card);
        QCOMPARE(NotificationLayout::cardHit(0.0f, NotificationLayout::kCardHeight).part, Part::None);
        // The list: grows with its rows up to kMaxRows, one row ("none") when empty.
        QCOMPARE(NotificationLayout::listHeight(0), NotificationLayout::listHeight(1));
        QVERIFY(NotificationLayout::listHeight(3) > NotificationLayout::listHeight(2));
        QCOMPARE(NotificationLayout::listHeight(9), NotificationLayout::listHeight(NotificationLayout::kMaxRows));
        // Rows top down, newest first, inside the list; "Clear all" above them.
        const int count = 3;
        for (int row = 0; row < count; ++row) {
            const auto card = NotificationLayout::listCard(row, count);
            QVERIFY(NotificationLayout::listContains(card.x - card.width / 2.0f, card.y - card.height / 2.0f, count));
            QCOMPARE(NotificationLayout::listHit(card.x - 0.05f, card.y, count), (NotificationHit{Part::Card, row}));
            const auto close = NotificationLayout::closeOf(card);
            QCOMPARE(NotificationLayout::listHit(close.x, close.y, count), (NotificationHit{Part::Close, row}));
        }
        QVERIFY(NotificationLayout::listCard(1, count).y < NotificationLayout::listCard(0, count).y);
        const auto clear = NotificationLayout::clearAll(count);
        QVERIFY(clear.y > NotificationLayout::listCard(0, count).y);
        QCOMPARE(NotificationLayout::listHit(clear.x, clear.y, count).part, Part::ClearAll);
        QCOMPARE(NotificationLayout::listHit(clear.x, clear.y, 0).part, Part::None); // nothing to clear
        // Past the shown rows nothing is hit.
        const auto last = NotificationLayout::listCard(NotificationLayout::kMaxRows - 1, 9);
        QCOMPARE(NotificationLayout::listHit(last.x, last.y - last.height, 9).part, Part::None);
    }

    void notificationActionButtons()
    {
        using RiftHome::Spatial::NotificationHit;
        using RiftHome::Spatial::NotificationLayout;
        using Part = NotificationHit::Part;
        // A card with two actions is taller; its buttons sit side by side along its bottom, inside it, apart.
        const float tall = NotificationLayout::cardHeight(2);
        QVERIFY(tall > NotificationLayout::kCardHeight);
        QCOMPARE(NotificationLayout::cardHeight(0), NotificationLayout::kCardHeight);
        const RiftHome::Spatial::FrameRect card{0.0f, 0.0f, NotificationLayout::kCardWidth, tall};
        const auto first = NotificationLayout::actionOf(card, 0, 2);
        const auto second = NotificationLayout::actionOf(card, 1, 2);
        QVERIFY(first.x < second.x && first.x + first.width / 2.0f < second.x - second.width / 2.0f);
        QVERIFY(first.y - first.height / 2.0f > -tall / 2.0f && first.x - first.width / 2.0f > -card.width / 2.0f);
        QCOMPARE(NotificationLayout::cardHit(second.x, second.y, 2), (NotificationHit{Part::Action, 0, 1}));
        // Above the buttons it is the card; its x still closes it.
        QCOMPARE(NotificationLayout::cardHit(first.x, first.y + 0.03f, 2).part, Part::Card);
        const auto x = NotificationLayout::closeOf(card);
        QCOMPARE(NotificationLayout::cardHit(x.x, x.y, 2).part, Part::Close);
        // Without actions the same point is just the card (no buttons there).
        QCOMPARE(NotificationLayout::cardHit(first.x, first.y, 0).part, Part::Card);
        // In the list: rows of their own heights, newest first; a row's buttons hit as that row's.
        const std::vector<int> actions{0, 2, 0};
        const int count = 3;
        QVERIFY(NotificationLayout::listHeight(count, actions) > NotificationLayout::listHeight(count));
        const auto row1 = NotificationLayout::listCard(1, count, actions);
        QCOMPARE(row1.height, tall);
        const auto row0 = NotificationLayout::listCard(0, count, actions);
        const auto row2 = NotificationLayout::listCard(2, count, actions);
        QVERIFY(row0.y - row0.height / 2.0f > row1.y + row1.height / 2.0f); // a gap between rows
        QVERIFY(row1.y - row1.height / 2.0f > row2.y + row2.height / 2.0f);
        const auto button = NotificationLayout::actionOf(row1, 1, 2);
        QCOMPARE(NotificationLayout::listHit(button.x, button.y, count, actions), (NotificationHit{Part::Action, 1, 1}));
        QVERIFY(NotificationLayout::listContains(row2.x, row2.y - row2.height / 2.0f, count, actions));
    }

    void touchControllersAsAGamepad()
    {
        using namespace RiftHome::Spatial;
        TouchInput left;
        TouchInput right;
        // At rest, a little off the middle (a stick never rests at exactly 0): nothing.
        left.stickX = 0.05f;
        QCOMPARE(gamepadFrom(left, right), GamepadState{});
        // Full right and up on the left stick: x at the top, y at the bottom (joysticks count down as positive).
        left.stickX = 1.0f;
        left.stickY = 1.0f;
        GamepadState s = gamepadFrom(left, right);
        QCOMPARE(s.lx, int16_t(32767));
        QCOMPARE(s.ly, int16_t(-32767));
        // Triggers 0..255, grips as shoulder buttons past half, the face buttons where an Xbox pad has them.
        right.trigger = 1.0f;
        left.trigger = 0.5f;
        right.squeeze = 0.7f;
        right.lower = true; // A
        left.upper = true; // Y
        s = gamepadFrom(left, right);
        QCOMPARE(s.rt, uint8_t(255));
        QCOMPARE(s.lt, uint8_t(128));
        QVERIFY(s.rb && !s.lb && s.a && s.y && !s.b && !s.x);
        QCOMPARE(gamepadAxis(-2.0f), int16_t(-32767)); // clamped
    }

    //! The copy's density follows Picture quality: 1.95 times the runtime's 1:1 density at the standard 165 %, as much
    //! more as the runtime's picture is denser above it, so the copy stays as many times denser than that picture.
    void copiesFollowPictureQuality()
    {
        QVERIFY(near(Sharpness::targetDensity(793.0f, 165), 1.95f * 793.0f, 0.5f));
        QVERIFY(near(Sharpness::targetDensity(793.0f, 200), 1.95f * 793.0f * 200.0f / 165.0f, 0.5f));
        QVERIFY(near(Sharpness::targetDensity(793.0f, 100), Sharpness::targetDensity(793.0f, 165))); // never less than standard
        // Against the runtime's picture (percent of the panel's pixels a side): the same ratio at 165 and at 200.
        const auto ratio = [](int quality) {
            return Sharpness::targetDensity(793.0f, quality) / (float(quality) * 793.0f);
        };
        QVERIFY(near(ratio(165), ratio(200), 1e-6f));
    }

    void farWindowsGetAFilteredCopy()
    {
        using S = RiftHome::Spatial::Sharpness;
        const float target = 1545.0f; // the CV1: 1.95 x 793 px a radian
        // Near, or at about the target's density: the window's own pixels (scaling by less than 0.9 only softens).
        QCOMPARE(S::wantedLevel(1000.0f, 1.0f, target), 0.0f);
        QCOMPARE(S::wantedLevel(1000.0f, 1.6f, target), 0.0f);
        // 3 m: 3000 px a radian, about half the pixels a side (level 6 = 0.5).
        const float far = S::wantedLevel(1000.0f, 3.0f, target);
        QVERIFY(far > 5.5f && far < 6.0f);
        QCOMPARE(S::nextLevel(0, far), 6);
        QCOMPARE(S::shownSize(1920, 6), 960);
        // A window made bigger (fewer pixels a metre) is sharp again where it was too dense.
        QCOMPARE(S::wantedLevel(500.0f, 3.0f, target), 0.0f);
        // Very far: at most an eighth a side; off when the runtime's density is not known.
        QCOMPARE(S::nextLevel(0, S::wantedLevel(1000.0f, 40.0f, target)), S::kMaxLevel);
        QCOMPARE(S::wantedLevel(1000.0f, 40.0f, 0.0f), 0.0f);
        // Nearer than a quarter metre counts as a quarter metre (no division by zero): enlarged, sharply.
        QCOMPARE(S::wantedLevel(1000.0f, 0.0f, target), float(S::kUpLevel));
    }

    void sharpnessLevelsDoNotFlipBackAndForth()
    {
        using S = RiftHome::Spatial::Sharpness;
        // Within three quarters of a step: stays; beyond: the nearest level.
        QCOMPARE(S::nextLevel(4, 4.7f), 4);
        QCOMPARE(S::nextLevel(4, 3.3f), 4);
        QCOMPARE(S::nextLevel(4, 4.8f), 5);
        QCOMPARE(S::nextLevel(4, 3.2f), 3);
        // Coming nearer: back to the full image once the wanted level is under 0.25.
        QCOMPARE(S::nextLevel(1, 0.3f), 1);
        QCOMPARE(S::nextLevel(1, 0.0f), 0);
        // Just under 0.9 a side starts at level 1 (0.89).
        QCOMPARE(S::nextLevel(0, S::wantedLevel(1000.0f, 1.545f / 0.89f, 1545.0f)), 1);
        QCOMPARE(S::shownSize(1000, 0), 1000);
        QCOMPARE(S::shownSize(1000, 1), 891);
    }

    void nearWindowsGetASharperBiggerCopy()
    {
        using S = RiftHome::Spatial::Sharpness;
        const float target = 1545.0f; // 1.95 x 793
        // 0.5 m at 1000 px a metre: 500 px a radian, under 0.8 of 793: the 2x copy.
        QCOMPARE(S::nextLevel(0, S::wantedLevel(1000.0f, 0.5f, target)), S::kUpLevel);
        QCOMPARE(S::shownSize(1280, S::kUpLevel), 2560);
        QCOMPARE(S::scaleOf(S::kUpLevel), 2.0f);
        // Between 0.8 and 0.9 of it (0.68 m): whichever it shows stays.
        const float between = S::wantedLevel(1000.0f, 0.68f, target);
        QCOMPARE(S::nextLevel(0, between), 0);
        QCOMPARE(S::nextLevel(S::kUpLevel, between), S::kUpLevel);
        // About 1:1 (0.8 m) and farther: its own pixels again.
        QCOMPARE(S::nextLevel(S::kUpLevel, S::wantedLevel(1000.0f, 0.8f, target)), 0);
        // Straight from near to far: the filtered smaller copy.
        QCOMPARE(S::nextLevel(S::kUpLevel, S::wantedLevel(1000.0f, 3.0f, target)), 6);
    }
};

QTEST_GUILESS_MAIN(TestSpatial)
#include "tst_spatial.moc"
