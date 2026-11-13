// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
#include "wayland/StatusPage.hpp"

#include "wayland/wlr.hpp"

#include <QFont>
#include <QFontMetrics>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>

namespace RiftHome::Wayland
{

namespace
{

//! The page's unit: its height's share, so it reads the same on any monitor.
float unit(int height)
{
    return height / 1080.0f;
}

QRectF button(int width, int height, int index)
{
    const float u = unit(height);
    const float w = 360 * u;
    const float h = 92 * u;
    const float gap = 40 * u;
    const float x = width / 2.0f - w - gap / 2.0f + index * (w + gap);
    return {x, height * 0.68, w, h};
}

void row(QPainter &p, float u, float x, float y, float w, const QString &label, const QString &value, bool ok)
{
    QFont labelFont;
    labelFont.setPixelSize(int(30 * u));
    QFont valueFont;
    valueFont.setPixelSize(int(30 * u));
    valueFont.setWeight(QFont::DemiBold);
    p.setPen(QColor(255, 255, 255, 150));
    p.setFont(labelFont);
    p.drawText(QRectF(x, y, w * 0.3, 50 * u), Qt::AlignLeft | Qt::AlignVCenter, label);
    p.setPen(Qt::NoPen);
    p.setBrush(ok ? QColor(80, 210, 130) : QColor(240, 170, 60));
    p.drawEllipse(QPointF(x + w * 0.32, y + 25 * u), 8 * u, 8 * u);
    p.setPen(QColor(255, 255, 255, 235));
    p.setFont(valueFont);
    const QRectF text(x + w * 0.32 + 24 * u, y, w - w * 0.32 - 24 * u, 50 * u);
    p.drawText(text, Qt::AlignLeft | Qt::AlignVCenter, QFontMetrics(valueFont).elidedText(value, Qt::ElideRight, int(text.width())));
}

struct ImageBuffer {
    wlr_buffer base;
    QImage image;
};

void imageDestroy(wlr_buffer *buffer)
{
    ImageBuffer *b = wl_container_of(buffer, b, base);
    wlr_buffer_finish(&b->base); // its destroy event and addons (wlroots' textures of it), as every buffer must
    delete b;
}

bool imageBeginAccess(wlr_buffer *buffer, uint32_t flags, void **data, uint32_t *format, size_t *stride)
{
    ImageBuffer *b = wl_container_of(buffer, b, base);
    if (flags & WLR_BUFFER_DATA_PTR_ACCESS_WRITE) {
        return false;
    }
    *data = const_cast<uchar *>(b->image.constBits());
    *format = DRM_FORMAT_ABGR8888; // RGBA8888 in memory
    *stride = size_t(b->image.bytesPerLine());
    return true;
}

void imageEndAccess(wlr_buffer *)
{
}

const wlr_buffer_impl kImageBufferImpl = {
    .destroy = imageDestroy,
    .get_dmabuf = nullptr,
    .get_shm = nullptr,
    .begin_data_ptr_access = imageBeginAccess,
    .end_data_ptr_access = imageEndAccess,
};

} // namespace

QRectF leaveButton(int width, int height)
{
    return button(width, height, 0);
}

QRectF terminalButton(int width, int height)
{
    return button(width, height, 1);
}

QImage drawStatusPage(const StatusState &state, int width, int height)
{
    QImage image(width, height, QImage::Format_RGBA8888_Premultiplied);
    QPainter p(&image);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);
    const float u = unit(height);
    // A deep blue evening, as Rift Home's icon.
    QLinearGradient sky(0, 0, 0, height);
    sky.setColorAt(0.0, QColor(30, 40, 72));
    sky.setColorAt(1.0, QColor(10, 13, 26));
    p.fillRect(image.rect(), sky);
    // The title and what is going on.
    QFont title;
    title.setPixelSize(int(84 * u));
    title.setWeight(QFont::DemiBold);
    p.setFont(title);
    p.setPen(QColor(244, 246, 251));
    p.drawText(QRectF(0, height * 0.13, width, 110 * u), Qt::AlignCenter, QStringLiteral("Rift Home"));
    QFont subtitle;
    subtitle.setPixelSize(int(34 * u));
    p.setFont(subtitle);
    p.setPen(QColor(255, 255, 255, 170));
    p.drawText(QRectF(0, height * 0.13 + 110 * u, width, 50 * u), Qt::AlignCenter,
               QStringLiteral("is running in the headset. Put it on to use your apps."));
    // The status card.
    const float cardW = 760 * u;
    const float cardX = (width - cardW) / 2.0f;
    const float cardY = height * 0.36;
    const QRectF card(cardX, cardY, cardW, 230 * u);
    p.setPen(QPen(QColor(255, 255, 255, 40), 2 * u));
    p.setBrush(QColor(255, 255, 255, 18));
    p.drawRoundedRect(card, 28 * u, 28 * u);
    const float inner = cardX + 50 * u;
    const float innerW = cardW - 100 * u;
    row(p, u, inner, cardY + 22 * u, innerW, QStringLiteral("Headset"), state.headset, state.headsetOk);
    row(p, u, inner, cardY + 22 * u + 64 * u, innerW, QStringLiteral("Runtime"), state.runtime, state.runtimeOk);
    row(p, u, inner, cardY + 22 * u + 128 * u, innerW, QStringLiteral("Windows"),
        state.windows == 1 ? QStringLiteral("1 in the headset") : QStringLiteral("%1 in the headset").arg(state.windows), true);
    // The buttons.
    QFont label;
    label.setPixelSize(int(32 * u));
    label.setWeight(QFont::DemiBold);
    p.setFont(label);
    const QRectF leave = leaveButton(width, height);
    p.setPen(Qt::NoPen);
    p.setBrush(state.confirmLeave ? QColor(225, 70, 60) : QColor(225, 70, 60, 70));
    p.drawRoundedRect(leave, leave.height() / 2, leave.height() / 2);
    p.setPen(state.confirmLeave ? QColor(255, 255, 255) : QColor(255, 150, 140));
    p.drawText(leave, Qt::AlignCenter, state.confirmLeave ? QStringLiteral("Click again to leave") : QStringLiteral("Leave Rift Home"));
    const QRectF terminal = terminalButton(width, height);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(255, 255, 255, 34));
    p.drawRoundedRect(terminal, terminal.height() / 2, terminal.height() / 2);
    // A terminal glyph: a prompt and a line.
    const QPointF g(terminal.left() + 70 * u, terminal.center().y());
    p.setPen(QPen(QColor(255, 255, 255, 230), 5 * u, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.drawPolyline(QPolygonF{g + QPointF(-12 * u, -12 * u), g + QPointF(0, 0), g + QPointF(-12 * u, 12 * u)});
    p.drawLine(g + QPointF(8 * u, 14 * u), g + QPointF(26 * u, 14 * u));
    p.setPen(QColor(255, 255, 255, 235));
    p.drawText(terminal.adjusted(60 * u, 0, 0, 0), Qt::AlignCenter, QStringLiteral("Terminal"));
    // The time, and the keys.
    QFont clock;
    clock.setPixelSize(int(40 * u));
    p.setFont(clock);
    p.setPen(QColor(255, 255, 255, 200));
    p.drawText(QRectF(0, height * 0.86, width, 50 * u), Qt::AlignCenter, state.time);
    QFont small;
    small.setPixelSize(int(24 * u));
    p.setFont(small);
    p.setPen(QColor(255, 255, 255, 110));
    p.drawText(QRectF(0, height * 0.86 + 56 * u, width, 40 * u), Qt::AlignCenter,
               QStringLiteral("Ctrl+Alt+Backspace twice: leave  ·  Ctrl+Alt+F1-F12: other consoles"));
    return image;
}

wlr_buffer *bufferFromImage(const QImage &image)
{
    auto *b = new ImageBuffer;
    b->image = image.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    wlr_buffer_init(&b->base, &kImageBufferImpl, b->image.width(), b->image.height());
    return &b->base;
}

} // namespace RiftHome::Wayland
