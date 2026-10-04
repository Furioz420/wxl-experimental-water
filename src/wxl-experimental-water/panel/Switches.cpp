// One switch per contribution, in one registry.
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

#include "wxl-experimental-water/panel/Switches.hpp"
#include "wxl-experimental-water/panel/Lang.hpp"

#include <cstdio>

namespace
{
    using wxl::water::Sw;

    // Indexed by Sw, and nothing but identifiers now: the label and the description live in the
    // string file under switch.<id>.label and switch.<id>.help. The order here IS the enum's order,
    // and these strings are what a saved profile stores -- so entries may be appended, and must not
    // be reordered or renamed.
    const char* const kId[] = {
        "detail", "chop", "nonlinear", "whitecaps", "shoaling",
        "shademaps", "animnormals", "noise", "foamtex", "sky", "glitter", "sss", "caustics",
        "ripple", "troughshade", "nightfloor", "bottom", "refraction", "extinction",
        "foam", "foamshore", "foamcrest",
        "detailfilter", "depthlimit",
    };

    static_assert(sizeof(kId) / sizeof(kId[0]) == static_cast<size_t>(Sw::Count),
                  "the switch identifiers and the enum have drifted apart");

    bool g_on[static_cast<size_t>(Sw::Count)];
    bool g_init = false;

    void Init()
    {
        if (g_init) return;
        g_init = true;
        for (bool& b : g_on) b = true;
    }

    bool Valid(Sw s)
    {
        return static_cast<int>(s) >= 0 && static_cast<int>(s) < static_cast<int>(Sw::Count);
    }

    /// Composes switch.<id>.<suffix> and looks it up.
    ///
    /// The buffer is static and reused, which is safe for exactly the way this is consumed: the
    /// result of the lookup is a pointer into the string table, never into this scratch, so the
    /// next call overwrites nothing anybody is still holding.
    const char* Lookup(Sw s, const char* suffix)
    {
        if (!Valid(s)) return "";
        static char key[64];
        std::snprintf(key, sizeof(key), "switch.%s.%s", kId[static_cast<size_t>(s)], suffix);
        return wxl::water::L(key);
    }
}

namespace wxl::water
{
    bool On(Sw s)
    {
        Init();
        // An out-of-range switch reads as ON. Reading as off would make a programming slip look
        // like a deliberately disabled effect, which is the one failure here that is invisible
        // rather than obvious.
        return Valid(s) ? g_on[static_cast<size_t>(s)] : true;
    }

    void SetOn(Sw s, bool on) { Init(); if (Valid(s)) g_on[static_cast<size_t>(s)] = on; }
    bool* Slot(Sw s) { Init(); return Valid(s) ? &g_on[static_cast<size_t>(s)] : nullptr; }

    const char* SwitchId(Sw s)    { return Valid(s) ? kId[static_cast<size_t>(s)] : ""; }
    const char* SwitchLabel(Sw s) { return Lookup(s, "label"); }
    const char* SwitchHelp(Sw s)  { return Lookup(s, "help"); }

    void AllOn() { Init(); for (bool& b : g_on) b = true; }
}
