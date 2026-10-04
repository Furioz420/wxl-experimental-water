// The panel's widget vocabulary: one way to present a setting, used everywhere.
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

#include "wxl-experimental-water/panel/Switches.hpp"

// A panel with a hundred controls is only usable if every control behaves identically. These are
// the properties every setting gets for free, because they live in the widget and not at the call
// site:
//
//   * ITS DEFAULT IS VISIBLE. A modified field is marked and resets from its own context menu.
//   * ITS UNIT IS STATED. Half these numbers are world units, some seconds, some pure ratios.
//   * IT IS SEARCHABLE. The filter box hides everything that does not match.
//   * IT IS BOUNDED FOR DRAGGING AND UNBOUNDED FOR TYPING. The slider range is a judgement about
//     what is useful; finding out that judgement was wrong means being able to exceed it.
//
// EVERY CONTROL IS ADDRESSED BY KEY. A key like "detail.cell" resolves to "detail.cell.label" and
// "detail.cell.help" in the string file. A key with no entry renders as itself, so a gap is loud.
namespace wxl::water::ui
{
    /// Per-frame reset. Call once before anything else in the panel body.
    void NewFrame();

    /// Draws the filter box. Everything after it is subject to whatever is typed in.
    void FilterBox();

    /// Whether a control passes the current filter. Tests the resolved label, so a search works in
    /// whichever language the panel is showing.
    bool Passes(const char* key);

    /// A collapsible group. Returns false when collapsed, in which case the caller skips its body.
    /// @param defaultOpen false for a group that is shape rather than intent -- open on demand,
    ///        so the controls somebody actually reaches for are not below a screenful of the ones
    ///        they set once. The state itself persists in the panel ini after the first click.
    bool Section(const char* key, bool defaultOpen = true);
    void EndSection();

    /// One scalar setting: label, drag, typed input, unit, default marker, reset, help.
    ///
    /// @param def  the value the field returns to, and what decides whether it shows as modified.
    ///             It must be the SAME literal the struct initialises with -- a default that
    ///             disagrees marks an untouched panel as edited everywhere.
    /// @param unit shown dimmed after the input. Not localised: "u", "s", "deg", "%" read the same.
    bool Field(const char* key, float* v, float lo, float hi, float def,
               const char* unit, const char* fmt);

    bool FieldInt(const char* key, int* v, int lo, int hi, int def, const char* unit);

    /// An angle, edited in degrees and stored as a unit vector.
    bool Heading(const char* key, float* x, float* y);

    /// A colour, on the full picker rather than three sliders.
    bool Color3(const char* key, float* rgb, const float* def);

    /// Extinction edited as a TINT and a DISTANCE rather than as three attenuation coefficients.
    ///
    /// The three raw numbers are the physical form and are unusable as a control: they are inverse
    /// lengths, they interact multiplicatively with the path, and the colour they produce is not
    /// the colour they look like. This asks for the colour deep water reaches and how far it takes
    /// to get there, and solves for the coefficients.
    bool Extinction(const char* key, float* ext3, float* maxPath);

    /// A switch from the registry, with its label and description.
    bool Switch(Sw s);

    /// An inline note: dimmed and wrapped, for a statement about a section rather than a control.
    void Note(const char* key);

    /// A measured value the panel reports rather than controls.
    void Readout(const char* fmt, ...);
    /// Same, drawn as a warning.
    void Warn(const char* fmt, ...);

    /// The (?) marker and its tooltip, for a caller not using Field().
    void Help(const char* key);

    /// Resolves "<key>.<suffix>" through the string table. Exposed because a few call sites build
    /// a label out of a loop index rather than from a literal.
    const char* Text(const char* key, const char* suffix);

    // ---- charts ---------------------------------------------------------------------------------
    //
    // A NUMBER IN A PANEL IS A CLAIM; A CHART IS THE EVIDENCE. Half the settings in this module are
    // curves over a quantity nobody can see from inside the game -- depth, wavelength, distance from
    // the eye, path length through water. Every one of them was being tuned by dragging and
    // guessing, and the shore tab proved what the fix is: draw the function, and draw where the
    // map's own data actually falls on it. The second half is what turns a picture into an answer.
    //
    // Coordinates are NORMALISED, (0,0) bottom-left to (1,1) top-right, so a caller builds arrays in
    // its own units and never touches screen space. Everything is clipped to the frame, and a value
    // outside [0,1] is dropped rather than pinned to the edge -- a marker sitting on the border
    // reads as a real value there, which is worse than not drawing it.

    /// Reserves the space and draws the frame. Returns false if the window is too narrow to bother,
    /// in which case NO End is needed and nothing else may be called.
    bool BeginChart(const char* id, float height);
    void EndChart();

    /// The background distribution: where the data this chart is about actually lies. Scaled by its
    /// own peak, square-rooted, because one bucket usually holds most of a view and a linear bar
    /// chart renders every other bucket as a line one pixel tall.
    void ChartBars(const int* counts, int n);

    /// A curve of `n` samples spread evenly across the width. `ys` are already normalised.
    void ChartCurve(const float* ys, int n, unsigned rgba, float thickness);

    void ChartVLine(float x, unsigned rgba);
    void ChartHLine(float y, unsigned rgba);
    /// A filled rectangle in chart space -- a band, a swatch, a bar.
    void ChartRect(float x0, float y0, float x1, float y1, unsigned rgba);
    /// Text pinned at a point, drawn over everything. Nudged inward at the right edge so a label
    /// near the end of the axis stays inside the frame.
    void ChartText(float x, float y, unsigned rgba, const char* fmt, ...);

    /// ONE SWATCH, ONE MEANING. Call once per colour after EndChart().
    ///
    /// This replaces a paragraph naming the colours in prose, which was the wrong shape for the
    /// job twice over: it made the reader hold a colour in their head while scanning a sentence for
    /// it, and it went stale the moment a curve was recoloured, silently, because nothing connects
    /// a sentence to the constant it describes. A swatch is drawn from the SAME value the curve is,
    /// so the two cannot disagree.
    ///
    /// Laid out left to right and wrapped when the row runs out; the run resets at each EndChart().
    void Legend(unsigned rgba, const char* key);
}
