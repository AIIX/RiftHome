// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QColor>
#include <QImage>
#include <QList>
#include <QString>
#include <QStringList>

namespace RiftHome::Spatial
{
class VirtualKeyboard;
}

namespace RiftHome::Xr
{

/*!
 * The window frame's pictures (a Vision Pro look): a translucent glass menu pill with icons, bars, the
 * scale arc, in a dark or a light style. Premultiplied RGBA8888 (sRGB values), drawn at kPixelsPerUnit pixels per frame
 * unit (a metre at 1.6 m), so the art fills the frame rectangles of Spatial::FrameLayout exactly.
 */
namespace FrameArt
{

constexpr float kPixelsPerUnit = 2000.0f;

//! Dark: dark glass, white icons. Light: light glass, dark icons.
enum class Theme {
    Dark,
    Light,
};

//! Idle: the frame is hidden (only the grab bar shows, dim); Shown: the ray is near; Hovered / Pressed: on it.
enum class State {
    Idle,
    Shown,
    Hovered,
    Pressed,
};

//! What the size button does next: normal -> maximized -> fullscreen -> normal.
enum class SizeAction {
    Maximize,
    Fullscreen,
    Restore,
};

struct Menu {
    int hovered = -1; //!< button index (Spatial::kMenuOrder), -1: none
    int pressed = -1;
    bool follow = false;
    bool pinned = false;
    bool keyboard = false; //!< the virtual keyboard is open
    bool cinema = false; //!< the window is in cinema mode
    SizeAction size = SizeAction::Maximize;
    bool operator==(const Menu &) const = default;
};

/*!
 * The runtime blurs what is behind Rift Home's glass (RiftLinux's backdrop blur): the glass is drawn clearer, so the
 * frost shows (set once, before any art is drawn).
 */
void setFrostedGlass(bool frosted);
QImage menu(const Menu &state, Theme theme);
//! A capsule bar, @p length and the bar's thickness in frame units.
QImage bar(bool vertical, float length, State state, Theme theme);
//! The reset dot above the tilt bar: a small round button with a turn-back arrow.
QImage resetDot(State state, Theme theme);
//! The dot at the curvature slider's right end: a small round button with a straight line (flat again).
QImage flatDot(State state, Theme theme);
//! The curvature slider's knob: a round, brighter handle that sits on its track.
QImage knob(State state, Theme theme);
//! The resize button (top-right corner, FrameLayout::kResizeDot): a round button with two arrows pointing apart.
QImage resizeDot(State state, Theme theme);
//! The padlock at the curvature slider's left end (FrameLayout::kLockDot): open while the window can be moved, closed
//! and lit (as the menu's buttons that are on) while its place is locked.
QImage lockDot(bool locked, State state, Theme theme);
//! The L-shaped scale handle around a rounded corner of @p cornerRadius frame units.
QImage scaleCorner(float cornerRadius, State state, Theme theme);
//! The virtual keyboard: glass with key caps and their labels (as they read now: shifted, caps lock), active modifiers lit.
QImage keyboard(const Spatial::VirtualKeyboard &keyboard, Theme theme);
//! What the dock shows (Spatial::DockLayout): toggles lit, the time, the volume.
struct Dock {
    bool keyboard = false;
    bool followAll = false;
    bool reclined = false; //!< the arrangement turned to the view (recline)
    bool settings = false; //!< the Settings window is open
    bool gamepad = false; //!< game mode is on
    int notifications = 0; //!< how many: a badge on the bell
    bool notificationsOpen = false; //!< their list shows
    int network = 0; //!< LayerBridge::Network: 0 unknown, 1 disconnected, 2 wired, 3 Wi-Fi
    int signal = 0; //!< Wi-Fi strength, 0..100
    bool vpn = false; //!< a VPN is up: a lock on the network icon
    int volume = -1; //!< percent; -1: unknown
    bool muted = false;
    QString time;
    bool operator==(const Dock &) const = default;
};
QImage dock(const Dock &state, Theme theme);
//! A notification as its card shows it (Spatial::NotificationLayout).
struct NotificationArt {
    QString app;
    QString summary;
    QString body;
    QImage icon;
    bool critical = false;
    QStringList actions; //!< its buttons' labels
};
//! What the ray is on: 0 nothing, 1 the card, 2 its close button, kHoveredAction + i its i-th action button.
constexpr int kHoveredAction = 10;
QImage notificationCard(const NotificationArt &notification, int hovered, Theme theme);
//! The dock's list: the newest @p list (at most NotificationLayout::kMaxRows) of @p count, "Clear all"; @p hovered: the ray's
//! (as notificationCard's, ClearAll's part for "Clear all").
QImage notificationList(const QList<NotificationArt> &list, int count, int hoveredRow, int hoveredPart, Theme theme);
//! Home's page (LauncherGrid): round icon plates floating in the room (no panel), names under them, page dots below.
//! An open window as Home's running apps show it.
struct RunningApp {
    QString title;
    QImage icon;
    bool minimized = false;
    bool vr = false; //!< another VR app (RiftLinux's own app list): a VR mark, no close button
};
//! Home: the apps of @p page (of @p pages), the search field (@p query; @p searching: being typed), the running apps.
QImage launcher(const QStringList &names, const QList<QImage> &icons, int page, int pages, const QList<RunningApp> &running,
                const QString &query, bool searching, Theme theme);
//! One app's plate, larger: drawn over its place while the ray is on it.
QImage homePlate(const QImage &icon, Theme theme);
/*!
 * An icon's plate colour (the minimal colour, not the major, as Plasma Bigscreen and
 * Kirigami's ImageColors do; not white plates in the dark style): the icon's most vivid colour that is not its
 * dominant one, as a deep tint in the dark style and a pale tint in the light one.
 */
QColor plateColour(const QImage &icon, Theme theme);
//! A small glowing neon blue dot: the window the virtual keyboard types into.
QImage focusDot();
//! What a drag and drop carries (otherwise nothing shows it): a page on a small glass tile, beside the cursor.
QImage dragBadge(Theme theme);
//! A highlight over a key: hovered or pressed.
QImage keyHighlight(bool pressed, Theme theme);
//! A label (what the hovered menu button does), right-aligned in a kLabelWidth x kLabelHeight image.
QImage label(const QString &text, Theme theme);
constexpr float kLabelWidth = 0.30f; //!< frame units ("Stop following" whole at a degree's text)
constexpr float kLabelHeight = 0.058f;
/*!
 * Cinema mode's row of shapes under the screen (Spatial::kAspectOrder): each its outline, its name and its ratio
 * (Original: the film's own, @p original "2.39:1" of @p originalAspect; empty: not known, dimmed). One look a meaning:
 * the screen's shape now (@p lit, Spatial::shapeLit; -1: none) solid, as the menu's buttons that are on; the one the
 * ray is on (@p hovered) only outlined; @p pressed a little filled; the others plain.
 */
QImage aspectBar(int lit, int hovered, int pressed, const QString &original, float originalAspect, Theme theme);
//! A window's numbers above its resize button (pointed at or dragged): up to three lines on glass, a degree's text; a
//! line too long for it is drawn smaller to fit (never cut short).
QImage stats(const QStringList &lines, Theme theme);
constexpr int kStatsLines = 4;
constexpr float kStatsWidth = 0.66f; //!< frame units
constexpr float kStatsHeight = 0.22f;
//! The shape of @p width x @p height as people name it: "16:9", "2.39:1", ... (within 1 %), else "1.67:1".
QString aspectRatio(int width, int height);

/*!
 * The lens test (Settings, Display & Room), one eye's picture, as Oculus' "Adjust Your Lenses": black, the instructions
 * above the middle, the lens spacing (@p spacingMm; 0: not known, not shown) and "Press any button to continue." below
 * it, and with @p cross a green cross at the middle (without: the runtime draws Oculus' own onto the panel there).
 * Drawn at @p pixelsPerRadian (the panel's pixels at the lens centre) on a flat picture kLensTestDegrees on a side,
 * its middle pixel (odd sizes) on the lens's axis. Opaque.
 */
QImage lensTest(float pixelsPerRadian, int spacingMm, bool cross);
constexpr float kLensTestDegrees = 60.0f;

} // namespace FrameArt

} // namespace RiftHome::Xr
