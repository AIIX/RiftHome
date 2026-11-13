// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QString>

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace RiftHome::Spatial
{

/*!
 * Rift Home's virtual keyboard: a US layout drawn in the room and typed on with the controllers' rays.
 * Pure: geometry, hit test, labels, the one-shot modifiers, and the key events each key sends (linux evdev codes, as
 * the seat's virtual keyboard device takes them).
 */
struct KeyDef {
    enum class Kind {
        Key, //!< sends its code (letters, digits, Enter, arrows, ...)
        Shift,
        Ctrl,
        Alt,
        Hide, //!< closes the keyboard
    };
    QString label;
    QString shiftLabel; //!< empty: the same as label
    uint32_t code = 0;
    float width = 1.0f; //!< in key pitches
    Kind kind = Kind::Key;
    bool letter = false; //!< caps lock shifts it
};

//! A key's place: centre and size in metres, keyboard-local (origin at the keyboard's centre, x right, y up).
struct KeyRect {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
};

struct KeyEvent {
    uint32_t code = 0;
    bool pressed = false;
    bool operator==(const KeyEvent &) const = default;
};

class VirtualKeyboard
{
public:
    static constexpr float kPitch = 0.05f; //!< metres from one key to the next (about 4 degrees at 0.65 m)
    static constexpr float kPadding = 0.012f; //!< around the keys

    VirtualKeyboard();

    const std::vector<KeyDef> &keys() const
    {
        return m_keys;
    }
    const std::vector<KeyRect> &rects() const
    {
        return m_rects;
    }
    float width() const
    {
        return m_width;
    }
    float height() const
    {
        return m_height;
    }
    //! The key at a keyboard-local point, or -1 (on the keyboard between keys, or off it).
    int hit(float x, float y) const;
    //! The keyboard (with its padding) contains the point: it is solid there.
    bool contains(float x, float y) const;

    // The modifiers: Shift, Ctrl and Alt are one-shot (tap, then the key); Shift twice = caps lock, a third time off.
    bool shift() const
    {
        return m_shift;
    }
    bool capsLock() const
    {
        return m_capsLock;
    }
    bool ctrl() const
    {
        return m_ctrl;
    }
    bool alt() const
    {
        return m_alt;
    }
    //! What the key shows now.
    QString label(int index) const;

    //! The key was pressed: the key events to send now (modifiers first). Hide and modifier keys send nothing.
    std::vector<KeyEvent> press(int index);
    //! The key was released: its key events (the key, then the modifiers it used); one-shot modifiers end.
    std::vector<KeyEvent> release(int index);
    //! Hide was pressed (and cleared).
    bool takeHide();

    //! The key and whether Shift is needed for a character (headless typing, tests); nothing for unknown ones.
    std::optional<std::pair<uint32_t, bool>> keyForChar(QChar c) const;

private:
    std::vector<KeyDef> m_keys;
    std::vector<KeyRect> m_rects;
    float m_width = 0.0f;
    float m_height = 0.0f;
    bool m_shift = false;
    bool m_capsLock = false;
    bool m_ctrl = false;
    bool m_alt = false;
    bool m_hide = false;
    int m_shiftTaps = 0;
    std::vector<uint32_t> m_heldModifiers; //!< sent down with the key now held, released with it
};

} // namespace RiftHome::Spatial
