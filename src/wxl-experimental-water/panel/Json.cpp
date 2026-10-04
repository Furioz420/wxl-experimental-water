// A reader for the one JSON shape this module needs.
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

#include "wxl-experimental-water/panel/Json.hpp"

namespace
{
    struct Cursor
    {
        char* p;
        int   line = 1;

        void Skip()
        {
            for (;;)
            {
                while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
                {
                    if (*p == '\n') ++line;
                    ++p;
                }
                // Line comments are not JSON. They are allowed here because this file is edited by
                // hand and a translator needs somewhere to leave a note about a phrasing.
                if (p[0] == '/' && p[1] == '/')
                {
                    while (*p && *p != '\n') ++p;
                    continue;
                }
                return;
            }
        }

        bool Eat(char c)
        {
            Skip();
            if (*p != c) return false;
            ++p;
            return true;
        }
    };

    /// Reads a quoted string, terminating and unescaping it IN PLACE.
    ///
    /// Unescaping in place is always possible because every escape sequence is at least as long as
    /// what it produces -- two characters becoming one, or six becoming at most three. The write
    /// pointer can therefore never overtake the read pointer.
    char* ReadString(Cursor& c)
    {
        c.Skip();
        if (*c.p != '"') return nullptr;
        ++c.p;

        char* out = c.p;
        char* start = out;
        while (*c.p && *c.p != '"')
        {
            if (*c.p == '\\')
            {
                ++c.p;
                switch (*c.p)
                {
                    case 'n':  *out++ = '\n'; ++c.p; break;
                    case 't':  *out++ = '\t'; ++c.p; break;
                    case 'r':  ++c.p; break;             // dropped: the panel wraps its own text
                    case '"':  *out++ = '"';  ++c.p; break;
                    case '\\': *out++ = '\\'; ++c.p; break;
                    case '/':  *out++ = '/';  ++c.p; break;
                    case 'u':
                    {
                        // Only the Latin-1 range is decoded, and only to one byte. The panel's font
                        // atlas covers exactly that range, so a codepoint above it would draw as a
                        // blank whatever this did -- and the file is UTF-8, where an accented
                        // character can simply be typed rather than escaped.
                        ++c.p;
                        unsigned v = 0;
                        for (int i = 0; i < 4 && *c.p; ++i, ++c.p)
                        {
                            const char h = *c.p;
                            v <<= 4;
                            if (h >= '0' && h <= '9') v |= static_cast<unsigned>(h - '0');
                            else if (h >= 'a' && h <= 'f') v |= static_cast<unsigned>(h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') v |= static_cast<unsigned>(h - 'A' + 10);
                            else return nullptr;
                        }
                        if (v && v < 0x100) *out++ = static_cast<char>(v);
                        break;
                    }
                    default: return nullptr;
                }
                continue;
            }
            if (*c.p == '\n') ++c.line;
            *out++ = *c.p++;
        }
        if (*c.p != '"') return nullptr;
        ++c.p;          // past the closing quote
        *out = '\0';    // terminate where the content ended, not where the quote was
        return start;
    }
}

namespace wxl::water::json
{
    bool ParseInPlace(char* text, LeafFn fn, void* user, int* errLine)
    {
        if (errLine) *errLine = 0;
        if (!text || !fn) return false;

        Cursor c{ text };
        auto fail = [&]() { if (errLine) *errLine = c.line; return false; };

        if (!c.Eat('{')) return fail();
        c.Skip();
        if (c.Eat('}')) return true;   // an empty table is valid and means "no overrides"

        for (;;)
        {
            char* key = ReadString(c);
            if (!key) return fail();
            if (!c.Eat(':')) return fail();
            if (!c.Eat('{')) return fail();

            c.Skip();
            if (!c.Eat('}'))
            {
                for (;;)
                {
                    char* lang = ReadString(c);
                    if (!lang) return fail();
                    if (!c.Eat(':')) return fail();
                    char* value = ReadString(c);
                    if (!value) return fail();

                    fn(user, key, lang, value);

                    if (c.Eat(',')) continue;
                    if (c.Eat('}')) break;
                    return fail();
                }
            }

            if (c.Eat(',')) { c.Skip(); if (c.Eat('}')) return true; continue; }
            if (c.Eat('}')) return true;
            return fail();
        }
    }
}
