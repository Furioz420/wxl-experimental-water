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

#pragma once

// NOT A JSON LIBRARY, and deliberately not one.
//
// The file this reads has a fixed shape that we also write:
//
//   { "some.key": { "en": "text", "fr": "texte" }, ... }
//
// No numbers, no arrays, no nesting past two levels, no null. Pulling in a general parser to read
// that would be a dependency and a build cost for a grammar this is six branches wide -- and a
// general parser would still not answer the question that actually matters here, which is which
// keys are missing from which language.
//
// It streams rather than building a tree: the caller is handed each (key, language, value) as it is
// found. Nothing is allocated by the reader.
namespace wxl::water::json
{
    /// Called once per leaf. All three pointers are into a caller-owned mutable buffer, NUL
    /// terminated in place, and valid until that buffer is freed or re-parsed.
    using LeafFn = void (*)(void* user, const char* key, const char* lang, const char* value);

    /// Parses in place -- @p text is modified, because strings are terminated and unescaped where
    /// they sit rather than copied.
    ///
    /// @param errLine  receives the 1-based line of the first syntax error, or 0 on success.
    /// @return false on malformed input, having already reported whatever it read before the fault.
    ///         A truncated file is therefore a partial load, not an empty one: half a translation
    ///         beats none, and the log says where it stopped.
    bool ParseInPlace(char* text, LeafFn fn, void* user, int* errLine);
}
