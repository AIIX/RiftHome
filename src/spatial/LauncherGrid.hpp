// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <array>
#include <cmath>

namespace RiftHome::Spatial
{

/*!
 * Home: pages of round app icons floating in front of the
 * user in rows of 5, 6 and 5, the short rows offset by half an icon (Vision Pro's shape), names under them, page dots
 * below, and under those the running apps: a card per open window. Home-local metres, origin at the
 * whole's centre, x right, y up. Pure.
 */
struct LauncherGrid {
    static constexpr std::array<int, 3> kRowLength{5, 6, 5};
    static constexpr int kRows = 3;
    static constexpr int kPerPage = 16;
    static constexpr float kCell = 0.26f; //!< metres (about 10 degrees at 1.5 m)
    static constexpr float kRowPitch = 0.27f;
    static constexpr float kPlate = 0.16f; //!< the round icon plate's diameter
    static constexpr float kSearch = 0.09f; //!< the search field's row, above the apps
    static constexpr float kFooter = 0.07f; //!< the page dots
    static constexpr float kStrip = 0.13f; //!< the running apps' row
    static constexpr float kCardWidest = 0.38f; //!< a running app's card pitch while there are few (whole titles)
    static constexpr float kCardHeight = 0.105f;
    static constexpr int kMaxCards = 6;

    static float width()
    {
        return 6 * kCell;
    }
    static float height()
    {
        return kSearch + kRows * kRowPitch + kFooter + kStrip;
    }
    //! The top of the apps' rows.
    static float rowsTop()
    {
        return height() / 2.0f - kSearch;
    }
    //! The page dots' centre line.
    static float footerY()
    {
        return rowsTop() - kRows * kRowPitch - kFooter / 2.0f;
    }
    //! The search field: centre and size.
    static void searchField(float &x, float &y, float &width, float &height_)
    {
        x = 0.0f;
        y = height() / 2.0f - kSearch / 2.0f;
        width = 0.62f;
        height_ = 0.062f;
    }
    static bool onSearch(float px, float py)
    {
        float x, y, w, h;
        searchField(x, y, w, h);
        return std::fabs(px - x) <= w / 2.0f + 0.01f && std::fabs(py - y) <= h / 2.0f + 0.01f;
    }
    //! The running apps' card pitch: wide while there are few, sharing Home's width when there are many.
    static float cardPitch(int count)
    {
        return std::min(kCardWidest, width() / float(std::clamp(count, 1, kMaxCards)));
    }
    //! Running app @p index of @p count: its card (centre, size), and the close button in its top-right corner.
    static void card(int index, int count, float &x, float &y, float &width, float &height_)
    {
        const int n = std::min(count, kMaxCards);
        const float pitch = cardPitch(count);
        x = (float(index) - (n - 1) / 2.0f) * pitch;
        y = -height() / 2.0f + kStrip / 2.0f;
        width = pitch - 0.02f;
        height_ = kCardHeight;
    }
    static void closeButton(int index, int count, float &x, float &y, float &size)
    {
        float cx, cy, w, h;
        card(index, count, cx, cy, w, h);
        size = 0.034f;
        x = cx + w / 2.0f - size / 2.0f - 0.008f;
        y = cy + h / 2.0f - size / 2.0f - 0.008f;
    }
    //! The running app's card at a home-local point (-1: none); @p close: on its close button.
    static int cardAt(float px, float py, int count, bool &close)
    {
        close = false;
        for (int i = 0; i < std::min(count, kMaxCards); ++i) {
            float x, y, w, h;
            card(i, count, x, y, w, h);
            if (std::fabs(px - x) <= w / 2.0f && std::fabs(py - y) <= h / 2.0f) {
                float bx, by, size;
                closeButton(i, count, bx, by, size);
                close = std::fabs(px - bx) <= size * 0.75f && std::fabs(py - by) <= size * 0.75f;
                return i;
            }
        }
        return -1;
    }
    static int pages(int apps)
    {
        return std::max(1, (apps + kPerPage - 1) / kPerPage);
    }
    static int rowStart(int row)
    {
        int start = 0;
        for (int r = 0; r < row; ++r) {
            start += kRowLength[size_t(r)];
        }
        return start;
    }
    //! A cell's centre (index on the page, row by row); its plate sits a little above, its name below.
    static void cellCentre(int cell, float &x, float &y)
    {
        int row = 0;
        while (row < kRows - 1 && cell >= rowStart(row + 1)) {
            ++row;
        }
        const int n = kRowLength[size_t(row)];
        x = (float(cell - rowStart(row)) - (n - 1) / 2.0f) * kCell;
        y = rowsTop() - (row + 0.5f) * kRowPitch;
    }
    //! The cell at a home-local point, or -1 (between a row's icons counts: no dead spots; the footer, off).
    static int cellAt(float x, float y)
    {
        const int row = int(std::floor((rowsTop() - y) / kRowPitch));
        if (row < 0 || row >= kRows) {
            return -1;
        }
        const int n = kRowLength[size_t(row)];
        const int i = int(std::floor(x / kCell + n / 2.0f));
        if (i < 0 || i >= n) {
            return -1;
        }
        return rowStart(row) + i;
    }
    static bool contains(float x, float y)
    {
        return std::fabs(x) <= width() / 2.0f && std::fabs(y) <= height() / 2.0f;
    }
};

} // namespace RiftHome::Spatial
