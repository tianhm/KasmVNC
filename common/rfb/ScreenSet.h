/* Copyright 2009 Pierre Ossman for Cendio AB
 *
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this software; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307,
 * USA.
 */

// Management class for the RFB virtual screens

#ifndef __RFB_SCREENSET_INCLUDED__
#define __RFB_SCREENSET_INCLUDED__

#include <cstdio>
#include <cstring>

#include <vector>
#include <algorithm>
#include <rdr/types.h>
#include <rfb/Rect.h>
#include <set>

namespace rfb {

    // rfb::Screen
    //
    // Represents a single RFB virtual screen, which includes
    // coordinates, an id and flags.

    struct Screen {
        Screen() = default;
        Screen(uint32_t id_, int x_, int y_, int w_, int h_, uint32_t flags_) :
            id(id_), dimensions(x_, y_, x_ + w_, y_ + h_), flags(flags_) {};

        bool operator==(const Screen &r) const {
            if (id != r.id)
                return false;
            if (!dimensions.equals(r.dimensions))
                return false;
            if (flags != r.flags)
                return false;
            return true;
        }

        uint32_t id{};
        Rect dimensions;
        uint32_t flags{};
    };

    // rfb::ScreenSet
    //
    // Represents a complete screen configuration, excluding framebuffer
    // dimensions.

    struct ScreenSet {
        static constexpr int MAX_SCREENS = 255;
        ScreenSet() = default;

        using iterator = std::vector<Screen>::iterator;
        using const_iterator = std::vector<Screen>::const_iterator;

        iterator begin() {
            return screens.begin();
        }
        [[nodiscard]] const_iterator begin() const {
            return screens.begin();
        }
        iterator end() {
            return screens.end();
        }
        [[nodiscard]] const_iterator end() const {
            return screens.end();
        }

        [[nodiscard]] int num_screens() const {
            return static_cast<int>(screens.size());
        }

        void add_screen(const Screen &screen) {
            screens.push_back(screen);
            std::sort(screens.begin(), screens.end(), compare_screen);
        }
        void remove_screen(rdr::U32 id) {
            //std::erase_if(screens, [id](const Screen &screen) { return screen.id == id; });
            screens.erase(std::remove_if(screens.begin(), screens.end(), [id](const Screen &screen) { return screen.id == id; }), screens.end());
        }

        [[nodiscard]] bool validate(int fb_width, int fb_height) const {
            std::set<uint32_t> seen_ids;
            Rect fb_rect;

            if (screens.empty())
                return false;
            if (num_screens() > MAX_SCREENS)
                return false;

            fb_rect.setXYWH(0, 0, fb_width, fb_height);

            for (auto &screen: screens) {
                if (screen.dimensions.is_empty())
                    return false;
                if (!screen.dimensions.enclosed_by(fb_rect))
                    return false;
                if (seen_ids.find(screen.id) != seen_ids.end())
                    return false;
                seen_ids.insert(screen.id);
            }

            return true;
        };

        void print(char *str, size_t len) const {
            char buffer[128];
            snprintf(buffer, sizeof(buffer), "%d screen(s)\n", num_screens());
            str[0] = '\0';
            strncat(str, buffer, len - 1 - strlen(str));
            for (auto &screen: screens) {
                snprintf(buffer,
                         sizeof(buffer),
                         "    %10d (0x%08x): %dx%d+%d+%d (flags 0x%08x)\n",
                         static_cast<int>(screen.id),
                         static_cast<unsigned>(screen.id),
                         screen.dimensions.width(),
                         screen.dimensions.height(),
                         screen.dimensions.tl.x,
                         screen.dimensions.tl.y,
                         static_cast<unsigned>(screen.flags));
                strncat(str, buffer, len - 1 - strlen(str));
            }
        };

        bool operator==(const ScreenSet &r) const {
            auto a = screens;
            //std::sort(a.begin(), a.end(), compare_screen);
            auto b = r.screens;
            //std::sort(b.begin(), b.end(), compare_screen);
            return a == b;
        }

        bool operator!=(const ScreenSet &r) const {
            return !operator==(r);
        }

        std::vector<Screen> screens;

    private:
        static bool compare_screen(const Screen &first, const Screen &second) {
            return first.id < second.id;
        }
    };
}; // namespace rfb

#endif
