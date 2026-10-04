// Panel strings, by key, from an editable file.
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

#include "wxl-experimental-water/panel/Lang.hpp"
#include "wxl-experimental-water/panel/Json.hpp"

#include "common/Log.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace
{
    using wxl::water::Lang;

    constexpr const char* kPath = "wxl-water-lang.json";

    struct Entry
    {
        const char* key = nullptr;
        const char* en  = nullptr;
        const char* fr  = nullptr;
    };

    /// The file itself, kept alive because every string in the table points into it.
    ///
    /// A vector and not a string, and that is not a style preference. std::string may store short
    /// contents INSIDE the object, in which case moving it copies the bytes to a new address and
    /// every pointer the table holds into it dangles. A vector's move always transfers the heap
    /// block, so the addresses the parser handed out stay the addresses the table reads.
    std::vector<char>  g_buffer;
    std::vector<Entry> g_table;      ///< sorted by key, for a binary search
    Lang               g_lang = Lang::English;

    /// Keys the code asked for that the file has never heard of. Bounded and deduplicated by
    /// POINTER rather than by content: every key is a string literal in the panel's source, so the
    /// same key is always the same address, and comparing addresses makes this free enough to run
    /// on a lookup that happens hundreds of times a frame.
    constexpr int kMaxUnknown = 64;
    const char*   g_unknown[kMaxUnknown] = {};
    int           g_unknownCount = 0;

    void NoteUnknown(const char* key)
    {
        for (int i = 0; i < g_unknownCount; ++i)
            if (g_unknown[i] == key) return;
        if (g_unknownCount < kMaxUnknown) g_unknown[g_unknownCount++] = key;
    }

    /// Accumulated during the parse, before the table is sorted.
    struct Leaf { const char* key; const char* lang; const char* value; };
    std::vector<Leaf> g_leaves;

    void OnLeaf(void*, const char* key, const char* lang, const char* value)
    {
        g_leaves.push_back(Leaf{ key, lang, value });
    }

    const Entry* Find(const char* key)
    {
        if (!key || g_table.empty()) return nullptr;
        int lo = 0, hi = static_cast<int>(g_table.size()) - 1;
        while (lo <= hi)
        {
            const int mid = (lo + hi) / 2;
            const int c = std::strcmp(g_table[mid].key, key);
            if (c == 0) return &g_table[mid];
            if (c < 0) lo = mid + 1; else hi = mid - 1;
        }
        return nullptr;
    }
}

namespace wxl::water
{
    Lang Language() { return g_lang; }
    void SetLanguage(Lang lang) { g_lang = lang; }

    const char* L(const char* key)
    {
        if (!key || !*key) return "";
        const Entry* e = Find(key);
        if (!e) { NoteUnknown(key); return key; }

        const char* want = (g_lang == Lang::French) ? e->fr : e->en;
        if (want && *want) return want;
        // Falling back to English is the right answer for a translation in progress. Falling back
        // to the KEY when even English is absent is the right answer for an entry that exists but
        // says nothing -- an empty label is a control nobody can identify.
        if (e->en && *e->en) return e->en;
        return key;
    }

    int MissingCount(Lang lang)
    {
        int n = 0;
        for (const Entry& e : g_table)
        {
            const char* s = (lang == Lang::French) ? e.fr : e.en;
            if (!s || !*s) ++n;
        }
        return n;
    }

    int UnknownCount() { return g_unknownCount; }

    bool LoadStrings()
    {
        std::FILE* fp = std::fopen(kPath, "rb");
        if (!fp)
        {
            WLOG_WARN("water: no string file at %s -- the panel will show its keys", kPath);
            return false;
        }

        std::fseek(fp, 0, SEEK_END);
        const long len = std::ftell(fp);
        std::fseek(fp, 0, SEEK_SET);
        if (len <= 0 || len > (8 << 20)) { std::fclose(fp); return false; }

        std::vector<char> text(static_cast<size_t>(len) + 1);
        const size_t got = std::fread(text.data(), 1, static_cast<size_t>(len), fp);
        std::fclose(fp);
        text[got] = '\0';

        // A BOM would otherwise be the first token and fail the parse on line one, which is a
        // baffling error to hit on a file that looks correct in every editor that wrote it.
        size_t begin = 0;
        if (got >= 3 && static_cast<unsigned char>(text[0]) == 0xEF
                     && static_cast<unsigned char>(text[1]) == 0xBB
                     && static_cast<unsigned char>(text[2]) == 0xBF)
            begin = 3;

        g_leaves.clear();
        int errLine = 0;
        const bool ok = json::ParseInPlace(text.data() + begin, &OnLeaf, nullptr, &errLine);
        if (!ok)
        {
            // The previous table survives: a panel showing raw keys is a worse outcome than a
            // stale word, and a typo in the file should not cost the whole panel.
            WLOG_WARN("water: string file syntax error at %s line %d -- keeping the %d strings "
                      "already loaded", kPath, errLine, static_cast<int>(g_table.size()));
            return false;
        }

        // Committed only now, and both together: the table points into the buffer, so replacing one
        // without the other leaves every string in the panel pointing at freed memory.
        g_buffer = std::move(text);
        g_table.clear();
        g_table.reserve(g_leaves.size() / 2 + 1);

        std::sort(g_leaves.begin(), g_leaves.end(),
                  [](const Leaf& a, const Leaf& b) { return std::strcmp(a.key, b.key) < 0; });

        for (size_t i = 0; i < g_leaves.size(); )
        {
            Entry e;
            e.key = g_leaves[i].key;
            size_t j = i;
            while (j < g_leaves.size() && std::strcmp(g_leaves[j].key, e.key) == 0)
            {
                if (std::strcmp(g_leaves[j].lang, "en") == 0) e.en = g_leaves[j].value;
                else if (std::strcmp(g_leaves[j].lang, "fr") == 0) e.fr = g_leaves[j].value;
                ++j;
            }
            g_table.push_back(e);
            i = j;
        }
        g_leaves.clear();

        // Asking for the keys again is what makes a key that was missing and has since been added
        // stop being reported. Cheap, and the alternative is a count that only ever grows.
        g_unknownCount = 0;

        const int missEn = MissingCount(Lang::English);
        const int missFr = MissingCount(Lang::French);
        if (missEn || missFr)
            WLOG_WARN("water: strings loaded from %s -- %d keys, MISSING %d en / %d fr",
                      kPath, static_cast<int>(g_table.size()), missEn, missFr);
        else
            WLOG_INFO("water: strings loaded from %s -- %d keys, both languages complete",
                      kPath, static_cast<int>(g_table.size()));
        return true;
    }
}
