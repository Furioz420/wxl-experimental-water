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

#include "wxl-experimental-water/panel/Profile.hpp"

namespace
{
    /// The prefix a key is recognised by. Also what makes a key identifiable as one when it turns
    /// up out of context in a chat log.
    constexpr char kPrefix[] = "WXLSEA1:";
    constexpr size_t kPrefixLen = sizeof(kPrefix) - 1;

    /// Base64, URL-and-chat safe: '-' and '_' in place of '+' and '/', and no padding.
    ///
    /// The two substitutions are not cosmetic. A '+' survives a paste and then does not survive a
    /// URL, and a '/' makes some clients treat the key as a path and linkify half of it. Padding is
    /// dropped because '=' at the end of a token is the other thing clients like to strip.
    constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

    int Value(char c)
    {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '-') return 62;
        if (c == '_') return 63;
        return -1;
    }

    /// Fletcher-16 over the payload.
    ///
    /// Chosen over a plain sum because the failure it has to catch is a key that lost or gained a
    /// character in transit, and a sum is blind to reordering -- which is exactly what a wrapped
    /// line does to a pasted token.
    uint16_t Checksum(const uint8_t* data, size_t len)
    {
        uint16_t a = 0xFF, b = 0xFF;
        for (size_t i = 0; i < len; ++i)
        {
            a = static_cast<uint16_t>((a + data[i]) % 255);
            b = static_cast<uint16_t>((b + a) % 255);
        }
        return static_cast<uint16_t>((b << 8) | a);
    }
}

namespace wxl::water::profile
{
    std::string Encode(uint8_t version, const std::vector<uint8_t>& payload)
    {
        const size_t fields = payload.size() / kFieldBytes;

        std::vector<uint8_t> framed;
        framed.reserve(payload.size() + 5);
        framed.push_back(version);
        framed.push_back(static_cast<uint8_t>(fields & 0xFF));
        framed.push_back(static_cast<uint8_t>((fields >> 8) & 0xFF));
        framed.insert(framed.end(), payload.begin(), payload.end());

        const uint16_t sum = Checksum(framed.data(), framed.size());
        framed.push_back(static_cast<uint8_t>(sum & 0xFF));
        framed.push_back(static_cast<uint8_t>(sum >> 8));

        std::string out(kPrefix);
        out.reserve(kPrefixLen + (framed.size() * 4 + 2) / 3);
        for (size_t i = 0; i < framed.size(); i += 3)
        {
            const uint32_t b0 = framed[i];
            const uint32_t b1 = (i + 1 < framed.size()) ? framed[i + 1] : 0u;
            const uint32_t b2 = (i + 2 < framed.size()) ? framed[i + 2] : 0u;
            const uint32_t trio = (b0 << 16) | (b1 << 8) | b2;
            // Only as many characters as the remaining bytes actually carry: four characters for
            // three bytes, three for two, two for one. Emitting the full quartet would decode back
            // to bytes nobody put in.
            const size_t left = framed.size() - i;
            out.push_back(kAlphabet[(trio >> 18) & 0x3F]);
            out.push_back(kAlphabet[(trio >> 12) & 0x3F]);
            if (left > 1) out.push_back(kAlphabet[(trio >> 6) & 0x3F]);
            if (left > 2) out.push_back(kAlphabet[trio & 0x3F]);
        }
        return out;
    }

    bool Decode(const char* key, std::vector<uint8_t>& out, uint8_t* version, size_t* fields)
    {
        if (!key) return false;

        // Leading whitespace is skipped rather than rejected: a key arrives pasted, and a paste
        // arrives with whatever the client put around it.
        while (*key == ' ' || *key == '\t' || *key == '\r' || *key == '\n') ++key;
        for (size_t i = 0; i < kPrefixLen; ++i)
            if (key[i] != kPrefix[i]) return false;

        std::vector<uint8_t> framed;
        uint32_t acc = 0;
        int bits = 0;
        for (const char* p = key + kPrefixLen; *p; ++p)
        {
            if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') continue;
            const int v = Value(*p);
            if (v < 0) return false;   // a character that is not ours: the key is not intact
            acc = (acc << 6) | static_cast<uint32_t>(v);
            bits += 6;
            if (bits >= 8)
            {
                bits -= 8;
                framed.push_back(static_cast<uint8_t>((acc >> bits) & 0xFF));
            }
        }

        // Header, at least one field, checksum.
        if (framed.size() < 3 + kFieldBytes + 2) return false;

        const uint16_t want = static_cast<uint16_t>(framed[framed.size() - 2])
                            | static_cast<uint16_t>(framed[framed.size() - 1] << 8);
        framed.resize(framed.size() - 2);
        if (Checksum(framed.data(), framed.size()) != want) return false;

        // NEITHER OF THESE IS A GATE. The version says which build wrote the key and the count says
        // how many settings it carries; both are reported and neither is compared. What IS checked
        // is that the two agree with the payload actually present -- a length that disagrees with
        // its own header means the token was cut, not that it came from elsewhere.
        const size_t carried = static_cast<size_t>(framed[1])
                             | (static_cast<size_t>(framed[2]) << 8);
        if (framed.size() - 3 != carried * kFieldBytes) return false;

        if (version) *version = framed[0];
        if (fields) *fields = carried;
        out.assign(framed.begin() + 3, framed.end());
        return true;
    }
}
