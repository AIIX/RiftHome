// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "spatial/WindowFrame.hpp"

#include <algorithm>
#include <vector>

namespace RiftHome::Spatial
{

/*!
 * Notifications in the room: a card above the dock's place for a few seconds when one arrives, and
 * the dock's list (its bell) of the newest ones with a close button each and "Clear all". A notification with actions
 * ("Open", "Show in folder") has a row of buttons under its text, up to kMaxActions; its card is that much
 * taller. Local metres, origin at the centre, x right, y up (the card's, the list's). Pure.
 */
struct NotificationHit {
    enum class Part {
        None,
        Card, //!< tap: its default action, then gone
        Close, //!< its x: gone
        ClearAll,
        Action, //!< one of its action buttons (action): that action, then gone
    };
    Part part = Part::None;
    int row = -1; //!< the list's row (0: the newest); the card: 0
    int action = -1; //!< Action: which button, left to right
    bool operator==(const NotificationHit &) const = default;
};

struct NotificationLayout {
    static constexpr float kCardWidth = 0.32f; //!< about 24 degrees at the dock's 0.76 m
    static constexpr float kCardHeight = 0.082f; //!< three lines of text a degree or more tall at the dock's distance
    static constexpr float kClose = 0.03f; //!< the close button's diameter
    static constexpr float kPadding = 0.012f; //!< the list's, around its rows
    static constexpr float kHeader = 0.042f; //!< the list's title row with "Clear all"
    static constexpr float kClearWidth = 0.085f;
    static constexpr float kClearHeight = 0.03f;
    static constexpr float kRowGap = 0.008f;
    static constexpr int kMaxRows = 4;
    static constexpr float kGapAbove = 0.02f; //!< between the dock and what sits above it
    static constexpr int kMaxActions = 3;
    static constexpr float kActionsHeight = 0.036f; //!< the row of action buttons under a card's text
    static constexpr float kActionHeight = 0.026f;
    static constexpr float kActionGap = 0.008f;
    static constexpr float kActionSide = 0.017f; //!< from the card's sides

    //! A card's height with @p actions buttons.
    static float cardHeight(int actions)
    {
        return kCardHeight + (actions > 0 ? kActionsHeight : 0.0f);
    }
    //! The @p i-th of @p n action buttons of a card centred at @p card, equal widths along its bottom.
    static FrameRect actionOf(const FrameRect &card, int i, int n)
    {
        n = std::clamp(n, 1, kMaxActions);
        const float width = (card.width - 2.0f * kActionSide - float(n - 1) * kActionGap) / float(n);
        const float x = card.x - card.width / 2.0f + kActionSide + width / 2.0f + float(i) * (width + kActionGap);
        const float y = card.y - card.height / 2.0f + kActionsHeight / 2.0f + 0.002f;
        return {x, y, width, kActionHeight};
    }

    //! The close button of a card centred at @p card: its top-right corner.
    static FrameRect closeOf(const FrameRect &card)
    {
        return {card.x + card.width / 2.0f - kClose * 0.95f, card.y + card.height / 2.0f - kClose * 0.85f, kClose, kClose};
    }
    //! On a card centred at @p card with @p actions buttons: its x, a button, else the card itself.
    static NotificationHit hitOn(const FrameRect &card, int actions, int row, float px, float py)
    {
        if (closeOf(card).grown(0.006f, 0.006f).contains(px, py)) {
            return {NotificationHit::Part::Close, row};
        }
        const int n = std::min(actions, kMaxActions);
        for (int i = 0; i < n; ++i) {
            if (actionOf(card, i, n).grown(0.002f, 0.004f).contains(px, py)) {
                return {NotificationHit::Part::Action, row, i};
            }
        }
        return card.contains(px, py) ? NotificationHit{NotificationHit::Part::Card, row} : NotificationHit{};
    }
    //! On the card that shows when a notification arrives (card-local), with @p actions buttons.
    static NotificationHit cardHit(float px, float py, int actions = 0)
    {
        return hitOn({0.0f, 0.0f, kCardWidth, cardHeight(actions)}, actions, 0, px, py);
    }

    //! Rows the list shows for @p count notifications (one, saying there are none, when empty).
    static int rows(int count)
    {
        return std::clamp(count, 1, kMaxRows);
    }
    static float listWidth()
    {
        return kCardWidth + 2.0f * kPadding;
    }
    //! @p actions: the shown rows' action buttons, newest first (missing: none).
    static int actionsOf(const std::vector<int> &actions, int row)
    {
        return row >= 0 && size_t(row) < actions.size() ? actions[size_t(row)] : 0;
    }
    static float listHeight(int count, const std::vector<int> &actions = {})
    {
        const int n = rows(count);
        float cards = 0.0f;
        for (int row = 0; row < n; ++row) {
            cards += cardHeight(actionsOf(actions, row));
        }
        return 2.0f * kPadding + kHeader + cards + float(n - 1) * kRowGap;
    }
    //! The row-th card (0: the newest, at the top, under the title).
    static FrameRect listCard(int row, int count, const std::vector<int> &actions = {})
    {
        float top = listHeight(count, actions) / 2.0f - kPadding - kHeader;
        for (int above = 0; above < row; ++above) {
            top -= cardHeight(actionsOf(actions, above)) + kRowGap;
        }
        const float height = cardHeight(actionsOf(actions, row));
        return {0.0f, top - height / 2.0f, kCardWidth, height};
    }
    static FrameRect clearAll(int count, const std::vector<int> &actions = {})
    {
        return {kCardWidth / 2.0f - kClearWidth / 2.0f, listHeight(count, actions) / 2.0f - kPadding - kHeader / 2.0f, kClearWidth, kClearHeight};
    }
    static bool listContains(float px, float py, int count, const std::vector<int> &actions = {})
    {
        const float height = listHeight(count, actions);
        return px >= -listWidth() / 2.0f && px <= listWidth() / 2.0f && py >= -height / 2.0f && py <= height / 2.0f;
    }
    static NotificationHit listHit(float px, float py, int count, const std::vector<int> &actions = {})
    {
        if (count > 0 && clearAll(count, actions).grown(0.004f, 0.006f).contains(px, py)) {
            return {NotificationHit::Part::ClearAll, -1};
        }
        for (int row = 0; row < std::min(count, kMaxRows); ++row) {
            const NotificationHit hit = hitOn(listCard(row, count, actions), actionsOf(actions, row), row, px, py);
            if (hit.part != NotificationHit::Part::None) {
                return hit;
            }
        }
        return {};
    }
};

} // namespace RiftHome::Spatial
