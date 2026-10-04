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

#include "wxl-experimental-water/panel/Widgets.hpp"
#include "wxl-experimental-water/panel/Lang.hpp"

#include "imgui.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>

namespace
{
    using namespace wxl::water;

    char g_filter[64] = {};
    int  g_sectionDepth = 0;
    /// Whether the next Legend() starts a row. Reset by EndChart(), so a caller never manages it.
    bool g_legendFirst = true;

    const ImVec4 kModified{ 0.98f, 0.78f, 0.32f, 1.0f };
    const ImVec4 kWarn    { 0.95f, 0.55f, 0.35f, 1.0f };

    bool Matches(const char* haystack, const char* needle)
    {
        if (!haystack || !needle || !*needle) return true;
        for (const char* h = haystack; *h; ++h)
        {
            const char* a = h;
            const char* b = needle;
            while (*a && *b)
            {
                const char ca = (*a >= 'A' && *a <= 'Z') ? static_cast<char>(*a + 32) : *a;
                const char cb = (*b >= 'A' && *b <= 'Z') ? static_cast<char>(*b + 32) : *b;
                if (ca != cb) break;
                ++a; ++b;
            }
            if (!*b) return true;
        }
        return false;
    }

    void Tooltip(const char* text)
    {
        if (!text || !*text) return;
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::BeginItemTooltip())
        {
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
            ImGui::TextUnformatted(text);
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
    }

    /// The label cell: name, ambered when off its default, and the reset on a right-click.
    ///
    /// Right-click rather than a visible button because a reset control on every row is a hundred
    /// buttons competing with the settings for attention, for a thing wanted rarely.
    void LabelCell(const char* label, bool modified, bool* doReset)
    {
        if (modified) ImGui::TextColored(kModified, "%s", label);
        else          ImGui::TextUnformatted(label);

        if (ImGui::BeginPopupContextItem("##rst"))
        {
            if (ImGui::MenuItem(L("panel.reset.item"))) *doReset = true;
            ImGui::EndPopup();
        }
        if (modified && ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", L("panel.modified.tip"));
    }

    bool BeginRow(const char* id)
    {
        ImGui::PushID(id);
        if (!ImGui::BeginTable("##row", 2,
                               ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoSavedSettings))
        {
            ImGui::PopID();
            return false;
        }
        // Fixed label column so a stack of controls lines up regardless of name length.
        ImGui::TableSetupColumn("##l", ImGuiTableColumnFlags_WidthFixed, 168.0f);
        ImGui::TableSetupColumn("##c", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        return true;
    }

    void EndRow()
    {
        ImGui::EndTable();
        ImGui::PopID();
    }

    /// The chart being built. One at a time and not nestable, which is the whole reason it can be a
    /// file-level struct rather than a handle every call has to carry: a chart is a leaf of the
    /// panel, never a container.
    struct Chart
    {
        ImDrawList* dl = nullptr;
        ImVec2 min{ 0.0f, 0.0f };
        ImVec2 max{ 0.0f, 0.0f };
        bool   open = false;

        /// Chart space to screen space. Y is flipped: charts are read with zero at the bottom, and
        /// every caller would otherwise write the inversion itself and one of them would forget.
        ImVec2 At(float x, float y) const
        {
            return ImVec2(min.x + (max.x - min.x) * x, max.y - (max.y - min.y) * y);
        }
    };
    Chart g_chart;
}

namespace wxl::water::ui
{
    bool BeginChart(const char* id, float height)
    {
        // Floored rather than skipped: a zero-width item is an assert in ImGui, and the available
        // width passes through zero while a window is being dragged narrow.
        const float avail = ImGui::GetContentRegionAvail().x;
        if (avail < 48.0f) { g_chart.open = false; return false; }

        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        const ImVec2 size(avail, height);
        ImGui::InvisibleButton(id, size);

        g_chart.dl = ImGui::GetWindowDrawList();
        g_chart.min = ImVec2(p0.x, p0.y);
        g_chart.max = ImVec2(p0.x + size.x, p0.y + size.y);
        g_chart.open = true;

        g_chart.dl->AddRectFilled(g_chart.min, g_chart.max, IM_COL32(16, 18, 24, 255));
        // Clipped for the whole body: a curve is sampled from a function nobody has bounded, and a
        // single sample off the top would otherwise paint a line across the panel behind it.
        g_chart.dl->PushClipRect(g_chart.min, g_chart.max, true);
        return true;
    }

    void EndChart()
    {
        if (!g_chart.open) return;
        g_chart.dl->PopClipRect();
        g_chart.dl->AddRect(g_chart.min, g_chart.max, IM_COL32(90, 95, 110, 255));
        g_chart.open = false;
        g_legendFirst = true;
    }

    void Legend(unsigned rgba, const char* key)
    {
        const char* text = L(key);
        const float sq = ImGui::GetFontSize() * 0.70f;
        const float need = sq + 5.0f + ImGui::CalcTextSize(text).x + 16.0f;

        // Wrapped rather than clipped or scrolled: a legend that runs off the edge is a legend with
        // an entry nobody can read, and the entry that goes missing is the last one -- which is the
        // one a reader is looking for when they have already recognised the others.
        if (!g_legendFirst && ImGui::GetContentRegionAvail().x > need)
            ImGui::SameLine(0.0f, 16.0f);
        g_legendFirst = false;

        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float y = p.y + (ImGui::GetTextLineHeight() - sq) * 0.5f;
        dl->AddRectFilled(ImVec2(p.x, y), ImVec2(p.x + sq, y + sq), rgba);
        // Outlined, because several of these are faint by design -- a 40-alpha reference line is
        // invisible as a bare swatch against the panel behind it.
        dl->AddRect(ImVec2(p.x, y), ImVec2(p.x + sq, y + sq), IM_COL32(220, 225, 235, 90));

        ImGui::Dummy(ImVec2(sq + 5.0f, ImGui::GetTextLineHeight()));
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::TextDisabled("%s", text);
    }

    void ChartBars(const int* counts, int n)
    {
        if (!g_chart.open || !counts || n <= 0) return;

        int peak = 1;
        for (int i = 0; i < n; ++i) if (counts[i] > peak) peak = counts[i];

        for (int i = 0; i < n; ++i)
        {
            if (counts[i] <= 0) continue;
            // Square root, not linear. One bucket usually holds most of a view, and against that
            // peak every other bucket renders as a line one pixel tall -- which reads as "there is
            // no data here" when what it means is "there is less data here".
            const float h = std::sqrt(static_cast<float>(counts[i]) / static_cast<float>(peak));
            const float x0 = static_cast<float>(i) / static_cast<float>(n);
            const float x1 = static_cast<float>(i + 1) / static_cast<float>(n);
            g_chart.dl->AddRectFilled(g_chart.At(x0, h), g_chart.At(x1, 0.0f),
                                      IM_COL32(64, 76, 96, 120));
        }
    }

    void ChartCurve(const float* ys, int n, unsigned rgba, float thickness)
    {
        if (!g_chart.open || !ys || n < 2) return;

        // Chunked rather than allocated: a curve is sampled per pixel column and this runs every
        // frame for every open chart, so a fixed buffer walked in overlapping runs costs nothing
        // and cannot fail. The overlap is what keeps the joins invisible.
        constexpr int kRun = 256;
        ImVec2 pts[kRun];
        int i = 0;
        while (i < n - 1)
        {
            int count = 0;
            while (count < kRun && i + count < n)
            {
                const float t = static_cast<float>(i + count) / static_cast<float>(n - 1);
                pts[count] = g_chart.At(t, ys[i + count]);
                ++count;
            }
            g_chart.dl->AddPolyline(pts, count, rgba, 0, thickness);
            if (count < kRun) break;
            i += count - 1;
        }
    }

    void ChartVLine(float x, unsigned rgba)
    {
        // Dropped rather than clamped when it falls outside: a marker pinned to the border reads as
        // a real value sitting there, which is a wrong answer where none was needed.
        if (!g_chart.open || x < 0.0f || x > 1.0f) return;
        g_chart.dl->AddLine(g_chart.At(x, 0.0f), g_chart.At(x, 1.0f), rgba);
    }

    void ChartHLine(float y, unsigned rgba)
    {
        if (!g_chart.open || y < 0.0f || y > 1.0f) return;
        g_chart.dl->AddLine(g_chart.At(0.0f, y), g_chart.At(1.0f, y), rgba);
    }

    void ChartRect(float x0, float y0, float x1, float y1, unsigned rgba)
    {
        if (!g_chart.open) return;
        g_chart.dl->AddRectFilled(g_chart.At(x0, y1), g_chart.At(x1, y0), rgba);
    }

    void ChartText(float x, float y, unsigned rgba, const char* fmt, ...)
    {
        if (!g_chart.open) return;

        char buf[128];
        va_list args;
        va_start(args, fmt);
        std::vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);

        ImVec2 at = g_chart.At(x, y);
        // Nudged inward at the right edge, so a label belonging to the last marker on the axis is
        // still readable instead of being clipped away by the frame it labels.
        const float w = ImGui::CalcTextSize(buf).x;
        if (at.x + w > g_chart.max.x - 2.0f) at.x = g_chart.max.x - w - 2.0f;
        if (at.x < g_chart.min.x + 2.0f) at.x = g_chart.min.x + 2.0f;
        g_chart.dl->AddText(at, rgba, buf);
    }

    const char* Text(const char* key, const char* suffix)
    {
        static char buf[96];
        std::snprintf(buf, sizeof(buf), "%s.%s", key ? key : "", suffix);
        return L(buf);
    }

    void NewFrame() { g_sectionDepth = 0; }

    void FilterBox()
    {
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##filter", L("panel.filter.hint"), g_filter, sizeof(g_filter));
    }

    bool Passes(const char* key)
    {
        if (!g_filter[0]) return true;
        // The resolved label AND the key: searching for a key is how somebody chasing a missing
        // string finds the control it belongs to.
        return Matches(Text(key, "label"), g_filter) || Matches(key, g_filter);
    }

    bool Section(const char* key, bool defaultOpen)
    {
        const char* title = Text(key, "label");
        ImGui::PushID(key);
        const bool open = ImGui::CollapsingHeader(title, defaultOpen ? ImGuiTreeNodeFlags_DefaultOpen : 0);
        if (!open) ImGui::PopID();
        else ++g_sectionDepth;
        return open;
    }

    void EndSection()
    {
        if (g_sectionDepth > 0) { --g_sectionDepth; ImGui::PopID(); }
        ImGui::Spacing();
    }

    bool Field(const char* key, float* v, float lo, float hi, float def,
               const char* unit, const char* fmt)
    {
        if (!v || !Passes(key)) return false;
        // Resolved before the row: Text() reuses one buffer, so holding two of its results at once
        // would give the same string twice.
        char label[96];
        std::snprintf(label, sizeof(label), "%s", Text(key, "label"));

        bool changed = false, reset = false;
        if (!BeginRow(key)) return false;

        // A tolerance rather than an exact compare: these are floats round-tripped through a text
        // file, and a default that survived a save and a load is not bit-identical to its literal.
        const bool modified = std::fabs(*v - def) > std::fabs(def) * 1e-4f + 1e-6f;
        LabelCell(label, modified, &reset);

        ImGui::TableSetColumnIndex(1);
        const float avail = ImGui::GetContentRegionAvail().x;
        const float inputW = 74.0f;
        const float unitW = (unit && *unit) ? 34.0f : 0.0f;

        ImGui::SetNextItemWidth(avail - inputW - unitW - 42.0f);
        // AlwaysClamp on the slider only. The typed box takes anything on purpose.
        changed |= ImGui::SliderFloat("##s", v, lo, hi, fmt, ImGuiSliderFlags_AlwaysClamp);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(inputW);
        changed |= ImGui::InputFloat("##i", v, 0.0f, 0.0f, fmt);
        if (unitW > 0.0f) { ImGui::SameLine(); ImGui::TextDisabled("%s", unit); }
        Tooltip(Text(key, "help"));
        EndRow();

        if (reset) { *v = def; changed = true; }
        return changed;
    }

    bool FieldInt(const char* key, int* v, int lo, int hi, int def, const char* unit)
    {
        if (!v || !Passes(key)) return false;
        char label[96];
        std::snprintf(label, sizeof(label), "%s", Text(key, "label"));

        bool changed = false, reset = false;
        if (!BeginRow(key)) return false;
        LabelCell(label, *v != def, &reset);

        ImGui::TableSetColumnIndex(1);
        const float unitW = (unit && *unit) ? 34.0f : 0.0f;
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - unitW - 42.0f);
        changed |= ImGui::SliderInt("##s", v, lo, hi, "%d", ImGuiSliderFlags_AlwaysClamp);
        if (unitW > 0.0f) { ImGui::SameLine(); ImGui::TextDisabled("%s", unit); }
        Tooltip(Text(key, "help"));
        EndRow();

        if (reset) { *v = def; changed = true; }
        return changed;
    }

    bool Heading(const char* key, float* x, float* y)
    {
        if (!x || !y || !Passes(key)) return false;
        constexpr float kDeg = 57.29577951f;
        constexpr float kRad = 0.017453293f;

        char label[96];
        std::snprintf(label, sizeof(label), "%s", Text(key, "label"));
        float deg = std::atan2(*y, *x) * kDeg;
        bool reset = false;

        if (!BeginRow(key)) return false;
        LabelCell(label, false, &reset);

        ImGui::TableSetColumnIndex(1);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 42.0f);
        // Dragged rather than clamped at the ends: a heading is an angle, and stopping at 180 means
        // dragging back across the whole range to cross north.
        const bool changed = ImGui::DragFloat("##d", &deg, 0.5f, -180.0f, 180.0f, "%.1f deg");
        Tooltip(Text(key, "help"));
        EndRow();

        if (changed)
        {
            if (deg > 180.0f) deg -= 360.0f;
            if (deg < -180.0f) deg += 360.0f;
            *x = std::cos(deg * kRad);
            *y = std::sin(deg * kRad);
        }
        return changed;
    }

    bool Color3(const char* key, float* rgb, const float* def)
    {
        if (!rgb || !Passes(key)) return false;
        char label[96];
        std::snprintf(label, sizeof(label), "%s", Text(key, "label"));

        bool reset = false;
        if (!BeginRow(key)) return false;

        bool modified = false;
        if (def)
            for (int i = 0; i < 3; ++i)
                modified = modified || std::fabs(rgb[i] - def[i]) > 1e-4f;
        LabelCell(label, modified, &reset);

        ImGui::TableSetColumnIndex(1);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 42.0f);
        // Float rather than 8-bit: several of these are multiplied before they are displayed, and
        // quantising the control to 256 steps puts banding into a value that had none.
        const bool changed = ImGui::ColorEdit3("##c", rgb,
                                               ImGuiColorEditFlags_Float
                                             | ImGuiColorEditFlags_PickerHueWheel
                                             | ImGuiColorEditFlags_NoInputs);
        Tooltip(Text(key, "help"));
        EndRow();

        if (reset && def) { for (int i = 0; i < 3; ++i) rgb[i] = def[i]; return true; }
        return changed;
    }

    bool Extinction(const char* key, float* ext3, float* maxPath)
    {
        if (!ext3 || !maxPath || !Passes(key)) return false;

        // The coefficients say how fast each channel is removed per unit. Read back as the colour
        // surviving a reference path, which is a colour somebody can look at and judge.
        constexpr float kRef = 8.0f;
        float tint[3];
        for (int i = 0; i < 3; ++i) tint[i] = std::exp(-ext3[i] * kRef);

        bool changed = false;
        char label[96];
        std::snprintf(label, sizeof(label), "%s", Text(key, "label"));

        if (ImGui::TreeNodeEx(label, ImGuiTreeNodeFlags_DefaultOpen))
        {
            Note("ext.note");
            if (BeginRow("exttint"))
            {
                ImGui::TextUnformatted(L("ext.tint.label"));
                ImGui::TableSetColumnIndex(1);
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 42.0f);
                if (ImGui::ColorEdit3("##t", tint,
                                      ImGuiColorEditFlags_Float | ImGuiColorEditFlags_PickerHueWheel
                                    | ImGuiColorEditFlags_NoInputs))
                {
                    for (int i = 0; i < 3; ++i)
                    {
                        // Clamped away from both ends: zero is an infinite coefficient and one is a
                        // channel that never attenuates, and the picker can reach both.
                        const float t = (tint[i] < 0.001f) ? 0.001f
                                                           : ((tint[i] > 0.999f) ? 0.999f : tint[i]);
                        ext3[i] = -std::log(t) / kRef;
                    }
                    changed = true;
                }
                Tooltip(Text(key, "help"));
                EndRow();
            }
            changed |= Field("ext.path", maxPath, 20.0f, 600.0f, 180.0f, "u", "%.0f");
            ImGui::TreePop();
        }
        return changed;
    }

    bool Switch(Sw s)
    {
        const char* label = SwitchLabel(s);
        if (g_filter[0] && !Matches(label, g_filter) && !Matches(SwitchId(s), g_filter))
            return false;
        const bool changed = ImGui::Checkbox(label, Slot(s));
        Tooltip(SwitchHelp(s));
        return changed;
    }

    void Note(const char* key)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(L(key));
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
    }

    void Readout(const char* fmt, ...)
    {
        char buf[512];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        ImGui::TextDisabled("%s", buf);
    }

    void Warn(const char* fmt, ...)
    {
        char buf[512];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        ImGui::TextColored(kWarn, "%s", buf);
    }

    void Help(const char* key) { Tooltip(Text(key, "help")); }
}
