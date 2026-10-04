// Water tuning profiles as a shareable text key.
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

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// A PROFILE IS A STRING SOMEBODY CAN PASTE INTO A CHAT WINDOW, which is the whole requirement and
// the only reason this is not just the settings file.
//
// So the encoding has to survive being copied by hand: no characters a chat client will linkify,
// eat or reflow, and whitespace anywhere has to be harmless.
//
// A KEY FROM ANOTHER BUILD IS LOADED, NOT REFUSED, and that is a reversal of what this file used to
// say. The old rule -- refuse anything whose version or field count is not this build's -- was
// protecting against a payload whose bytes would land on the wrong settings. But the enumeration
// those bytes are laid out by is APPEND-ONLY, so a key from an older build is a strict PREFIX of
// what this one writes: every byte in it lands on the setting it was written from, and the fields
// added since simply keep their current values. Refusing it threw away a sea somebody had spent an
// evening finding, to protect against a misalignment that cannot happen.
//
// What is still checked is whether the key is INTACT -- prefix, alphabet, checksum, self-consistent
// length. Those catch a token that was mangled in transit, which is the failure that would actually
// scramble a sea. The caller is told how many fields arrived so it can say so.
namespace wxl::water::profile
{
    /// Wraps a settings payload into a shareable key.
    ///
    /// The version and field count are still written. They are no longer gates -- they are what
    /// lets the far end say "this key carries 84 of the 91 settings I know" instead of guessing.
    std::string Encode(uint8_t version, const std::vector<uint8_t>& payload);

    /// Unwraps a key. Returns false -- and leaves @p out untouched -- only when the key is not
    /// intact: a foreign prefix, a character outside the alphabet, a failed checksum, or a length
    /// that disagrees with its own field count.
    ///
    /// @param version  written back if not null. Reported, never compared.
    /// @param fields   written back if not null: how many settings the key carries. The caller
    ///                 applies as many as it has settings for and leaves the rest as they were.
    bool Decode(const char* key, std::vector<uint8_t>& out,
                uint8_t* version = nullptr, size_t* fields = nullptr);

    /// Bytes one field occupies. Everything the sea is tuned by is four bytes wide -- a float, or a
    /// seed -- so the field count and the payload length determine each other, which is what lets a
    /// mismatch be caught before a single value is applied.
    inline constexpr size_t kFieldBytes = 4;
}
