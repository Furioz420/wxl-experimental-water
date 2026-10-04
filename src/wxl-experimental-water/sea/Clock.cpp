// The clock the sea runs on.
// Copyright (C) 2026 WarcraftXL
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

#include "wxl-experimental-water/sea/Clock.hpp"

#include <windows.h>

namespace
{
    /// Performance counter, not the millisecond tick: the phase advance of the shortest wave in a
    /// storm is a fraction of a frame, and a 1 ms quantum reads as a stutter on every crest.
    double CounterSeconds()
    {
        static LARGE_INTEGER freq = [] {
            LARGE_INTEGER f{};
            QueryPerformanceFrequency(&f);
            if (f.QuadPart == 0) f.QuadPart = 1;
            return f;
        }();
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        return static_cast<double>(now.QuadPart) / static_cast<double>(freq.QuadPart);
    }

    /// Counter reading the sea's time is measured from. Taken once, at load; never reset, because
    /// moving it backwards would run the sea in reverse for the length of the correction.
    const double g_origin = CounterSeconds();
}

namespace wxl::water
{
    double SessionSeconds()
    {
        return CounterSeconds() - g_origin;
    }

    double Now()
    {
        return SessionSeconds() * 0.75;
    }
}
