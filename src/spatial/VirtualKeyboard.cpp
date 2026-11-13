// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
#include "spatial/VirtualKeyboard.hpp"

#include <linux/input-event-codes.h>

#include <algorithm>
#include <cmath>

namespace RiftHome::Spatial
{

namespace
{

using Kind = KeyDef::Kind;

KeyDef key(const char *label, const char *shiftLabel, uint32_t code, float width = 1.0f)
{
    return {QString::fromUtf8(label), QString::fromUtf8(shiftLabel), code, width, Kind::Key, false};
}

KeyDef letter(char c, uint32_t code)
{
    return {QString(QLatin1Char(c)), QString(QLatin1Char(char(c - 'a' + 'A'))), code, 1.0f, Kind::Key, true};
}

KeyDef special(const char *label, uint32_t code, float width, Kind kind = Kind::Key)
{
    return {QString::fromUtf8(label), {}, code, width, kind, false};
}

} // namespace

VirtualKeyboard::VirtualKeyboard()
{
    // Five rows of 14.5 pitches: a US layout without function keys, Hide and arrows in the bottom row.
    const std::vector<std::vector<KeyDef>> rows = {
        {special("Esc", KEY_ESC, 1.0f), key("1", "!", KEY_1), key("2", "@", KEY_2), key("3", "#", KEY_3), key("4", "$", KEY_4),
         key("5", "%", KEY_5), key("6", "^", KEY_6), key("7", "&", KEY_7), key("8", "*", KEY_8), key("9", "(", KEY_9),
         key("0", ")", KEY_0), key("-", "_", KEY_MINUS), key("=", "+", KEY_EQUAL), special("⌫", KEY_BACKSPACE, 1.5f)},
        {special("Tab", KEY_TAB, 1.5f), letter('q', KEY_Q), letter('w', KEY_W), letter('e', KEY_E), letter('r', KEY_R),
         letter('t', KEY_T), letter('y', KEY_Y), letter('u', KEY_U), letter('i', KEY_I), letter('o', KEY_O), letter('p', KEY_P),
         key("[", "{", KEY_LEFTBRACE), key("]", "}", KEY_RIGHTBRACE), key("\\", "|", KEY_BACKSLASH)},
        {special("Ctrl", 0, 1.75f, Kind::Ctrl), letter('a', KEY_A), letter('s', KEY_S), letter('d', KEY_D), letter('f', KEY_F),
         letter('g', KEY_G), letter('h', KEY_H), letter('j', KEY_J), letter('k', KEY_K), letter('l', KEY_L),
         key(";", ":", KEY_SEMICOLON), key("'", "\"", KEY_APOSTROPHE), special("Enter", KEY_ENTER, 1.75f)},
        {special("Shift", 0, 2.25f, Kind::Shift), letter('z', KEY_Z), letter('x', KEY_X), letter('c', KEY_C), letter('v', KEY_V),
         letter('b', KEY_B), letter('n', KEY_N), letter('m', KEY_M), key(",", "<", KEY_COMMA), key(".", ">", KEY_DOT),
         key("/", "?", KEY_SLASH), special("Shift", 0, 2.25f, Kind::Shift)},
        {special("Hide", 0, 1.5f, Kind::Hide), special("Alt", 0, 1.25f, Kind::Alt), special("", KEY_SPACE, 6.75f),
         special("←", KEY_LEFT, 1.25f), special("↓", KEY_DOWN, 1.25f), special("↑", KEY_UP, 1.25f), special("→", KEY_RIGHT, 1.25f)},
    };
    const float rowsWidth = 14.5f * kPitch;
    m_width = rowsWidth + 2.0f * kPadding;
    m_height = float(rows.size()) * kPitch + 2.0f * kPadding;
    const float gap = 0.004f; // between neighbouring keys
    for (size_t r = 0; r < rows.size(); ++r) {
        float x = -rowsWidth / 2.0f;
        const float y = m_height / 2.0f - kPadding - (float(r) + 0.5f) * kPitch;
        for (const KeyDef &k : rows[r]) {
            const float w = k.width * kPitch;
            m_keys.push_back(k);
            m_rects.push_back({x + w / 2.0f, y, w - gap, kPitch - gap});
            x += w;
        }
    }
}

bool VirtualKeyboard::contains(float x, float y) const
{
    return std::fabs(x) <= m_width / 2.0f && std::fabs(y) <= m_height / 2.0f;
}

int VirtualKeyboard::hit(float x, float y) const
{
    for (size_t i = 0; i < m_rects.size(); ++i) {
        const KeyRect &r = m_rects[i];
        // The whole pitch counts (the gaps between keys too): no dead spots between keys.
        if (std::fabs(x - r.x) <= (r.width + 0.004f) / 2.0f && std::fabs(y - r.y) <= kPitch / 2.0f) {
            return int(i);
        }
    }
    return -1;
}

QString VirtualKeyboard::label(int index) const
{
    const KeyDef &k = m_keys.at(size_t(index));
    const bool shifted = k.letter ? (m_shift != m_capsLock) : m_shift;
    return shifted && !k.shiftLabel.isEmpty() ? k.shiftLabel : k.label;
}

std::vector<KeyEvent> VirtualKeyboard::press(int index)
{
    const KeyDef &k = m_keys.at(size_t(index));
    switch (k.kind) {
    case Kind::Shift:
        // One-shot, then caps lock, then off.
        if (m_capsLock) {
            m_capsLock = false;
            m_shift = false;
        } else if (m_shift) {
            m_shift = false;
            m_capsLock = true;
        } else {
            m_shift = true;
        }
        return {};
    case Kind::Ctrl:
        m_ctrl = !m_ctrl;
        return {};
    case Kind::Alt:
        m_alt = !m_alt;
        return {};
    case Kind::Hide:
        m_hide = true;
        return {};
    case Kind::Key:
        break;
    }
    m_heldModifiers.clear();
    if (k.letter ? (m_shift != m_capsLock) : m_shift) {
        m_heldModifiers.push_back(KEY_LEFTSHIFT);
    }
    if (m_ctrl) {
        m_heldModifiers.push_back(KEY_LEFTCTRL);
    }
    if (m_alt) {
        m_heldModifiers.push_back(KEY_LEFTALT);
    }
    std::vector<KeyEvent> events;
    for (uint32_t modifier : m_heldModifiers) {
        events.push_back({modifier, true});
    }
    events.push_back({k.code, true});
    return events;
}

std::vector<KeyEvent> VirtualKeyboard::release(int index)
{
    const KeyDef &k = m_keys.at(size_t(index));
    if (k.kind != Kind::Key) {
        return {};
    }
    std::vector<KeyEvent> events{{k.code, false}};
    for (auto it = m_heldModifiers.rbegin(); it != m_heldModifiers.rend(); ++it) {
        events.push_back({*it, false});
    }
    m_heldModifiers.clear();
    // One-shot modifiers end with the key they were for.
    m_shift = false;
    m_ctrl = false;
    m_alt = false;
    return events;
}

bool VirtualKeyboard::takeHide()
{
    const bool hide = m_hide;
    m_hide = false;
    return hide;
}

std::optional<std::pair<uint32_t, bool>> VirtualKeyboard::keyForChar(QChar c) const
{
    if (c == QLatin1Char(' ')) {
        return std::pair{uint32_t(KEY_SPACE), false};
    }
    if (c == QLatin1Char('\n')) {
        return std::pair{uint32_t(KEY_ENTER), false};
    }
    if (c == QLatin1Char('\t')) {
        return std::pair{uint32_t(KEY_TAB), false};
    }
    for (const KeyDef &k : m_keys) {
        if (k.kind != Kind::Key || k.code == KEY_SPACE || k.label.size() != 1) {
            continue;
        }
        if (k.label.at(0) == c) {
            return std::pair{k.code, false};
        }
        if (!k.shiftLabel.isEmpty() && k.shiftLabel.at(0) == c) {
            return std::pair{k.code, true};
        }
    }
    return std::nullopt;
}

} // namespace RiftHome::Spatial
