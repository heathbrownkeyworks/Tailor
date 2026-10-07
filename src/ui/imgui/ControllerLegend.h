#pragma once
#include "Theme.h"
#include "UiState.h"
#include <algorithm>

namespace Tailor::ImGuiUI
{
    struct ControllerHint { std::string binding, label; };
    struct ControllerLegend
    {
        std::string title;
        std::vector<ControllerHint> hints;
        bool playstation = false;
    };

    inline std::string ControllerBinding(const Model& model, const char* action, const char* fallback)
    {
        const auto bindings = model.find("_controllerBindings");
        const std::string physical = bindings != model.end() && bindings->is_object() ? bindings->value(action, std::string(fallback)) : fallback;
        const std::string family = model.value("_controllerGlyphs", std::string("xbox"));
        if (family == "generic") return physical;
        if (family == "playstation") {
            if (physical == "South") return "Cross";
            if (physical == "East") return "Circle";
            if (physical == "West") return "Square";
            if (physical == "North") return "Triangle";
            if (physical == "LeftShoulder") return "L1";
            if (physical == "RightShoulder") return "R1";
            if (physical == "Start") return "Options";
            if (physical == "Back") return "Share";
        } else {
            if (physical == "South") return "A";
            if (physical == "East") return "B";
            if (physical == "West") return "X";
            if (physical == "North") return "Y";
            if (physical == "LeftShoulder") return "LB";
            if (physical == "RightShoulder") return "RB";
            if (physical == "Start") return "Menu";
            if (physical == "Back") return "View";
        }
        if (physical == "LeftThumb") return "L3";
        if (physical == "RightThumb") return "R3";
        return physical;
    }

    inline bool ControllerSymbol(const ControllerLegend& legend, const std::string& binding)
    {
        return legend.playstation && (binding == "Cross" || binding == "Circle" || binding == "Square" || binding == "Triangle");
    }

    // Measure and draw with the same whole-hint wrapping so the row reserves its
    // full height before the preview and NPC nameplate are laid out.
    inline float DrawControllerLegend(const ControllerLegend& legend, const Fonts& fonts, float scale, float width, ImDrawList* draw = nullptr, float top = 0)
    {
        const float padding = 16 * scale, gap = 20 * scale, rowGap = 8 * scale;
        const float size = 14 * scale * FontMetricScale, badgeHeight = size + 6 * scale;
        const float rowHeight = badgeHeight;
        struct Item { float width, badge; const ControllerHint* hint; };
        std::vector<Item> items;
        ImGui::PushFont(fonts.bold, size);
        items.push_back({ImGui::CalcTextSize(legend.title.c_str()).x, 0, nullptr});
        ImGui::PopFont();
        ImGui::PushFont(fonts.body, size);
        for (const auto& hint : legend.hints) {
            const float badge = hint.binding.empty() ? 0 : std::max(badgeHeight, (ControllerSymbol(legend, hint.binding) ? size : ImGui::CalcTextSize(hint.binding.c_str()).x) + 10 * scale);
            items.push_back({badge + (badge ? 8 * scale : 0) + ImGui::CalcTextSize(hint.label.c_str()).x, badge, &hint});
        }
        std::size_t first = 0;
        float y = top + 8 * scale;
        while (first < items.size()) {
            std::size_t end = first;
            float rowWidth = 0;
            while (end < items.size()) {
                const float next = rowWidth + (end != first ? gap : 0) + items[end].width;
                if (end != first && next > width - 2 * padding) break;
                rowWidth = next; ++end;
            }
            float x = std::max(padding, (width - rowWidth) / 2);
            for (auto i = first; i < end; ++i) {
                const auto& item = items[i];
                if (draw) {
                    const float textY = y + (rowHeight - size) / 2;
                    if (!item.hint) draw->AddText(fonts.bold, size, ImVec2(x, textY), IM_COL32(249, 197, 104, 255), legend.title.c_str());
                    else {
                        if (item.badge) {
                            draw->AddRectFilled(ImVec2(x, y), ImVec2(x + item.badge, y + badgeHeight), Palette::Surface, 4 * scale);
                            draw->AddRect(ImVec2(x, y), ImVec2(x + item.badge, y + badgeHeight), IM_COL32(244, 235, 216, 46), 4 * scale);
                            const auto& binding = item.hint->binding;
                            if (ControllerSymbol(legend, binding)) {
                                const ImVec2 c(x + item.badge / 2, y + badgeHeight / 2);
                                const float r = 6 * scale, stroke = 1.7f * scale;
                                if (binding == "Cross") {
                                    draw->AddLine(ImVec2(c.x-r,c.y-r), ImVec2(c.x+r,c.y+r), Palette::Text, stroke);
                                    draw->AddLine(ImVec2(c.x-r,c.y+r), ImVec2(c.x+r,c.y-r), Palette::Text, stroke);
                                } else if (binding == "Circle") draw->AddCircle(c, r, Palette::Text, 20, stroke);
                                else if (binding == "Square") draw->AddRect(ImVec2(c.x-r,c.y-r), ImVec2(c.x+r,c.y+r), Palette::Text, 0, 0, stroke);
                                else draw->AddTriangle(ImVec2(c.x,c.y-r), ImVec2(c.x+r,c.y+r), ImVec2(c.x-r,c.y+r), Palette::Text, stroke);
                            } else draw->AddText(fonts.body, size, ImVec2(x + (item.badge - ImGui::CalcTextSize(binding.c_str()).x) / 2, textY), Palette::Text, binding.c_str());
                        }
                        draw->AddText(fonts.body, size, ImVec2(x + item.badge + (item.badge ? 8 * scale : 0), textY), Palette::Muted, item.hint->label.c_str());
                    }
                }
                x += item.width + gap;
            }
            first = end; y += rowHeight + rowGap;
        }
        ImGui::PopFont();
        return std::max(48 * scale, y - top - rowGap + 8 * scale);
    }
}
