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

#pragma once

// WHY A KEY TABLE AFTER ALL.
//
// This module argued the other way once, and at the time it was right: with twenty strings, both
// translations at the call site cannot drift apart and cost nothing. The panel now carries roughly
// four hundred, and the argument inverts -- inline pairs became the bulk of the panel's source, and
// nobody who does not compile the project could touch a word of it.
//
// THE FAILURE MODE OF A KEY TABLE IS SILENT DRIFT, and that is what the design has to answer rather
// than accept:
//
//   * a missing key renders AS THE KEY. Not blank, not the other language -- a control labelled
//     "field.foam.density.label" is impossible to miss and impossible to mistake for finished text.
//   * loading COMPARES the languages and reports what each one is missing, to the log and to the
//     panel. Drift is a number somebody sees, not a discovery somebody makes.
//
// The file lives beside the client and reloads on demand, so a wording change is an edit and a
// click rather than a rebuild.
namespace wxl::water
{
    enum class Lang { English, French };

    Lang Language();
    void SetLanguage(Lang lang);

    /// The string for @p key in the current language.
    ///
    /// Falls back to English, then to the key itself. Never returns null and never returns empty:
    /// an empty label is a control nobody can identify, which is worse than a visible key.
    const char* L(const char* key);

    /// Reads the string file. Safe to call repeatedly -- the table is replaced, not merged, so a
    /// key deleted from the file disappears from the panel on the next reload.
    ///
    /// @return false if the file is unreadable or malformed. The previous table survives a failed
    ///         reload, because a panel showing raw keys is a worse outcome than a stale word.
    bool LoadStrings();

    /// How many keys the file defines no translation for in @p lang.
    int MissingCount(Lang lang);

    /// Keys asked for at least once and not found at all, this session. This is the other half of
    /// the drift report: MissingCount catches a key the file knows about in one language only, this
    /// catches one the file has never heard of.
    int UnknownCount();
}
