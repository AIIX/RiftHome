// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QJsonObject>
#include <QString>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace RiftHome::Spatial
{

/*!
 * What the Touch controllers' buttons do outside game mode, the user's own choice in Settings: each action has a list
 * of bindings (a button, or two together), and goes back to its defaults. The triggers (click) never take part: they are
 * how Settings itself is used. The right Oculus button is the runtime's; game mode keeps its gamepad.
 */
enum class ButtonAction {
    Home, //!< Home opens or closes
    Dock, //!< the dock shows or hides
    LeftClick, //!< held: as the trigger pulled (a click on a window, a press on the dock, Home, the keyboard)
    RightClick, //!< held: the right mouse button on the window the hand points at
    MiddleClick, //!< held: the middle mouse button
    Keyboard, //!< the virtual keyboard shows or hides
    Screenshot, //!< a screenshot of the room (Pictures/RiftHome/screenshots)
    Recline, //!< the arrangement turns to where the user looks (lying back), or upright again
    FollowAll, //!< the dock's Follow all
    GameMode, //!< the controllers as a gamepad, or back
    Settings, //!< Settings opens or closes
    Notifications, //!< the list of notifications shows or hides
    VolumeUp,
    VolumeDown,
    Mute,
};
constexpr std::size_t kButtonActions = 15;

//! The inputs a binding is made of, named by slot so other controllers can map onto them later.
enum class ControllerInput {
    LeftPrimary, //!< X
    LeftSecondary, //!< Y
    LeftMenu, //!< the menu button (three lines)
    LeftStick, //!< the left thumbstick pressed
    LeftGrip, //!< only in a chord: alone the grip grabs
    RightPrimary, //!< A
    RightSecondary, //!< B
    RightStick,
    RightGrip,
};
constexpr std::size_t kControllerInputs = 9;

//! The inputs that are down, one bit each (ControllerInput order).
using InputMask = std::uint16_t;
constexpr InputMask inputBit(ControllerInput input)
{
    return InputMask(1u << unsigned(input));
}

//! The actions, one bit each (ButtonAction order).
using ActionMask = std::uint32_t;
constexpr ActionMask actionBit(ButtonAction action)
{
    return ActionMask(1u << unsigned(action));
}

/*!
 * One button, or two together (a chord: either is held and the other pressed, in any order). Two inputs are stored in
 * the one canonical order (grips first), so the same pair is always the same binding.
 */
struct Binding {
    ControllerInput first{};
    std::optional<ControllerInput> second; //!< a chord when set

    //! A button alone, or a pair (@p b may be empty); false (and a chord of one thing, or a grip alone) is not a binding.
    static std::optional<Binding> make(ControllerInput a, std::optional<ControllerInput> b = std::nullopt);
    //! "leftSecondary", "leftGrip+rightSecondary" (either order); nothing for an unknown or invalid one.
    static std::optional<Binding> fromName(const QString &name);
    QString name() const; //!< the settings' name
    QString label() const; //!< as Settings shows it: "Y", "Left grip + B"
    bool isChord() const
    {
        return second.has_value();
    }
    bool contains(ControllerInput input) const
    {
        return first == input || second == input;
    }
    InputMask mask() const;
    bool operator==(const Binding &) const = default;
};

/*!
 * Action -> bindings, as in KDE's Shortcuts page: each action has zero or more bindings and a binding belongs to one
 * action. settings.json's "controls": {"version": 2, "bindings": {"home": ["leftSecondary", "leftMenu"], ...}}, only the
 * actions that differ from the defaults (one listed with [] has none). The first format (button -> action, no version) is
 * read and converted.
 */
class ButtonMap
{
public:
    //! Home: Y and the menu button. Dock: X. Right click: A and the thumbstick presses. Recline: a grip held and B.
    static ButtonMap defaults();
    //! From the settings' "controls" group (either format); a missing or unknown one: its default.
    static ButtonMap fromJson(const QJsonObject &controls);
    //! The group as saved: the version and the actions that differ from the defaults.
    QJsonObject toJson() const;

    const std::vector<Binding> &bindings(ButtonAction action) const
    {
        return m_bindings[std::size_t(action)];
    }
    //! Replaces @p action's bindings (a binding another action had is taken from it).
    void setBindings(ButtonAction action, const std::vector<Binding> &bindings);
    //! Adds @p binding to @p action; the action that had it loses it and is returned.
    std::optional<ButtonAction> assign(ButtonAction action, const Binding &binding);
    void clear(ButtonAction action)
    {
        m_bindings[std::size_t(action)].clear();
    }
    //! The action that has @p binding.
    std::optional<ButtonAction> actionOf(const Binding &binding) const;
    //! Whether @p action has its defaults.
    bool isDefault(ButtonAction action) const;
    /*!
     * What takes effect: these bindings, except that when none leads to Home or the dock the menu button opens Home (so
     * Home, and with it Settings, can always be reached).
     */
    ButtonMap effective() const;
    //! The same bindings for every action (their order does not matter).
    bool operator==(const ButtonMap &other) const;

    //! The settings' names: "home", "dock", "rightClick", ...; "leftPrimary", ...
    static QString name(ButtonAction action);
    static QString label(ButtonAction action);
    static bool actionFromName(const QString &name, ButtonAction *action);
    static QString inputName(ControllerInput input);
    static QString inputLabel(ControllerInput input);
    static bool inputFromName(const QString &name, ControllerInput *input);
    //! Whether @p action is held while its binding is (the three mouse buttons), else it fires once.
    static bool isHeld(ButtonAction action)
    {
        return action == ButtonAction::LeftClick || action == ButtonAction::RightClick || action == ButtonAction::MiddleClick;
    }

private:
    std::array<std::vector<Binding>, kButtonActions> m_bindings;
};

/*!
 * The firing rules, fed each frame with the inputs that are down (pure, no time). A chord fires when its second input
 * goes down while the first is held; that press, and the release of the first, do nothing else. A button alone fires on
 * press, unless it is also part of an assigned chord: then on release, and only if no chord happened while it was held.
 * The three clicks are held while their binding is, a chord's while both are down.
 */
class BindingTracker
{
public:
    struct Result {
        ActionMask pressed = 0; //!< fired this frame (not the clicks)
        ActionMask held = 0; //!< the clicks held now
        //! The bindings that hold them (a click may be held by several): which hand's click it is, whether a thumbstick's.
        std::vector<std::pair<ButtonAction, Binding>> heldBy;
        bool isPressed(ButtonAction action) const
        {
            return pressed & actionBit(action);
        }
        bool isHeld(ButtonAction action) const
        {
            return held & actionBit(action);
        }
    };
    //! One frame: @p map is the effective one.
    Result update(const ButtonMap &map, InputMask down);
    //! Forgets what was down (a mode change: nothing fires on the way out).
    void reset()
    {
        m_down = m_chorded = 0;
    }

private:
    InputMask m_down = 0;
    InputMask m_chorded = 0; //!< inputs that took part in a chord since they went down
};

/*!
 * Recording a binding by pressing it: fed each frame with the recordable inputs down. It ends when all are released,
 * with the one or two pressed (the first two when more); a grip alone is no binding and the recording goes on.
 */
class BindingRecorder
{
public:
    //! The binding once everything is released; nothing before.
    std::optional<Binding> update(InputMask down);
    void reset()
    {
        m_down = 0;
        m_order.clear();
    }

private:
    InputMask m_down = 0;
    std::vector<ControllerInput> m_order; //!< in the order pressed
};

} // namespace RiftHome::Spatial
