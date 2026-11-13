// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
#include "spatial/Controls.hpp"

#include <QJsonArray>

#include <algorithm>

namespace RiftHome::Spatial
{

namespace
{

constexpr std::array<const char *, kControllerInputs> kInputNames{"leftPrimary", "leftSecondary", "leftMenu", "leftStick", "leftGrip",
                                                                 "rightPrimary", "rightSecondary", "rightStick", "rightGrip"};
constexpr std::array<const char *, kControllerInputs> kInputLabels{"X", "Y", "☰", "Left stick", "Left grip", "A", "B", "Right stick", "Right grip"};
constexpr std::array<const char *, kButtonActions> kNames{"home",     "dock",          "leftClick", "rightClick",    "middleClick",
                                                          "keyboard", "screenshot",    "recline",   "followAll",     "gameMode",
                                                          "settings", "notifications", "volumeUp",  "volumeDown",    "mute"};
constexpr std::array<const char *, kButtonActions> kLabels{"Home",     "Dock",          "Left click",  "Right click",  "Middle click",
                                                           "Keyboard", "Screenshot",    "Recline",     "Follow all",   "Game mode",
                                                           "Settings", "Notifications", "Volume up",   "Volume down",  "Mute"};

bool isGrip(ControllerInput input)
{
    return input == ControllerInput::LeftGrip || input == ControllerInput::RightGrip;
}

//! Grips first: a chord reads "hold this, press that".
int rank(ControllerInput input)
{
    return isGrip(input) ? int(input) - 100 : int(input);
}

//! The first format's buttons, in its order, as the inputs they are now.
constexpr std::array<ControllerInput, 7> kOldButtons{ControllerInput::LeftPrimary, ControllerInput::LeftSecondary, ControllerInput::LeftMenu,
                                                    ControllerInput::LeftStick,   ControllerInput::RightPrimary,  ControllerInput::RightSecondary,
                                                    ControllerInput::RightStick};
constexpr std::array<const char *, 7> kOldKeys{"leftX", "leftY", "leftMenu", "leftStick", "rightA", "rightB", "rightStick"};

//! The same bindings, whatever their order.
bool sameBindings(const std::vector<Binding> &a, const std::vector<Binding> &b)
{
    return a.size() == b.size() && std::is_permutation(a.begin(), a.end(), b.begin());
}

} // namespace

std::optional<Binding> Binding::make(ControllerInput a, std::optional<ControllerInput> b)
{
    if (!b) {
        if (isGrip(a)) {
            return std::nullopt; // alone the grip grabs
        }
        return Binding{a, std::nullopt};
    }
    if (a == *b) {
        return std::nullopt;
    }
    if (rank(*b) < rank(a)) {
        std::swap(a, *b);
    }
    return Binding{a, b};
}

std::optional<Binding> Binding::fromName(const QString &name)
{
    const QStringList parts = name.split(QLatin1Char('+'));
    if (parts.size() < 1 || parts.size() > 2) {
        return std::nullopt;
    }
    ControllerInput a{};
    if (!ButtonMap::inputFromName(parts[0], &a)) {
        return std::nullopt;
    }
    if (parts.size() == 1) {
        return make(a);
    }
    ControllerInput b{};
    if (!ButtonMap::inputFromName(parts[1], &b)) {
        return std::nullopt;
    }
    return make(a, b);
}

QString Binding::name() const
{
    return second ? ButtonMap::inputName(first) + QLatin1Char('+') + ButtonMap::inputName(*second) : ButtonMap::inputName(first);
}

QString Binding::label() const
{
    return second ? ButtonMap::inputLabel(first) + QStringLiteral(" + ") + ButtonMap::inputLabel(*second) : ButtonMap::inputLabel(first);
}

InputMask Binding::mask() const
{
    return InputMask(inputBit(first) | (second ? inputBit(*second) : 0));
}

ButtonMap ButtonMap::defaults()
{
    ButtonMap map;
    const auto add = [&map](ButtonAction action, ControllerInput a, std::optional<ControllerInput> b = std::nullopt) {
        map.m_bindings[std::size_t(action)].push_back(*Binding::make(a, b));
    };
    add(ButtonAction::Home, ControllerInput::LeftSecondary);
    add(ButtonAction::Home, ControllerInput::LeftMenu);
    add(ButtonAction::Dock, ControllerInput::LeftPrimary);
    add(ButtonAction::RightClick, ControllerInput::RightPrimary);
    add(ButtonAction::RightClick, ControllerInput::LeftStick);
    add(ButtonAction::RightClick, ControllerInput::RightStick);
    add(ButtonAction::Recline, ControllerInput::LeftGrip, ControllerInput::RightSecondary);
    add(ButtonAction::Recline, ControllerInput::RightGrip, ControllerInput::RightSecondary);
    return map;
}

ButtonMap ButtonMap::fromJson(const QJsonObject &controls)
{
    ButtonMap map = defaults();
    if (controls.value(QLatin1String("version")).toInt() >= 2) {
        const QJsonObject bindings = controls.value(QLatin1String("bindings")).toObject();
        for (std::size_t a = 0; a < kButtonActions; ++a) {
            if (bindings.value(QLatin1String(kNames[a])).isArray()) {
                map.m_bindings[a].clear();
            }
        }
        // A binding belongs to one action: of two that list it the first keeps it; one listed beats a default.
        std::vector<Binding> claimed;
        for (std::size_t a = 0; a < kButtonActions; ++a) {
            for (const QJsonValue &value : bindings.value(QLatin1String(kNames[a])).toArray()) {
                const auto binding = Binding::fromName(value.toString());
                if (binding && std::find(claimed.begin(), claimed.end(), *binding) == claimed.end()) {
                    claimed.push_back(*binding);
                    map.assign(ButtonAction(a), *binding);
                }
            }
        }
        return map;
    }
    // The first format: button -> action, a missing or unknown one its default (then the buttons' actions were these).
    std::array<std::optional<ButtonAction>, 7> old{ButtonAction::Dock,       ButtonAction::Home,       ButtonAction::Home,      ButtonAction::RightClick,
                                                   ButtonAction::RightClick, std::nullopt,             ButtonAction::RightClick};
    bool any = false;
    for (std::size_t i = 0; i < old.size(); ++i) {
        const QString value = controls.value(QLatin1String(kOldKeys[i])).toString();
        ButtonAction action{};
        if (value == QLatin1String("none")) {
            old[i] = std::nullopt;
            any = true;
        } else if (actionFromName(value, &action) && action <= ButtonAction::Screenshot) {
            old[i] = action;
            any = true;
        }
    }
    if (!any) {
        return map;
    }
    for (std::size_t a = 0; a <= std::size_t(ButtonAction::Screenshot); ++a) {
        map.m_bindings[a].clear(); // those the first format had are rebuilt from its buttons
    }
    for (std::size_t i = 0; i < old.size(); ++i) {
        if (old[i]) {
            map.m_bindings[std::size_t(*old[i])].push_back(*Binding::make(kOldButtons[i]));
        }
    }
    return map;
}

bool ButtonMap::operator==(const ButtonMap &other) const
{
    for (std::size_t a = 0; a < kButtonActions; ++a) {
        if (!sameBindings(m_bindings[a], other.m_bindings[a])) {
            return false;
        }
    }
    return true;
}

QJsonObject ButtonMap::toJson() const
{
    const ButtonMap standard = defaults();
    QJsonObject bindings;
    for (std::size_t a = 0; a < kButtonActions; ++a) {
        if (!sameBindings(m_bindings[a], standard.m_bindings[a])) {
            QJsonArray list;
            for (const Binding &binding : m_bindings[a]) {
                list.append(binding.name());
            }
            bindings.insert(QLatin1String(kNames[a]), list);
        }
    }
    return QJsonObject{{QStringLiteral("version"), 2}, {QStringLiteral("bindings"), bindings}};
}

void ButtonMap::setBindings(ButtonAction action, const std::vector<Binding> &bindings)
{
    m_bindings[std::size_t(action)].clear();
    for (const Binding &binding : bindings) {
        assign(action, binding);
    }
}

std::optional<ButtonAction> ButtonMap::assign(ButtonAction action, const Binding &binding)
{
    const std::optional<ButtonAction> from = actionOf(binding);
    if (from == action) {
        return std::nullopt;
    }
    if (from) {
        auto &list = m_bindings[std::size_t(*from)];
        list.erase(std::remove(list.begin(), list.end(), binding), list.end());
    }
    m_bindings[std::size_t(action)].push_back(binding);
    return from;
}

std::optional<ButtonAction> ButtonMap::actionOf(const Binding &binding) const
{
    for (std::size_t a = 0; a < kButtonActions; ++a) {
        if (std::find(m_bindings[a].begin(), m_bindings[a].end(), binding) != m_bindings[a].end()) {
            return ButtonAction(a);
        }
    }
    return std::nullopt;
}

bool ButtonMap::isDefault(ButtonAction action) const
{
    return sameBindings(m_bindings[std::size_t(action)], defaults().m_bindings[std::size_t(action)]);
}

ButtonMap ButtonMap::effective() const
{
    if (!m_bindings[std::size_t(ButtonAction::Home)].empty() || !m_bindings[std::size_t(ButtonAction::Dock)].empty()) {
        return *this;
    }
    ButtonMap map = *this;
    map.assign(ButtonAction::Home, *Binding::make(ControllerInput::LeftMenu)); // nothing else leads home: the menu button does
    return map;
}

QString ButtonMap::name(ButtonAction action)
{
    return QLatin1String(kNames[std::size_t(action)]);
}

QString ButtonMap::label(ButtonAction action)
{
    return QLatin1String(kLabels[std::size_t(action)]);
}

bool ButtonMap::actionFromName(const QString &name, ButtonAction *action)
{
    for (std::size_t i = 0; i < kNames.size(); ++i) {
        if (name == QLatin1String(kNames[i])) {
            *action = ButtonAction(i);
            return true;
        }
    }
    return false;
}

QString ButtonMap::inputName(ControllerInput input)
{
    return QLatin1String(kInputNames[std::size_t(input)]);
}

QString ButtonMap::inputLabel(ControllerInput input)
{
    return QString::fromUtf8(kInputLabels[std::size_t(input)]);
}

bool ButtonMap::inputFromName(const QString &name, ControllerInput *input)
{
    for (std::size_t i = 0; i < kInputNames.size(); ++i) {
        if (name == QLatin1String(kInputNames[i])) {
            *input = ControllerInput(i);
            return true;
        }
    }
    return false;
}

BindingTracker::Result BindingTracker::update(const ButtonMap &map, InputMask down)
{
    Result result;
    const InputMask pressedNow = InputMask(down & ~m_down);
    const InputMask releasedNow = InputMask(m_down & ~down);
    // A grip in a chord is a modifier: held first, then the other button (a grip squeezed while a button is held, to
    // grab or by accident, does not turn that button's press or click into the chord's action).
    const InputMask grips = InputMask(inputBit(ControllerInput::LeftGrip) | inputBit(ControllerInput::RightGrip));
    // Which inputs are part of a chord of two buttons that is assigned: either may be pressed first, so they wait for
    // their release. A button in a chord with a grip does not: without the grip held first, no chord can follow.
    InputMask inChord = 0;
    for (std::size_t a = 0; a < kButtonActions; ++a) {
        for (const Binding &binding : map.bindings(ButtonAction(a))) {
            if (binding.isChord() && !(binding.mask() & grips)) {
                inChord |= binding.mask();
            }
        }
    }
    // Chords first: the second input goes down while the first is held (both in one frame counts).
    InputMask chorded = m_chorded;
    for (std::size_t a = 0; a < kButtonActions; ++a) {
        const ButtonAction action = ButtonAction(a);
        for (const Binding &binding : map.bindings(action)) {
            if (!binding.isChord()) {
                continue;
            }
            const InputMask mask = binding.mask();
            if ((down & mask) != mask) {
                continue;
            }
            // With a grip: the other button went down while the grip is held. Two buttons: either went down last.
            const InputMask starts = (mask & grips) ? InputMask(mask & ~grips) : mask;
            if (pressedNow & starts) {
                chorded |= mask;
                if (!ButtonMap::isHeld(action)) {
                    result.pressed |= actionBit(action);
                }
            }
            if (ButtonMap::isHeld(action) && (chorded & mask) == mask) {
                result.held |= actionBit(action);
                result.heldBy.emplace_back(action, binding);
            }
        }
    }
    // A button alone.
    for (std::size_t a = 0; a < kButtonActions; ++a) {
        const ButtonAction action = ButtonAction(a);
        for (const Binding &binding : map.bindings(action)) {
            if (binding.isChord()) {
                continue;
            }
            const InputMask bit = binding.mask();
            if (ButtonMap::isHeld(action)) {
                if ((down & bit) && !(chorded & bit)) {
                    result.held |= actionBit(action);
                    result.heldBy.emplace_back(action, binding);
                }
            } else if (inChord & bit) {
                if ((releasedNow & bit) && !(chorded & bit)) {
                    result.pressed |= actionBit(action);
                }
            } else if ((pressedNow & bit) && !(chorded & bit)) {
                result.pressed |= actionBit(action);
            }
        }
    }
    m_chorded = InputMask(chorded & down);
    m_down = down;
    return result;
}

std::optional<Binding> BindingRecorder::update(InputMask down)
{
    const InputMask pressedNow = InputMask(down & ~m_down);
    m_down = down;
    for (std::size_t i = 0; i < kControllerInputs; ++i) {
        if ((pressedNow & InputMask(1u << i)) && std::find(m_order.begin(), m_order.end(), ControllerInput(i)) == m_order.end()) {
            m_order.push_back(ControllerInput(i));
        }
    }
    if (down != 0 || m_order.empty()) {
        return std::nullopt;
    }
    const std::vector<ControllerInput> order = std::move(m_order);
    m_order.clear();
    return Binding::make(order[0], order.size() > 1 ? std::optional<ControllerInput>(order[1]) : std::nullopt);
}

} // namespace RiftHome::Spatial
