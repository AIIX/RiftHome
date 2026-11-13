// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
#include "xr/FrameArt.hpp"

#include "spatial/Dock.hpp"
#include "spatial/LauncherGrid.hpp"
#include "spatial/NotificationPanel.hpp"
#include "spatial/VirtualKeyboard.hpp"
#include "spatial/WindowFrame.hpp"

#include <QFont>
#include <QFontMetrics>
#include <QFontMetricsF>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QRadialGradient>

#include <array>
#include <cmath>

namespace RiftHome::Xr::FrameArt
{

namespace
{

using Spatial::FrameLayout;

int px(float units)
{
    return std::max(1, int(std::lround(units * kPixelsPerUnit)));
}

QImage canvas(int width, int height)
{
    QImage image(width, height, QImage::Format_RGBA8888_Premultiplied);
    image.fill(Qt::transparent);
    return image;
}

//! A style's colours: translucent glass, its rim, the ink of icons and text, a highlight behind buttons.
struct Palette {
    QColor glass;
    QColor rim;
    QColor ink;
    QColor highlight; //!< hovered / pressed / on: this colour at rising alpha
    QColor closeInk; //!< the close icon while hovered
};

Palette palette(Theme theme)
{
    if (theme == Theme::Light) {
        // Frosted glass (the light panels looked solid otherwise): white at about 60 %, a bright rim.
        return {QColor(250, 250, 252, 150), QColor(255, 255, 255, 190), QColor(30, 30, 34), QColor(30, 30, 34), QColor(205, 45, 35)};
    }
    return {QColor(26, 27, 31, 190), QColor(255, 255, 255, 50), QColor(255, 255, 255), QColor(255, 255, 255), QColor(255, 120, 110)};
}

QColor withAlpha(QColor c, int alpha)
{
    c.setAlpha(alpha);
    return c;
}

/*!
 * A panel's glass in @p rect: the style's colour at @p darkAlpha (dark) or @p lightAlpha (light); the light style's
 * with a soft sheen (brighter at the top) so it reads as glass, not paper. No blur behind: OpenXR layers cannot.
 */
//! Glass's opacity: full without the runtime's blur; clearer with it (the backdrop is frosted, not sharp, behind it).
float glassOpacity = 1.0f;

QBrush glassBrush(const Palette &colours, Theme theme, const QRectF &rect, int darkAlpha, int lightAlpha)
{
    darkAlpha = int(std::lround(darkAlpha * glassOpacity));
    lightAlpha = int(std::lround(lightAlpha * glassOpacity));
    if (theme == Theme::Dark) {
        return withAlpha(colours.glass, darkAlpha);
    }
    QLinearGradient sheen(rect.topLeft(), rect.bottomLeft());
    sheen.setColorAt(0.0, withAlpha(colours.glass, std::min(255, lightAlpha + 30)));
    sheen.setColorAt(0.45, withAlpha(colours.glass, lightAlpha));
    sheen.setColorAt(1.0, withAlpha(colours.glass, std::max(0, lightAlpha - 20)));
    return sheen;
}

//! Bars and the scale arc: the style's glass, more opaque as the ray comes near, with a rim so they show on any
//! background (the room is black for now).
std::pair<QColor, QColor> barColours(State state, Theme theme)
{
    // A little translucent even when held (3-5 % more transparency on the bars and the arc).
    int fill = 150;
    int rim = theme == Theme::Dark ? 88 : 55;
    switch (state) {
    case State::Idle:
        fill = 90;
        rim = 42;
        break;
    case State::Shown:
        break;
    case State::Hovered:
        fill = 222;
        rim = 130;
        break;
    case State::Pressed:
        fill = 242;
        rim = 185;
        break;
    }
    // Dark bars are lifted a little so they read on black; light ones get a dark rim.
    QColor body = theme == Theme::Dark ? QColor(84, 86, 96) : QColor(248, 248, 250);
    if (state == State::Hovered || state == State::Pressed) {
        body = theme == Theme::Dark ? QColor(235, 235, 240) : QColor(255, 255, 255);
    }
    const QColor edge = theme == Theme::Dark ? QColor(255, 255, 255) : QColor(0, 0, 0);
    return {withAlpha(body, fill), withAlpha(edge, rim)};
}

//! Icons in a 64 x 64 box centred on @p c, drawn with the painter's pen.
void drawIcon(QPainter &p, Spatial::FrameControl control, SizeAction size, QPointF c, float s)
{
    const auto at = [&](float x, float y) {
        return QPointF(c.x() + x * s, c.y() + y * s);
    };
    switch (control) {
    case Spatial::FrameControl::Follow: {
        // A person: head and shoulders, with motion arcs either side.
        p.drawEllipse(at(0, -11), 7 * s, 7 * s);
        QPainterPath body;
        body.moveTo(at(-13, 14));
        body.cubicTo(at(-13, 0), at(13, 0), at(13, 14));
        p.drawPath(body);
        p.drawArc(QRectF(at(-27, -16), QSizeF(16 * s, 32 * s)), 120 * 16, 120 * 16);
        p.drawArc(QRectF(at(11, -16), QSizeF(16 * s, 32 * s)), -60 * 16, 120 * 16);
        break;
    }
    case Spatial::FrameControl::Main: {
        // A screen on a stand, a dot in its centre.
        p.drawRoundedRect(QRectF(at(-20, -15), QSizeF(40 * s, 26 * s)), 4 * s, 4 * s);
        p.drawLine(at(-8, 18), at(8, 18));
        QPen dot = p.pen();
        p.save();
        p.setBrush(dot.color());
        p.setPen(Qt::NoPen);
        p.drawEllipse(at(0, -2), 4 * s, 4 * s);
        p.restore();
        break;
    }
    case Spatial::FrameControl::Pin: {
        // A map pin.
        QPainterPath pin;
        pin.moveTo(at(0, 20));
        pin.cubicTo(at(-6, 10), at(-15, 2), at(-15, -6));
        pin.arcTo(QRectF(at(-15, -21), QSizeF(30 * s, 30 * s)), 180, -180);
        pin.cubicTo(at(15, 2), at(6, 10), at(0, 20));
        p.drawPath(pin);
        p.drawEllipse(at(0, -6), 5 * s, 5 * s);
        break;
    }
    case Spatial::FrameControl::Size: {
        if (size == SizeAction::Fullscreen) {
            // A cinema screen: a wide frame with a play triangle.
            p.drawRoundedRect(QRectF(at(-22, -14), QSizeF(44 * s, 28 * s)), 4 * s, 4 * s);
            QPainterPath play;
            play.moveTo(at(-5, -7));
            play.lineTo(at(8, 0));
            play.lineTo(at(-5, 7));
            play.closeSubpath();
            p.save();
            p.setBrush(p.pen().color());
            p.drawPath(play);
            p.restore();
        } else {
            // Corners pointing out (maximize) or in (back to normal size).
            const bool out = size == SizeAction::Maximize;
            const float a = out ? 18.0f : 6.0f; // the corner's tip
            const float l = out ? -9.0f : 9.0f; // its arms, towards the centre or away from it
            for (int sx : {-1, 1}) {
                for (int sy : {-1, 1}) {
                    QPainterPath corner;
                    corner.moveTo(at(sx * a, sy * (a + l)));
                    corner.lineTo(at(sx * a, sy * a));
                    corner.lineTo(at(sx * (a + l), sy * a));
                    p.drawPath(corner);
                }
            }
        }
        break;
    }
    case Spatial::FrameControl::Keyboard: {
        // A keyboard: its outline, two rows of keys, the space bar.
        p.drawRoundedRect(QRectF(at(-22, -13), QSizeF(44 * s, 26 * s)), 4 * s, 4 * s);
        p.save();
        p.setBrush(p.pen().color());
        p.setPen(Qt::NoPen);
        for (int row = 0; row < 2; ++row) {
            for (int col = 0; col < 5; ++col) {
                p.drawEllipse(at(-14 + col * 7, -6 + row * 7), 1.9 * s, 1.9 * s);
            }
        }
        p.restore();
        p.drawLine(at(-8, 7.5f), at(8, 7.5f));
        break;
    }
    case Spatial::FrameControl::Cinema: {
        // A wide screen and a row of seats facing it.
        p.drawRoundedRect(QRectF(at(-22, -16), QSizeF(44 * s, 20 * s)), 3 * s, 3 * s);
        for (int seat = -1; seat <= 1; ++seat) {
            p.drawArc(QRectF(at(seat * 14 - 5, 9), QSizeF(10 * s, 10 * s)), 0, 180 * 16);
        }
        break;
    }
    case Spatial::FrameControl::Minimize:
        // A window going down into a line.
        p.drawRoundedRect(QRectF(at(-14, -16), QSizeF(28 * s, 18 * s)), 3 * s, 3 * s);
        p.drawLine(at(-16, 14), at(16, 14));
        break;
    case Spatial::FrameControl::Close:
        p.drawLine(at(-13, -13), at(13, 13));
        p.drawLine(at(13, -13), at(-13, 13));
        break;
    default:
        break;
    }
}

} // namespace

void setFrostedGlass(bool frosted)
{
    glassOpacity = frosted ? 0.8f : 1.0f;
}

QImage menu(const Menu &state, Theme theme)
{
    const Palette colours = palette(theme);
    const int w = px(FrameLayout::kButton);
    const int pitch = px(FrameLayout::kButton);
    const int pad = px(FrameLayout::kMenuPadding);
    const int h = Spatial::kMenuButtons * pitch + 2 * pad;
    QImage image = canvas(w, h);
    QPainter p(&image);
    p.setRenderHint(QPainter::Antialiasing);
    // The glass pill.
    p.setPen(QPen(colours.rim, 2.0));
    p.setBrush(glassBrush(colours, theme, QRectF(0, 0, w, h), 190, 150));
    p.drawRoundedRect(QRectF(1, 1, w - 2, h - 2), (w - 2) / 2.0, (w - 2) / 2.0);
    const float s = pitch / 64.0f * 0.62f; // icon scale: 64 icon units fill 62 % of a button
    for (int i = 0; i < Spatial::kMenuButtons; ++i) {
        const Spatial::FrameControl control = Spatial::kMenuOrder[i];
        const QPointF c(w / 2.0, pad + (i + 0.5) * pitch);
        const bool on = (control == Spatial::FrameControl::Follow && state.follow) || (control == Spatial::FrameControl::Pin && state.pinned)
            || (control == Spatial::FrameControl::Keyboard && state.keyboard) || (control == Spatial::FrameControl::Cinema && state.cinema);
        const float r = pitch * 0.42f;
        int fill = 0;
        if (on) {
            fill = 235;
        } else if (state.pressed == i) {
            fill = theme == Theme::Dark ? 110 : 80;
        } else if (state.hovered == i) {
            fill = theme == Theme::Dark ? 60 : 36;
        }
        if (fill > 0) {
            p.setPen(Qt::NoPen);
            p.setBrush(withAlpha(colours.highlight, fill));
            p.drawEllipse(c, r, r);
        }
        // A button that is on is drawn inverted: the highlight colour filled, the icon in the glass's colour.
        QColor ink = on ? withAlpha(colours.glass, 255) : colours.ink;
        if (control == Spatial::FrameControl::Close && state.hovered == i && !on) {
            ink = colours.closeInk;
        }
        p.setPen(QPen(ink, 5.0 * s, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        drawIcon(p, control, state.size, c, s);
    }
    return image;
}

QImage bar(bool vertical, float length, State state, Theme theme)
{
    const int thick = px(FrameLayout::kBarThickness);
    const int len = px(length);
    QImage image = canvas(vertical ? thick : len, vertical ? len : thick);
    QPainter p(&image);
    p.setRenderHint(QPainter::Antialiasing);
    const auto [body, rim] = barColours(state, theme);
    p.setPen(QPen(rim, 1.5));
    p.setBrush(body);
    const QRectF r(1.5, 1.5, image.width() - 3, image.height() - 3);
    p.drawRoundedRect(r, (thick - 3) / 2.0, (thick - 3) / 2.0);
    return image;
}

QImage resetDot(State state, Theme theme)
{
    const int size = px(FrameLayout::kDot);
    QImage image = canvas(size, size);
    QPainter p(&image);
    p.setRenderHint(QPainter::Antialiasing);
    const auto [body, rim] = barColours(state, theme);
    p.setPen(QPen(rim, 1.5));
    p.setBrush(body);
    p.drawEllipse(QRectF(1.5, 1.5, size - 3, size - 3));
    // A turn-back arrow: three quarters of a circle and its head.
    const bool lit = state == State::Hovered || state == State::Pressed;
    const QColor ink = theme == Theme::Dark ? (lit ? QColor(26, 27, 31) : QColor(255, 255, 255, 220))
                                            : QColor(30, 30, 34, lit ? 255 : 220);
    const float r = size * 0.24f;
    const QPointF c(size / 2.0, size / 2.0);
    p.setPen(QPen(ink, size * 0.075, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    p.drawArc(QRectF(c.x() - r, c.y() - r, 2 * r, 2 * r), 90 * 16, 270 * 16);
    QPainterPath head;
    head.moveTo(c.x() - r * 0.45, c.y() - r - r * 0.45);
    head.lineTo(c.x(), c.y() - r);
    head.lineTo(c.x() - r * 0.45, c.y() - r + r * 0.45);
    p.drawPath(head);
    return image;
}

QImage flatDot(State state, Theme theme)
{
    const int size = px(FrameLayout::kDot);
    QImage image = canvas(size, size);
    QPainter p(&image);
    p.setRenderHint(QPainter::Antialiasing);
    const auto [body, rim] = barColours(state, theme);
    p.setPen(QPen(rim, 1.5));
    p.setBrush(body);
    p.drawEllipse(QRectF(1.5, 1.5, size - 3, size - 3));
    // A straight line: the window flat again.
    const bool lit = state == State::Hovered || state == State::Pressed;
    const QColor ink = theme == Theme::Dark ? (lit ? QColor(26, 27, 31) : QColor(255, 255, 255, 220))
                                            : QColor(30, 30, 34, lit ? 255 : 220);
    p.setPen(QPen(ink, size * 0.085, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(QPointF(size * 0.28, size / 2.0), QPointF(size * 0.72, size / 2.0));
    return image;
}

QImage knob(State state, Theme theme)
{
    const int size = px(FrameLayout::kKnob);
    QImage image = canvas(size, size);
    QPainter p(&image);
    p.setRenderHint(QPainter::Antialiasing);
    // Brighter than the track it sits on (a bar in the same state), so it reads as the handle.
    const auto [body, rim] = barColours(state == State::Idle ? State::Shown : State::Pressed, theme);
    QColor fill = body;
    fill.setAlpha(255);
    p.setPen(QPen(rim, 2.0));
    p.setBrush(fill);
    p.drawEllipse(QRectF(2.0, 2.0, size - 4, size - 4));
    if (state == State::Hovered || state == State::Pressed) {
        QColor ring = theme == Theme::Dark ? QColor(255, 255, 255, 200) : QColor(30, 30, 34, 200);
        p.setPen(QPen(ring, size * 0.07));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(QRectF(size * 0.3, size * 0.3, size * 0.4, size * 0.4));
    }
    return image;
}

QImage resizeDot(State state, Theme theme)
{
    // A round button as the reset dots, larger, with a diagonal double arrow (as a desktop's resize cursor): "more
    // pixels", unlike the scale handle's L around the corner.
    const int size = px(FrameLayout::kResizeDot);
    QImage image = canvas(size, size);
    QPainter p(&image);
    p.setRenderHint(QPainter::Antialiasing);
    const auto [body, rim] = barColours(state, theme);
    p.setPen(QPen(rim, 1.5));
    p.setBrush(body);
    p.drawEllipse(QRectF(1.5, 1.5, size - 3, size - 3));
    const bool lit = state == State::Hovered || state == State::Pressed;
    const QColor ink = theme == Theme::Dark ? (lit ? QColor(26, 27, 31) : QColor(255, 255, 255, 230))
                                            : QColor(30, 30, 34, lit ? 255 : 220);
    const QPointF c(size / 2.0, size / 2.0);
    const qreal reach = size * 0.24; // from the centre to each arrow's tip, along the diagonal
    const qreal head = size * 0.15;
    const QPointF a = c + QPointF(-reach, reach); // bottom-left
    const QPointF b = c + QPointF(reach, -reach); // top-right
    p.setPen(QPen(ink, size * 0.08, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    p.drawLine(a, b);
    QPainterPath heads;
    heads.moveTo(b + QPointF(-head, 0.0));
    heads.lineTo(b);
    heads.lineTo(b + QPointF(0.0, head));
    heads.moveTo(a + QPointF(head, 0.0));
    heads.lineTo(a);
    heads.lineTo(a + QPointF(0.0, -head));
    p.drawPath(heads);
    return image;
}

QImage scaleCorner(float cornerRadius, State state, Theme theme)
{
    // As FrameLayout places it: the window's corner centre C is kScaleArm in from the image's left and top; the arc
    // runs around it just outside the window's rounded corner, arms along the bottom and right edges.
    const float arm = FrameLayout::kScaleArm;
    const float thick = FrameLayout::kBarThickness;
    const float outer = cornerRadius + FrameLayout::kGap + thick;
    const int size = px(arm + outer);
    QImage image = canvas(size, size);
    QPainter p(&image);
    p.setRenderHint(QPainter::Antialiasing);
    const float k = kPixelsPerUnit;
    const QPointF c(arm * k, arm * k);
    const float r = (cornerRadius + FrameLayout::kGap + thick / 2.0f) * k;
    QPainterPath path;
    path.moveTo(thick * k / 2.0, c.y() + r);
    path.lineTo(c.x(), c.y() + r);
    path.arcTo(QRectF(c.x() - r, c.y() - r, 2 * r, 2 * r), 270, 90);
    path.lineTo(c.x() + r, thick * k / 2.0);
    // The arc's outline filled and rimmed, as the bars are.
    QPainterPathStroker stroker;
    stroker.setWidth(thick * k - 3.0);
    stroker.setCapStyle(Qt::RoundCap);
    stroker.setJoinStyle(Qt::RoundJoin);
    const auto [body, rim] = barColours(state, theme);
    p.setPen(QPen(rim, 1.5));
    p.setBrush(body);
    p.drawPath(stroker.createStroke(path).simplified());
    return image;
}

QImage keyboard(const Spatial::VirtualKeyboard &kb, Theme theme)
{
    // 1600 pixels a metre: keys of 80 pixels, sharp at the keyboard's distance.
    constexpr float k = 1600.0f;
    const Palette colours = palette(theme);
    QImage image = canvas(std::max(1, int(std::lround(kb.width() * k))), std::max(1, int(std::lround(kb.height() * k))));
    QPainter p(&image);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);
    // Nearly opaque in the dark style: the room behind (candles, windows) must not show through the keys (desk capture
    // kbd-desk-2); the light style's frosted glass a little clearer, still enough for its dark labels.
    p.setPen(QPen(colours.rim, 2.0));
    p.setBrush(glassBrush(colours, theme, QRectF(0, 0, image.width(), image.height()), 238, 186));
    p.drawRoundedRect(QRectF(1, 1, image.width() - 2, image.height() - 2), 0.02 * k, 0.02 * k);
    QFont font;
    font.setWeight(QFont::DemiBold);
    const QPointF centre(image.width() / 2.0, image.height() / 2.0);
    for (size_t i = 0; i < kb.keys().size(); ++i) {
        const Spatial::KeyDef &key = kb.keys()[i];
        const Spatial::KeyRect &r = kb.rects()[i];
        const QRectF cap(centre.x() + (r.x - r.width / 2.0f) * k, centre.y() - (r.y + r.height / 2.0f) * k, r.width * k, r.height * k);
        const bool lit = (key.kind == Spatial::KeyDef::Kind::Shift && (kb.shift() || kb.capsLock()))
            || (key.kind == Spatial::KeyDef::Kind::Ctrl && kb.ctrl()) || (key.kind == Spatial::KeyDef::Kind::Alt && kb.alt());
        const bool special = key.kind != Spatial::KeyDef::Kind::Key || key.label.size() > 1 || key.width > 1.0f;
        QColor capColour = withAlpha(colours.highlight, special ? 26 : 40);
        if (lit) {
            capColour = withAlpha(colours.highlight, 220);
        }
        p.setPen(Qt::NoPen);
        p.setBrush(capColour);
        p.drawRoundedRect(cap, 0.008 * k, 0.008 * k);
        QString text = kb.label(int(i));
        if (key.kind == Spatial::KeyDef::Kind::Shift && kb.capsLock()) {
            text = QStringLiteral("Caps");
        }
        font.setPixelSize(int(cap.height() * (text.size() > 1 ? 0.30 : 0.42)));
        p.setFont(font);
        p.setPen(lit ? withAlpha(colours.glass, 255) : colours.ink);
        p.drawText(cap, Qt::AlignCenter, text);
    }
    return image;
}

namespace
{

//! A round plate in the icon's colour with the icon in it (desktop icons are not round: they sit on the plate).
void drawPlate(QPainter &p, QPointF c, float diameter, const QImage &icon, Theme theme)
{
    p.setPen(Qt::NoPen);
    // A soft shadow under the plate, so it reads over a bright room too.
    QRadialGradient shadow(c + QPointF(0, diameter * 0.04), diameter * 0.62);
    shadow.setColorAt(0.75, QColor(0, 0, 0, 70));
    shadow.setColorAt(1.0, QColor(0, 0, 0, 0));
    p.setBrush(shadow);
    p.drawEllipse(c + QPointF(0, diameter * 0.04), diameter * 0.62, diameter * 0.62);
    const QColor tint = plateColour(icon, theme);
    QLinearGradient glass(c.x(), c.y() - diameter / 2, c.x(), c.y() + diameter / 2);
    glass.setColorAt(0.0, tint.lighter(theme == Theme::Dark ? 135 : 104));
    glass.setColorAt(1.0, tint.darker(theme == Theme::Dark ? 115 : 106));
    p.setBrush(glass);
    p.drawEllipse(c, diameter / 2.0, diameter / 2.0);
    // A thin light rim, as glass catches the light.
    p.setPen(QPen(QColor(255, 255, 255, theme == Theme::Dark ? 55 : 120), diameter * 0.012));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(c, diameter / 2.0 - diameter * 0.006, diameter / 2.0 - diameter * 0.006);
    const float icon_ = diameter * 0.62f;
    p.drawImage(QRectF(c.x() - icon_ / 2, c.y() - icon_ / 2, icon_, icon_), icon);
}

} // namespace

QImage launcher(const QStringList &names, const QList<QImage> &icons, int page, int pages, const QList<RunningApp> &running,
                const QString &query, bool searching, Theme theme)
{
    using Spatial::LauncherGrid;
    constexpr float k = 1100.0f; // pixels a metre: names readable at 1.5 m
    QImage image = canvas(int(std::lround(LauncherGrid::width() * k)), int(std::lround(LauncherGrid::height() * k)));
    QPainter p(&image);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    const QPointF centre(image.width() / 2.0, image.height() / 2.0);
    QFont font;
    font.setWeight(QFont::DemiBold);
    font.setPixelSize(int(0.026f * k)); // a degree tall at 1.5 m
    p.setFont(font);
    const QFontMetrics metrics(font);
    for (int i = 0; i < names.size(); ++i) {
        float x = 0.0f;
        float y = 0.0f;
        LauncherGrid::cellCentre(i, x, y);
        const QPointF c(centre.x() + x * k, centre.y() - (y + 0.03f) * k);
        drawPlate(p, c, LauncherGrid::kPlate * k, icons.value(i), theme);
        // The name: white with a dark halo, readable over any room.
        const float w = LauncherGrid::kCell * k - 0.01f * k;
        const QString text = metrics.elidedText(names[i], Qt::ElideRight, int(w));
        const QRectF box(c.x() - w / 2.0f, c.y() + LauncherGrid::kPlate * k / 2.0f + 0.012f * k, w, 0.034f * k);
        p.setPen(QColor(0, 0, 0, 150));
        for (const QPointF d : {QPointF(1.5, 1.5), QPointF(-1.5, 1.5), QPointF(0, 2)}) {
            p.drawText(box.translated(d), Qt::AlignHCenter | Qt::AlignTop, text);
        }
        p.setPen(QColor(255, 255, 255));
        p.drawText(box, Qt::AlignHCenter | Qt::AlignTop, text);
    }
    // The search field: a glass pill with a magnifier, the query (a caret while typing) or what it is for.
    {
        const Palette colours = palette(theme);
        float sx, sy, sw, sh;
        LauncherGrid::searchField(sx, sy, sw, sh);
        const QRectF field(centre.x() + (sx - sw / 2.0f) * k, centre.y() - (sy + sh / 2.0f) * k, sw * k, sh * k);
        p.setPen(QPen(searching ? withAlpha(colours.highlight, 200) : colours.rim, searching ? 3.0 : 2.0));
        p.setBrush(glassBrush(colours, theme, field, 200, 165));
        p.drawRoundedRect(field, field.height() / 2.0, field.height() / 2.0);
        const qreal r = field.height() * 0.2;
        const QPointF lens(field.left() + field.height() * 0.55, field.center().y() - r * 0.2);
        p.setPen(QPen(colours.ink, 0.004f * k, Qt::SolidLine, Qt::RoundCap));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(lens, r, r);
        p.drawLine(lens + QPointF(r * 0.7, r * 0.7), lens + QPointF(r * 1.5, r * 1.5));
        QFont searchFont;
        searchFont.setPixelSize(int(0.028f * k));
        p.setFont(searchFont);
        const QRectF textBox = field.adjusted(field.height() * 1.0, 0, -field.height() * 0.5, 0);
        if (query.isEmpty()) {
            p.setPen(withAlpha(colours.ink, 175));
            p.drawText(textBox, Qt::AlignVCenter | Qt::AlignLeft, searching ? QStringLiteral("Type an app's name") : QStringLiteral("Search apps"));
        } else {
            p.setPen(colours.ink);
            p.drawText(textBox, Qt::AlignVCenter | Qt::AlignLeft, query + (searching ? QStringLiteral("│") : QString()));
        }
        p.setFont(font);
    }
    // Page dots.
    const float dot = 0.012f * k;
    const float y = centre.y() - LauncherGrid::footerY() * k;
    for (int i = 0; i < pages; ++i) {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(255, 255, 255, i == page ? 235 : 90));
        p.drawEllipse(QPointF(centre.x() + (i - (pages - 1) / 2.0f) * dot * 2.2f, y), dot / 2.0f, dot / 2.0f);
    }
    // The running apps: a glass card per open window (icon, title; minimized ones dimmed), a close button in its corner.
    const Palette colours = palette(theme);
    const int count = std::min(int(running.size()), LauncherGrid::kMaxCards);
    QFont title;
    title.setWeight(QFont::DemiBold);
    title.setPixelSize(int(0.026f * k));
    QFont small;
    small.setPixelSize(int(0.022f * k));
    for (int i = 0; i < count; ++i) {
        const RunningApp &app = running[i];
        float x, cy, w, h;
        LauncherGrid::card(i, count, x, cy, w, h);
        const QRectF box(centre.x() + (x - w / 2.0f) * k, centre.y() - (cy + h / 2.0f) * k, w * k, h * k);
        p.setOpacity(app.minimized ? 0.6 : 1.0);
        p.setPen(QPen(colours.rim, 2.0));
        p.setBrush(glassBrush(colours, theme, box, 210, 170));
        p.drawRoundedRect(box, box.height() * 0.3, box.height() * 0.3);
        const qreal iconSize = box.height() * 0.62;
        const QRectF iconBox(box.left() + box.height() * 0.19, box.center().y() - iconSize / 2.0, iconSize, iconSize);
        if (!app.icon.isNull()) {
            p.drawImage(iconBox, app.icon);
        } else if (app.vr) {
            // A headset seen from the front: the visor and its two lenses.
            const QRectF visor = iconBox.adjusted(iconBox.width() * 0.04, iconBox.height() * 0.24, -iconBox.width() * 0.04, -iconBox.height() * 0.24);
            p.setPen(Qt::NoPen);
            p.setBrush(colours.ink);
            p.drawRoundedRect(visor, visor.height() * 0.35, visor.height() * 0.35);
            p.setBrush(withAlpha(colours.glass, 255)); // lenses cut out of the visor
            const qreal lens = visor.height() * 0.32;
            p.drawEllipse(QPointF(visor.left() + visor.width() * 0.3, visor.center().y()), lens, lens);
            p.drawEllipse(QPointF(visor.left() + visor.width() * 0.7, visor.center().y()), lens, lens);
        }
        const QRectF text(iconBox.right() + 0.012f * k, box.top(), box.right() - iconBox.right() - 0.05f * k, box.height());
        p.setPen(colours.ink);
        p.setFont(title);
        const QString name = QFontMetrics(title).elidedText(app.title.isEmpty() ? QStringLiteral("Window") : app.title, Qt::ElideRight, int(text.width()));
        p.drawText(text.adjusted(0, app.minimized || app.vr ? -0.026f * k : 0, 0, 0), Qt::AlignVCenter | Qt::AlignLeft, name);
        if (app.minimized || app.vr) {
            p.setFont(small);
            p.setPen(withAlpha(colours.ink, 205));
            p.drawText(text.adjusted(0, 0.032f * k, 0, 0), Qt::AlignVCenter | Qt::AlignLeft,
                       app.vr ? QStringLiteral("VR app") : QStringLiteral("minimised"));
        }
        p.setOpacity(1.0);
        if (app.vr) {
            continue; // not Rift Home's to close
        }
        float bx, by, size;
        LauncherGrid::closeButton(i, count, bx, by, size);
        const QPointF bc(centre.x() + bx * k, centre.y() - by * k);
        p.setPen(Qt::NoPen);
        p.setBrush(withAlpha(colours.ink, 40));
        p.drawEllipse(bc, size * k / 2.0, size * k / 2.0);
        p.setPen(QPen(colours.ink, 0.0035f * k, Qt::SolidLine, Qt::RoundCap));
        const qreal arm = size * k * 0.2;
        p.drawLine(bc + QPointF(-arm, -arm), bc + QPointF(arm, arm));
        p.drawLine(bc + QPointF(arm, -arm), bc + QPointF(-arm, arm));
    }
    return image;
}

QImage homePlate(const QImage &icon, Theme theme)
{
    QImage image = canvas(240, 240);
    QPainter p(&image);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    drawPlate(p, QPointF(120, 116), 190, icon, theme);
    return image;
}

QColor plateColour(const QImage &icon, Theme theme)
{
    // Colours of the icon's opaque pixels in coarse bins (8 levels a channel): how many, and their average.
    struct Bin {
        int count = 0;
        double r = 0, g = 0, b = 0;
    };
    std::array<Bin, 512> bins{};
    const QImage img = icon.convertToFormat(QImage::Format_RGBA8888);
    int opaque = 0;
    for (int y = 0; y < img.height(); y += 2) {
        const uchar *row = img.constScanLine(y);
        for (int x = 0; x < img.width(); x += 2) {
            const uchar *px = row + x * 4;
            if (px[3] < 160) {
                continue;
            }
            Bin &bin = bins[size_t((px[0] >> 5) << 6 | (px[1] >> 5) << 3 | (px[2] >> 5))];
            ++bin.count;
            bin.r += px[0];
            bin.g += px[1];
            bin.b += px[2];
            ++opaque;
        }
    }
    QColor chosen(128, 128, 128);
    if (opaque > 0) {
        const auto average = [](const Bin &bin) {
            return QColor(int(bin.r / bin.count), int(bin.g / bin.count), int(bin.b / bin.count));
        };
        const auto dominant = std::max_element(bins.begin(), bins.end(), [](const Bin &a, const Bin &b) {
            return a.count < b.count;
        });
        chosen = average(*dominant);
        // The most vivid of the other colours that cover at least 4 % of the icon (its accent, not its bulk).
        double best = -1.0;
        for (auto it = bins.begin(); it != bins.end(); ++it) {
            if (it == dominant || it->count < opaque / 25) {
                continue;
            }
            const QColor c = average(*it);
            const double vivid = c.hsvSaturationF() * (0.4 + 0.6 * c.valueF());
            if (vivid > best) {
                best = vivid;
                chosen = c;
            }
        }
        if (best < 0.08) {
            chosen = average(*dominant); // a grey icon: its own colour
        }
    }
    const float hue = std::max(0.0f, chosen.hsvHueF());
    const float saturation = chosen.hsvSaturationF();
    if (theme == Theme::Dark) {
        return QColor::fromHsvF(hue, std::min(saturation, 0.55f), 0.30f, 1.0f);
    }
    return QColor::fromHsvF(hue, std::min(saturation * 0.3f, 0.2f), 0.95f, 1.0f);
}

QImage focusDot()
{
    QImage image = canvas(96, 96);
    QPainter p(&image);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    QRadialGradient glow(QPointF(48, 48), 48);
    glow.setColorAt(0.0, QColor(150, 235, 255, 255));
    glow.setColorAt(0.28, QColor(40, 200, 255, 255));
    glow.setColorAt(0.42, QColor(0, 170, 255, 150));
    glow.setColorAt(1.0, QColor(0, 140, 255, 0));
    p.setBrush(glow);
    p.drawEllipse(QPointF(48, 48), 48, 48);
    return image;
}

QImage dock(const Dock &state, Theme theme)
{
    using Spatial::DockItem;
    using Spatial::DockLayout;
    constexpr float k = 1600.0f;
    const Palette colours = palette(theme);
    QImage image = canvas(int(std::lround(DockLayout::width() * k)), int(std::lround(DockLayout::height() * k)));
    QPainter p(&image);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);
    p.setPen(QPen(colours.rim, 2.0));
    p.setBrush(glassBrush(colours, theme, QRectF(0, 0, image.width(), image.height()), 232, 176));
    p.drawRoundedRect(QRectF(1, 1, image.width() - 2, image.height() - 2), image.height() / 2.0 - 1, image.height() / 2.0 - 1);
    const QPointF centre(image.width() / 2.0, image.height() / 2.0);
    const auto at = [&](DockItem item) {
        const Spatial::FrameRect r = DockLayout::rect(item);
        return QRectF(centre.x() + (r.x - r.width / 2.0f) * k, centre.y() - (r.y + r.height / 2.0f) * k, r.width * k, r.height * k);
    };
    const float s = DockLayout::kButton * k / 64.0f * 0.6f;
    const auto pen = [&](const QColor &c) {
        p.setPen(QPen(c, 5.0 * s, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
    };
    const auto button = [&](DockItem item, bool on) {
        const QRectF r = at(item);
        if (on) {
            p.setPen(Qt::NoPen);
            p.setBrush(withAlpha(colours.highlight, 235));
            p.drawEllipse(r.center(), r.height() * 0.42, r.height() * 0.42);
        }
        pen(on ? withAlpha(colours.glass, 255) : colours.ink);
        return r.center();
    };
    // Home: nine dots (the apps).
    {
        const QPointF c = button(DockItem::Home, false);
        p.setPen(Qt::NoPen);
        p.setBrush(colours.ink);
        for (int row = -1; row <= 1; ++row) {
            for (int col = -1; col <= 1; ++col) {
                p.drawEllipse(QPointF(c.x() + col * 11 * s, c.y() + row * 11 * s), 3.6 * s, 3.6 * s);
            }
        }
    }
    drawIcon(p, Spatial::FrameControl::Keyboard, SizeAction::Maximize, button(DockItem::Keyboard, state.keyboard), s);
    drawIcon(p, Spatial::FrameControl::Follow, SizeAction::Maximize, button(DockItem::FollowAll, state.followAll), s);
    // Recline: someone lying back on a lounger (its back rest and seat, the head, the body and the legs on it).
    {
        const QPointF c = button(DockItem::Recline, state.reclined);
        p.drawLine(QPointF(c.x() - 20 * s, c.y() - 12 * s), QPointF(c.x() - 12 * s, c.y() + 12 * s));
        p.drawLine(QPointF(c.x() - 12 * s, c.y() + 12 * s), QPointF(c.x() + 20 * s, c.y() + 12 * s));
        p.drawLine(QPointF(c.x() - 8 * s, c.y() - 8 * s), QPointF(c.x() - 1 * s, c.y() + 4 * s));
        p.drawLine(QPointF(c.x() - 1 * s, c.y() + 4 * s), QPointF(c.x() + 17 * s, c.y() + 4 * s));
        const QPen ink = p.pen();
        p.setPen(Qt::NoPen);
        p.setBrush(ink.color());
        p.drawEllipse(QPointF(c.x() - 10 * s, c.y() - 15 * s), 4.6 * s, 4.6 * s);
        p.setPen(ink);
        p.setBrush(Qt::NoBrush);
    }
    // Game mode: a gamepad (a rounded body with two grips, a cross on the left, two buttons on the right).
    {
        const QPointF c = button(DockItem::Gamepad, state.gamepad);
        QPainterPath body;
        body.moveTo(c.x() - 18 * s, c.y() - 6 * s);
        body.cubicTo(c.x() - 20 * s, c.y() + 14 * s, c.x() - 13 * s, c.y() + 16 * s, c.x() - 9 * s, c.y() + 8 * s);
        body.lineTo(c.x() + 9 * s, c.y() + 8 * s);
        body.cubicTo(c.x() + 13 * s, c.y() + 16 * s, c.x() + 20 * s, c.y() + 14 * s, c.x() + 18 * s, c.y() - 6 * s);
        body.cubicTo(c.x() + 17 * s, c.y() - 12 * s, c.x() + 12 * s, c.y() - 12 * s, c.x() + 8 * s, c.y() - 10 * s);
        body.lineTo(c.x() - 8 * s, c.y() - 10 * s);
        body.cubicTo(c.x() - 12 * s, c.y() - 12 * s, c.x() - 17 * s, c.y() - 12 * s, c.x() - 18 * s, c.y() - 6 * s);
        p.drawPath(body);
        p.drawLine(QPointF(c.x() - 11 * s, c.y() - 2 * s), QPointF(c.x() - 5 * s, c.y() - 2 * s));
        p.drawLine(QPointF(c.x() - 8 * s, c.y() - 5 * s), QPointF(c.x() - 8 * s, c.y() + 1 * s));
        const QPen ink = p.pen();
        p.setPen(Qt::NoPen);
        p.setBrush(ink.color());
        p.drawEllipse(QPointF(c.x() + 6 * s, c.y() - 1 * s), 2.2 * s, 2.2 * s);
        p.drawEllipse(QPointF(c.x() + 11 * s, c.y() - 4 * s), 2.2 * s, 2.2 * s);
        p.setPen(ink);
        p.setBrush(Qt::NoBrush);
    }
    // Screenshot: a camera (a body with a raised top, the lens).
    {
        const QPointF c = button(DockItem::Screenshot, false);
        QPainterPath body;
        body.addRoundedRect(QRectF(c.x() - 17 * s, c.y() - 9 * s, 34 * s, 23 * s), 4 * s, 4 * s);
        p.drawPath(body);
        p.drawLine(QPointF(c.x() - 7 * s, c.y() - 9 * s), QPointF(c.x() - 4 * s, c.y() - 14 * s));
        p.drawLine(QPointF(c.x() - 4 * s, c.y() - 14 * s), QPointF(c.x() + 4 * s, c.y() - 14 * s));
        p.drawLine(QPointF(c.x() + 4 * s, c.y() - 14 * s), QPointF(c.x() + 7 * s, c.y() - 9 * s));
        p.drawEllipse(QPointF(c.x(), c.y() + 2.5 * s), 6.5 * s, 6.5 * s);
    }
    // Settings: a gear (eight teeth around a ring).
    {
        const QPointF c = button(DockItem::Settings, state.settings);
        QPainterPath gear;
        for (int i = 0; i < 8; ++i) {
            const double a = i * M_PI / 4.0;
            const QPointF dir(std::cos(a), std::sin(a));
            const QPointF side(-dir.y(), dir.x());
            QPolygonF tooth;
            tooth << c + dir * (13 * s) + side * (4.2 * s) << c + dir * (19.5 * s) + side * (3.2 * s) << c + dir * (19.5 * s) - side * (3.2 * s)
                  << c + dir * (13 * s) - side * (4.2 * s);
            gear.addPolygon(tooth);
            gear.closeSubpath();
        }
        gear.addEllipse(c, 14.5 * s, 14.5 * s);
        gear.setFillRule(Qt::WindingFill);
        const QPen ring = p.pen();
        p.setPen(Qt::NoPen);
        p.setBrush(ring.color());
        p.drawPath(gear.simplified());
        p.setBrush(state.settings ? withAlpha(colours.highlight, 235) : withAlpha(colours.glass, 255));
        p.drawEllipse(c, 6.5 * s, 6.5 * s);
    }
    // The notifications: a bell, lit while their list shows, their count on a badge.
    {
        const QPointF c = button(DockItem::Notifications, state.notificationsOpen);
        QPainterPath bell;
        bell.moveTo(c.x() - 14 * s, c.y() + 10 * s);
        bell.cubicTo(c.x() - 10 * s, c.y() + 4 * s, c.x() - 11 * s, c.y() - 3 * s, c.x() - 10 * s, c.y() - 6 * s);
        bell.cubicTo(c.x() - 9 * s, c.y() - 17 * s, c.x() + 9 * s, c.y() - 17 * s, c.x() + 10 * s, c.y() - 6 * s);
        bell.cubicTo(c.x() + 11 * s, c.y() - 3 * s, c.x() + 10 * s, c.y() + 4 * s, c.x() + 14 * s, c.y() + 10 * s);
        bell.closeSubpath();
        p.drawPath(bell);
        p.drawLine(QPointF(c.x() - 4 * s, c.y() + 15 * s), QPointF(c.x() + 4 * s, c.y() + 15 * s)); // the clapper
        if (state.notifications > 0) {
            const QPointF badge(c.x() + 15 * s, c.y() - 14 * s);
            const qreal r = 13 * s; // readable in the headset: about half a degree at the dock's distance
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(235, 70, 60));
            p.drawEllipse(badge, r, r);
            QFont count;
            count.setWeight(QFont::Bold);
            count.setPixelSize(int(r * (state.notifications > 9 ? 1.05 : 1.35)));
            p.setFont(count);
            p.setPen(Qt::white);
            p.drawText(QRectF(badge.x() - r, badge.y() - r, 2 * r, 2 * r), Qt::AlignCenter,
                       state.notifications > 9 ? QStringLiteral("9+") : QString::number(state.notifications));
        }
    }
    // The network: Wi-Fi arcs lit by strength, a wired plug, or the arcs crossed out (none).
    {
        const QPointF c = at(DockItem::Network).center();
        const QColor dim = withAlpha(colours.ink, 70);
        if (state.network == 2) {
            pen(colours.ink);
            p.drawRoundedRect(QRectF(c.x() - 9 * s, c.y() - 15 * s, 18 * s, 15 * s), 3 * s, 3 * s); // the plug
            p.drawLine(QPointF(c.x() - 4 * s, c.y() - 15 * s), QPointF(c.x() - 4 * s, c.y() - 21 * s));
            p.drawLine(QPointF(c.x() + 4 * s, c.y() - 15 * s), QPointF(c.x() + 4 * s, c.y() - 21 * s));
            p.drawLine(QPointF(c.x(), c.y()), QPointF(c.x(), c.y() + 18 * s)); // its cable
        } else {
            const int lit = state.network == 3 ? (state.signal >= 67 ? 3 : state.signal >= 34 ? 2 : 1) : 0;
            const QPointF base(c.x(), c.y() + 12 * s);
            for (int i = 0; i < 3; ++i) {
                const double r = (9 + i * 8) * s;
                pen(i < lit ? colours.ink : dim);
                p.drawArc(QRectF(base.x() - r, base.y() - r, 2 * r, 2 * r), 45 * 16, 90 * 16);
            }
            p.setPen(Qt::NoPen);
            p.setBrush(lit > 0 ? colours.ink : dim);
            p.drawEllipse(base, 3.2 * s, 3.2 * s);
            if (state.network == 1) {
                pen(colours.closeInk);
                p.drawLine(QPointF(c.x() - 15 * s, c.y() - 14 * s), QPointF(c.x() + 15 * s, c.y() + 14 * s));
            }
        }
    }
    // A VPN up: a small lock at the network icon's lower right.
    if (state.vpn) {
        const QPointF c = at(DockItem::Network).center() + QPointF(16 * s, 12 * s);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(60, 140, 255));
        p.drawRoundedRect(QRectF(c.x() - 8 * s, c.y() - 3 * s, 16 * s, 12 * s), 2.5 * s, 2.5 * s);
        p.setPen(QPen(QColor(60, 140, 255), 3.2 * s, Qt::SolidLine, Qt::RoundCap));
        p.setBrush(Qt::NoBrush);
        p.drawArc(QRectF(c.x() - 5 * s, c.y() - 11 * s, 10 * s, 14 * s), 0, 180 * 16);
    }
    // The time, the volume between its buttons.
    QFont font;
    font.setWeight(QFont::DemiBold);
    font.setPixelSize(int(DockLayout::kButton * k * 0.36f));
    p.setFont(font);
    p.setPen(colours.ink);
    p.drawText(at(DockItem::Time), Qt::AlignCenter, state.time);
    const auto speaker = [&](QPointF c, int sign) {
        pen(colours.ink);
        QPainterPath body;
        body.moveTo(c.x() - 16 * s, c.y() - 5 * s);
        body.lineTo(c.x() - 10 * s, c.y() - 5 * s);
        body.lineTo(c.x() - 2 * s, c.y() - 12 * s);
        body.lineTo(c.x() - 2 * s, c.y() + 12 * s);
        body.lineTo(c.x() - 10 * s, c.y() + 5 * s);
        body.lineTo(c.x() - 16 * s, c.y() + 5 * s);
        body.closeSubpath();
        p.drawPath(body);
        p.drawLine(QPointF(c.x() + 6 * s, c.y()), QPointF(c.x() + 18 * s, c.y()));
        if (sign > 0) {
            p.drawLine(QPointF(c.x() + 12 * s, c.y() - 6 * s), QPointF(c.x() + 12 * s, c.y() + 6 * s));
        }
    };
    speaker(at(DockItem::VolumeDown).center(), -1);
    speaker(at(DockItem::VolumeUp).center(), +1);
    font.setPixelSize(int(DockLayout::kButton * k * 0.28f));
    p.setFont(font);
    p.setPen(state.muted ? colours.closeInk : colours.ink);
    p.drawText(at(DockItem::Volume), Qt::AlignCenter,
               state.muted ? QStringLiteral("muted") : state.volume < 0 ? QStringLiteral("--") : QStringLiteral("%1%").arg(state.volume));
    // Thin separators where the layout leaves its gaps: before the network (and the time) and before the volume.
    p.setPen(QPen(withAlpha(colours.ink, 60), 2.0));
    for (DockItem item : {DockItem::Network, DockItem::VolumeDown}) {
        const QRectF r = at(item);
        const float x = r.left() - DockLayout::kSeparator * k / 2.0f;
        p.drawLine(QPointF(x, r.top() + r.height() * 0.2), QPointF(x, r.bottom() - r.height() * 0.2));
    }
    return image;
}

namespace
{

//! A notification's card in @p box: icon, app, summary, body, its action buttons, its close button; @p hovered: 1 the card,
//! 2 the close button, kHoveredAction + i the i-th action button.
void drawNotification(QPainter &p, const QRectF &boxWithActions, const NotificationArt &n, int hovered, bool glass, Theme theme)
{
    using Spatial::NotificationLayout;
    const Palette colours = palette(theme);
    const float k = kPixelsPerUnit;
    const qreal radius = 0.022f * k;
    const int actions = std::min(int(n.actions.size()), NotificationLayout::kMaxActions);
    // The text's part (as a card without actions); the buttons' row under it.
    const QRectF box(boxWithActions.topLeft(), QSizeF(boxWithActions.width(), boxWithActions.height() - (actions > 0 ? NotificationLayout::kActionsHeight * k : 0.0)));
    const QRectF &whole = boxWithActions;
    p.setPen(QPen(colours.rim, 2.0));
    if (glass) {
        p.setBrush(glassBrush(colours, theme, whole, 225, 175));
    } else {
        p.setBrush(withAlpha(colours.ink, theme == Theme::Dark ? 22 : 16)); // a card on the list's glass
    }
    p.drawRoundedRect(whole, radius, radius);
    if (hovered == 1) {
        p.setPen(Qt::NoPen);
        p.setBrush(withAlpha(colours.highlight, 34));
        p.drawRoundedRect(whole, radius, radius);
    }
    // The action buttons: pills along the bottom, the hovered one lit.
    if (actions > 0) {
        const Spatial::FrameRect card{0.0f, 0.0f, NotificationLayout::kCardWidth, NotificationLayout::cardHeight(actions)};
        QFont label;
        label.setWeight(QFont::DemiBold);
        label.setPixelSize(int(0.0145f * k));
        p.setFont(label);
        for (int i = 0; i < actions; ++i) {
            const Spatial::FrameRect r = NotificationLayout::actionOf(card, i, actions);
            const QRectF pill(whole.center().x() + (r.x - r.width / 2.0f) * k, whole.center().y() - (r.y + r.height / 2.0f) * k, r.width * k, r.height * k);
            const bool hot = hovered == kHoveredAction + i;
            p.setPen(Qt::NoPen);
            p.setBrush(withAlpha(hot ? colours.highlight : colours.ink, hot ? 120 : theme == Theme::Dark ? 34 : 26));
            p.drawRoundedRect(pill, pill.height() / 2.0, pill.height() / 2.0);
            p.setPen(colours.ink);
            p.drawText(pill.adjusted(0.006f * k, 0, -0.006f * k, 0), Qt::AlignCenter,
                       QFontMetrics(label).elidedText(n.actions[i], Qt::ElideRight, int(pill.width() - 0.012f * k)));
        }
    }
    if (n.critical) {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(235, 90, 60));
        p.drawRoundedRect(QRectF(box.left() + 0.006f * k, box.top() + box.height() * 0.22, 0.004f * k, box.height() * 0.56), 0.002f * k, 0.002f * k);
    }
    const qreal iconSize = 0.044f * k;
    const QRectF iconBox(box.left() + 0.017f * k, box.center().y() - iconSize / 2.0, iconSize, iconSize);
    if (!n.icon.isNull()) {
        p.drawImage(iconBox, n.icon);
    }
    const QRectF text(iconBox.right() + 0.013f * k, box.top() + 0.009f * k, box.right() - iconBox.right() - 0.055f * k, box.height() - 0.018f * k);
    // A degree or more tall at the dock's 0.76 m.
    QFont small;
    small.setPixelSize(int(0.013f * k));
    QFont title;
    title.setWeight(QFont::DemiBold);
    title.setPixelSize(int(0.017f * k));
    QFont body;
    body.setPixelSize(int(0.0145f * k));
    const qreal smallH = QFontMetricsF(small).height();
    const qreal titleH = QFontMetricsF(title).height();
    p.setPen(withAlpha(colours.ink, 190));
    p.setFont(small);
    p.drawText(QRectF(text.left(), text.top(), text.width(), smallH), Qt::AlignLeft | Qt::AlignVCenter,
               QFontMetrics(small).elidedText(n.app.isEmpty() ? QStringLiteral("Notification") : n.app, Qt::ElideRight, int(text.width())));
    p.setPen(colours.ink);
    p.setFont(title);
    const QString summary = n.summary.isEmpty() ? n.body : n.summary;
    p.drawText(QRectF(text.left(), text.top() + smallH, text.width(), titleH), Qt::AlignLeft | Qt::AlignVCenter,
               QFontMetrics(title).elidedText(summary.simplified(), Qt::ElideRight, int(text.width())));
    if (!n.summary.isEmpty() && !n.body.isEmpty()) {
        p.setPen(withAlpha(colours.ink, 225));
        p.setFont(body);
        p.drawText(QRectF(text.left(), text.top() + smallH + titleH, text.width(), text.bottom() - text.top() - smallH - titleH),
                   Qt::AlignLeft | Qt::AlignTop, QFontMetrics(body).elidedText(n.body.simplified(), Qt::ElideRight, int(text.width())));
    }
    // The close button.
    using Spatial::NotificationLayout;
    const qreal size = NotificationLayout::kClose * k;
    const QPointF bc(box.right() - NotificationLayout::kClose * 0.95f * k, box.top() + NotificationLayout::kClose * 0.85f * k);
    p.setPen(Qt::NoPen);
    p.setBrush(hovered == 2 ? withAlpha(colours.closeInk, 230) : withAlpha(colours.ink, 36));
    p.drawEllipse(bc, size / 2.0 * 0.82, size / 2.0 * 0.82);
    p.setPen(QPen(hovered == 2 ? QColor(255, 255, 255) : colours.ink, 0.0028f * k, Qt::SolidLine, Qt::RoundCap));
    const qreal arm = size * 0.16;
    p.drawLine(bc + QPointF(-arm, -arm), bc + QPointF(arm, arm));
    p.drawLine(bc + QPointF(arm, -arm), bc + QPointF(-arm, arm));
}

} // namespace

QImage notificationCard(const NotificationArt &notification, int hovered, Theme theme)
{
    using Spatial::NotificationLayout;
    QImage image = canvas(px(NotificationLayout::kCardWidth), px(NotificationLayout::cardHeight(int(notification.actions.size()))));
    QPainter p(&image);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);
    drawNotification(p, QRectF(1, 1, image.width() - 2, image.height() - 2), notification, hovered, true, theme);
    return image;
}

QImage notificationList(const QList<NotificationArt> &list, int count, int hoveredRow, int hoveredPart, Theme theme)
{
    using Spatial::NotificationHit;
    using Spatial::NotificationLayout;
    const float k = kPixelsPerUnit;
    const Palette colours = palette(theme);
    std::vector<int> actions;
    for (const NotificationArt &n : list) {
        actions.push_back(std::min(int(n.actions.size()), NotificationLayout::kMaxActions));
    }
    QImage image = canvas(px(NotificationLayout::listWidth()), px(NotificationLayout::listHeight(count, actions)));
    QPainter p(&image);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);
    const QRectF panel(1, 1, image.width() - 2, image.height() - 2);
    p.setPen(QPen(colours.rim, 2.0));
    p.setBrush(glassBrush(colours, theme, panel, 232, 176));
    p.drawRoundedRect(panel, 0.03f * k, 0.03f * k);
    const QPointF centre(image.width() / 2.0, image.height() / 2.0);
    const auto at = [&](const Spatial::FrameRect &r) {
        return QRectF(centre.x() + (r.x - r.width / 2.0f) * k, centre.y() - (r.y + r.height / 2.0f) * k, r.width * k, r.height * k);
    };
    // The title, and "Clear all" when there is something to clear.
    QFont title;
    title.setWeight(QFont::DemiBold);
    title.setPixelSize(int(0.018f * k));
    p.setFont(title);
    p.setPen(colours.ink);
    const Spatial::FrameRect clear = NotificationLayout::clearAll(count, actions);
    const QRectF header(centre.x() - NotificationLayout::kCardWidth / 2.0f * k + 0.006f * k, at(clear).top(), NotificationLayout::kCardWidth * k * 0.6, at(clear).height());
    p.drawText(header, Qt::AlignLeft | Qt::AlignVCenter, count > NotificationLayout::kMaxRows ? QStringLiteral("Notifications (%1)").arg(count) : QStringLiteral("Notifications"));
    if (count > 0) {
        const QRectF pill = at(clear);
        const bool hot = hoveredPart == int(NotificationHit::Part::ClearAll);
        p.setPen(Qt::NoPen);
        p.setBrush(withAlpha(colours.highlight, hot ? 70 : 30));
        p.drawRoundedRect(pill, pill.height() / 2.0, pill.height() / 2.0);
        QFont small;
        small.setWeight(QFont::DemiBold);
        small.setPixelSize(int(0.0135f * k));
        p.setFont(small);
        p.setPen(colours.ink);
        p.drawText(pill, Qt::AlignCenter, QStringLiteral("Clear all"));
    }
    if (list.isEmpty()) {
        QFont none;
        none.setPixelSize(int(0.015f * k));
        p.setFont(none);
        p.setPen(withAlpha(colours.ink, 185));
        p.drawText(at(NotificationLayout::listCard(0, 0)), Qt::AlignCenter, QStringLiteral("No notifications"));
        return image;
    }
    for (int row = 0; row < std::min(int(list.size()), NotificationLayout::kMaxRows); ++row) {
        const int hovered = row != hoveredRow ? 0
            : hoveredPart == int(NotificationHit::Part::Card) ? 1
            : hoveredPart == int(NotificationHit::Part::Close) ? 2
            : hoveredPart >= kHoveredAction ? hoveredPart : 0;
        drawNotification(p, at(NotificationLayout::listCard(row, count, actions)), list[row], hovered, false, theme);
    }
    return image;
}

QImage dragBadge(Theme theme)
{
    const Palette colours = palette(theme);
    const int size = 128;
    QImage image = canvas(size, size);
    QPainter p(&image);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF tile(4, 4, size - 8, size - 8);
    p.setPen(QPen(colours.rim, 2.0));
    p.setBrush(glassBrush(colours, theme, tile, 230, 200));
    p.drawRoundedRect(tile, 26, 26);
    // A page with its corner folded.
    QPainterPath page;
    page.moveTo(40, 26);
    page.lineTo(76, 26);
    page.lineTo(92, 42);
    page.lineTo(92, 100);
    page.lineTo(40, 100);
    page.closeSubpath();
    p.setPen(QPen(colours.ink, 5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(withAlpha(colours.ink, 40));
    p.drawPath(page);
    p.drawPolyline(QPolygonF{QPointF(76, 26), QPointF(76, 42), QPointF(92, 42)});
    // A blue dot with a plus: it is being carried.
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(60, 140, 255));
    p.drawEllipse(QPointF(92, 96), 18, 18);
    p.setPen(QPen(Qt::white, 5, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(QPointF(92, 87), QPointF(92, 105));
    p.drawLine(QPointF(83, 96), QPointF(101, 96));
    return image;
}

QImage keyHighlight(bool pressed, Theme theme)
{
    QImage image = canvas(64, 64);
    QPainter p(&image);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(withAlpha(palette(theme).highlight, pressed ? 150 : 60));
    p.drawRoundedRect(QRectF(1, 1, 62, 62), 10, 10);
    return image;
}

QImage label(const QString &text, Theme theme)
{
    const Palette colours = palette(theme);
    QImage image = canvas(px(kLabelWidth), px(kLabelHeight));
    QPainter p(&image);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);
    QFont font;
    font.setPixelSize(int(image.height() * 0.50)); // about a degree tall at any distance (the frame's unit follows it)
    font.setWeight(QFont::DemiBold);
    p.setFont(font);
    const QFontMetricsF metrics(font);
    const qreal pad = image.height() * 0.40;
    const qreal width = std::min<qreal>(image.width() - 2, metrics.horizontalAdvance(text) + 2 * pad);
    const QRectF pill(image.width() - 1 - width, 1, width, image.height() - 2);
    p.setPen(QPen(colours.rim, 2.0));
    p.setBrush(glassBrush(colours, theme, pill, 190, 160));
    p.drawRoundedRect(pill, pill.height() / 2.0, pill.height() / 2.0);
    p.setPen(colours.ink);
    p.drawText(pill, Qt::AlignCenter, text);
    return image;
}

QImage lensTest(float pixelsPerRadian, int spacingMm, bool cross)
{
    // At the panel's density (a pixel here is a panel pixel at the lens centre). Positions in degrees from the middle
    // (up negative) at their place on the flat picture; sizes as degrees there.
    const auto at = [pixelsPerRadian](float degrees) {
        return qreal(pixelsPerRadian * std::tan(degrees * float(M_PI) / 180.0f));
    };
    const int side = 2 * int(std::lround(at(kLensTestDegrees / 2.0f))) + 1;
    QImage image(side, side, QImage::Format_RGBA8888_Premultiplied);
    image.fill(Qt::black);
    const int c = side / 2; //!< the middle pixel, on the lens's axis
    QPainter p(&image);

    // The cross, unsmoothed, laid out as Oculus' (81 panel pixels across, pure green, four lines a bar about a dark
    // middle, the horizontal ones stopping short of the vertical bar, so they never cross), its lines a pixel wide
    // but 4 apart, not 2: Oculus draws onto the panel's pixels, this picture reaches them through the runtime's two
    // resamplings (lines a pixel apart there merge or double whatever the fit). Blurred, a bar's lines run together;
    // the vertical ones tell the lens spacing and depth, the horizontal ones the height on the face.
    if (cross) {
        const QColor green(0, 255, 0);
        constexpr int arm = 39;
        for (const int offset : {-6, -2, 2, 6}) {
            p.fillRect(c + offset, c - arm, 1, 2 * arm + 1, green);
            p.fillRect(c - arm, c + offset, arm - 7, 1, green);
            p.fillRect(c + 8, c + offset, arm - 7, 1, green);
        }
    }

    // The words, white and centred: above the cross the instructions (their last line 6 degrees over its middle) and
    // the title over them, however many lines they take; below it the lens spacing and the button's frame.
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);
    const QColor ink(242, 242, 242);
    const auto fontOf = [&](float degrees, QFont::Weight weight) {
        QFont font;
        font.setPixelSize(std::max(8, int(std::lround(at(degrees)))));
        font.setWeight(weight);
        return font;
    };
    const auto draw = [&](const QRectF &area, int flags, const QString &text, const QFont &font) {
        p.setFont(font);
        p.setPen(ink);
        p.drawText(area, flags, text);
    };
    const int wrapped = Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap;
    const QFont title = fontOf(2.0f, QFont::DemiBold);
    const QFont body = fontOf(1.3f, QFont::Normal);
    const QString instructions = QStringLiteral("Move the headset up and down on your face until the horizontal lines are clear. Push in and "
                                                "shift the lens slider until the vertical lines are clear.");
    const QRectF column(c - at(16.0f), 0.0, 2.0 * at(16.0f), side);
    const qreal instructionsHeight = QFontMetricsF(body).boundingRect(column, wrapped, instructions).height();
    const qreal instructionsTop = c - at(6.0f) - instructionsHeight;
    const qreal titleHeight = QFontMetricsF(title).height();
    draw(QRectF(c - at(18.0f), instructionsTop - at(0.8f) - titleHeight, 2.0 * at(18.0f), titleHeight), Qt::AlignCenter,
         QStringLiteral("Adjust Your Lenses"), title);
    draw(column.adjusted(0.0, instructionsTop, 0.0, 0.0), wrapped, instructions, body);
    if (spacingMm > 0) {
        draw(QRectF(c - at(14.0f), c + at(4.5f), 2.0 * at(14.0f), at(7.0f) - at(4.5f)), Qt::AlignCenter,
             QStringLiteral("Lens spacing: %1 mm").arg(spacingMm), fontOf(1.4f, QFont::Normal));
    }
    const QRectF button(c - at(13.0f), c + at(8.5f), 2.0 * at(13.0f), at(11.5f) - at(8.5f));
    p.setPen(QPen(QColor(150, 150, 150), std::max(1.0, at(0.12f))));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(button, at(0.3f), at(0.3f));
    draw(button, Qt::AlignCenter, QStringLiteral("Press any button to continue."), body);
    return image;
}

} // namespace RiftHome::Xr::FrameArt
