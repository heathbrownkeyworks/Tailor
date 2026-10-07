#include "SettingsScreen.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <imgui_internal.h>

namespace Tailor::ImGuiUI
{
    namespace
    {
        constexpr ImU32 foreground = IM_COL32(244, 235, 216, 255);
        constexpr ImU32 muted = IM_COL32(184, 162, 133, 255);
        constexpr ImU32 amberLight = IM_COL32(251, 191, 36, 255);
        constexpr ImU32 amberDim = IM_COL32(184, 134, 58, 255);
        constexpr ImU32 line = IM_COL32(244, 235, 216, 38);
        constexpr ImU32 amberBorder = IM_COL32(245, 158, 11, 71);
        constexpr ImU32 strongBorder = IM_COL32(245, 158, 11, 107);

        struct Painter
        {
            const Fonts& fonts;
            float scale;
            ImDrawList* draw;

            float Width(const char* text, ImFont* font, float size, float tracking = 0) const
            {
                const auto count = std::strlen(text);
                return font->CalcTextSizeA(size * scale * FontMetricScale, 100000, 0, text).x +
                    (count > 1 ? (count - 1) * tracking * scale : 0);
            }
            void Text(ImVec2 pos, const char* text, ImFont* font, float size, ImU32 color, float tracking = 0) const
            {
                if (!tracking) { draw->AddText(font, size * scale * FontMetricScale, pos, color, text); return; }
                for (const char* ch = text; *ch; ++ch) {
                    draw->AddText(font, size * scale * FontMetricScale, pos, color, ch, ch + 1);
                    pos.x += font->CalcTextSizeA(size * scale * FontMetricScale, 100000, 0, ch, ch + 1).x + tracking * scale;
                }
            }
            void Gear(ImVec2 pos, float size) const
            {
                constexpr ImVec2 points[] = {{9.5f,3},{10,2},{14,2},{14.5f,5},{16.5f,5.9f},{19.1f,4.8f},
                    {21.1f,8.2f},{19,10.1f},{19,13},{21.1f,14.9f},{19.1f,18.3f},{16.5f,17.2f},
                    {14.5f,18.1f},{14,21.1f},{10,21.1f},{9.5f,18.1f},{7.5f,17.2f},{4.9f,18.3f},
                    {2.9f,14.9f},{5,13},{5,10.1f},{2.9f,8.2f},{4.9f,4.8f},{7.5f,5.9f},{9.5f,5}};
                ImVec2 transformed[std::size(points)];
                for (std::size_t i = 0; i < std::size(points); ++i)
                    transformed[i] = {pos.x + points[i].x * size / 24, pos.y + points[i].y * size / 24};
                draw->AddPolyline(transformed, static_cast<int>(std::size(points)), amberLight, ImDrawFlags_Closed, 1.4f * scale);
                draw->AddCircle({pos.x + size / 2, pos.y + size * 11.5f / 24}, size * 3.2f / 24, amberLight, 24, 1.4f * scale);
            }
        };
    }

    bool DrawSettingsScreen(const Fonts& fonts, float scale, const SettingsSwitches& switches, std::optional<SettingsChange>& change)
    {
        const bool compact = ImGui::GetIO().DisplaySize.y <= 800;
        const ImVec2 origin = ImGui::GetWindowPos(), panel = ImGui::GetWindowSize();
        const float padding = (compact ? 28 : 34) * scale;
        const float headerHeight = (compact ? 92 : 153) * scale;
        auto* draw = ImGui::GetWindowDrawList();
        Painter p{fonts, scale, draw};
        const float top = origin.y + (compact ? 12 : 24) * scale;
        ImGui::SetCursorScreenPos({origin.x + padding, top});
        auto* storage = ImGui::GetStateStorage();
        const auto visit = ImGui::GetID("Settings last frame");
        const auto frame = ImGui::GetFrameCount();
        const bool entering = storage->GetInt(visit, -2) != frame - 1;
        storage->SetInt(visit, frame);
        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(0,0,0,0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(0,0,0,0));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(0,0,0,0));
        ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(0,0,0,0));
        const bool back = ImGui::Button("##settingsBack", {58 * scale, 18 * scale});
        if (entering) {
            // Focus the button now; a queued keyboard activation can cancel the next mouse press.
            ImGui::FocusWindow(ImGui::GetCurrentWindow());
            ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
            ImGui::SetNavCursorVisible(true);
        }
        const ImU32 backColor = ImGui::IsItemHovered() || ImGui::IsItemFocused() ? amberLight : muted;
        if (ImGui::IsItemFocused()) draw->AddRect({origin.x+padding-3*scale,top-3*scale},
            {origin.x+padding+61*scale,top+21*scale},amberLight,0,0,2*scale);
        ImGui::PopStyleColor(4);
        const ImVec2 arrow{origin.x + padding, top + scale};
        draw->AddLine({arrow.x + 7 * scale, arrow.y + 3 * scale}, {arrow.x + 2 * scale, arrow.y + 8 * scale}, backColor, 1.2f * scale);
        draw->AddLine({arrow.x + 2 * scale, arrow.y + 8 * scale}, {arrow.x + 7 * scale, arrow.y + 13 * scale}, backColor, 1.2f * scale);
        draw->AddLine({arrow.x + 2 * scale, arrow.y + 8 * scale}, {arrow.x + 14 * scale, arrow.y + 8 * scale}, backColor, 1.2f * scale);
        p.Text({arrow.x + 23 * scale, top}, "Back", fonts.medium, 12, backColor);
        constexpr auto kicker = "TAILOR / PREFERENCES";
        p.Text({origin.x + panel.x - padding - p.Width(kicker, fonts.bold, 12, 2.2f), top + 2 * scale}, kicker, fonts.bold, 12, amberDim, 2.2f);

        const float sealSize = (compact ? 40 : 62) * scale;
        const ImVec2 seal{origin.x + padding, top + (compact ? 28 : 42) * scale};
        const ImVec2 center{seal.x + sealSize / 2, seal.y + sealSize / 2};
        for (int glow=20;glow>0;glow-=2)
            draw->AddCircleFilled(center,sealSize/2+glow*scale,IM_COL32(245,158,11,2),48);
        draw->AddCircleFilled(center, sealSize / 2, IM_COL32(42,33,24,255), 48);
        draw->AddCircle(center, sealSize / 2, strongBorder, 48);
        draw->AddCircle(center, sealSize / 2 - 3 * scale, IM_COL32(28,24,19,255), 48, 5 * scale);
        draw->AddCircle(center, sealSize / 2 - 6 * scale, amberBorder, 48);
        const float gearSize = (compact ? 22 : 29) * scale;
        p.Gear({center.x - gearSize / 2, center.y - gearSize / 2}, gearSize);
        const float titleX = seal.x + sealSize + (compact ? 14 : 20) * scale;
        p.Text({titleX, seal.y + (compact ? -1 : 1) * scale}, "Settings", fonts.heading, compact ? 26.0f : 32.0f, foreground);
        p.Text({titleX, seal.y + (compact ? 28 : 42) * scale}, "Outfits, wigs & NPC appearance.", fonts.body, 13, muted);
        draw->AddLine({origin.x, origin.y + headerHeight}, {origin.x + panel.x, origin.y + headerHeight}, line);

        ImGui::SetCursorScreenPos({origin.x, origin.y + headerHeight});
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0,0});
        ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0,0,0,0));
        ImGui::BeginChild("Settings Scroll", {panel.x, std::max(1.0f, panel.y - headerHeight)}, ImGuiChildFlags_NavFlattened);
        p.draw = ImGui::GetWindowDrawList();
        const ImVec2 start = ImGui::GetCursorScreenPos();
        const float left = start.x + padding, width = ImGui::GetContentRegionAvail().x - 2 * padding;
        float y = start.y + (compact ? 12 : 24) * scale;
        auto section = [&](const char* number, const char* title) {
            p.Text({left, y + scale}, number, fonts.medium, 12, amberDim);
            const float headingX = left + p.Width(number, fonts.medium, 12) + 12 * scale;
            p.Text({headingX, y}, title, fonts.bold, 13, muted, 1.65f);
            const float dividerX = headingX + p.Width(title, fonts.bold, 13, 1.65f) + 12 * scale;
            const float headingHeight = 13 * FontMetricScale * scale;
            p.draw->AddLine({dividerX, y + headingHeight / 2}, {left + width, y + headingHeight / 2}, line);
            y += headingHeight + (compact ? 8 : 11) * scale;
        };
        struct Row { const char* name; bool on; const char* label; const char* detail; const char* help; };
        auto rows = [&](const Row* values, int count) {
            const float rowHeight = (compact ? 52 : 83) * scale;
            const float inset = (compact ? 16 : 20) * scale;
            const float bottom = y + rowHeight * count + 2 * scale;
            p.draw->AddRectFilled({left,y}, {left+width,bottom}, IM_COL32(18,16,12,255), 10*scale);
            p.draw->AddRect({left,y}, {left+width,bottom}, line, 10*scale);
            for (int i = 0; i < count; ++i) {
                const auto& row = values[i];
                const float rowY = y + scale + i * rowHeight;
                if (i) p.draw->AddLine({left,rowY}, {left+width,rowY}, IM_COL32(244,235,216,20));
                const float helpX = left + width - inset - 68 * scale - 16 * scale - 28 * scale;
                ImGui::PushID(row.label);
                // The whole row flips its switch, by mouse or by the controller's Accept; the "?" over it
                // keeps its tip.
                ImGui::SetCursorScreenPos({left, rowY});
                ImGui::SetNextItemAllowOverlap();
                if (ImGui::InvisibleButton("##toggle", {width, rowHeight}, ImGuiButtonFlags_EnableNav)) change = SettingsChange{row.name, !row.on};
                const bool toggleFocused = ImGui::IsItemFocused();
                ImGui::SetCursorScreenPos({helpX, rowY + (rowHeight - 28 * scale) / 2});
                ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 14 * scale);
                ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(0,0,0,0));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(245,158,11,31));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(245,158,11,31));
                ImGui::PushStyleColor(ImGuiCol_Border, line);
                ImGui::Button("##help", {28 * scale,28 * scale});
                const bool help = ImGui::IsItemHovered() || ImGui::IsItemFocused();
                const auto helpPos = ImGui::GetItemRectMin();
                ImGui::PopStyleColor(4); ImGui::PopStyleVar();
                if (ImGui::IsMouseHoveringRect({left,rowY}, {left+width,rowY+rowHeight}) || help || toggleFocused)
                    p.draw->AddRectFilled({left+scale,rowY+scale}, {left+width-scale,rowY+rowHeight-scale}, IM_COL32(244,235,216,8));
                if (toggleFocused)
                    p.draw->AddRect({left+scale,rowY+scale}, {left+width-scale,rowY+rowHeight-scale}, amberLight, 8*scale, 0, 2*scale);
                p.Text({helpPos.x + (28*scale-p.Width("?",fonts.medium,12))/2, helpPos.y+5*scale}, "?", fonts.medium,12,help?amberLight:muted);
                const float labelY = rowY + (compact ? (rowHeight-21*scale)/2 : (rowHeight-41.6f*scale)/2);
                p.Text({left+inset,labelY}, row.label, fonts.medium,14,foreground);
                if (!compact) p.Text({left+inset,labelY+24*scale}, row.detail,fonts.body,13,muted);
                const float trackX = left+width-inset-39*scale, trackY=rowY+(rowHeight-23*scale)/2;
                const char* state = row.on ? "On" : "Off";
                p.Text({trackX-11*scale-p.Width(state,fonts.medium,13),rowY+(rowHeight-13*FontMetricScale*scale)/2},state,fonts.medium,13,row.on?amberLight:muted);
                p.draw->AddRectFilled({trackX,trackY},{trackX+39*scale,trackY+23*scale},row.on?IM_COL32(245,158,11,64):IM_COL32(28,24,19,255),20*scale);
                p.draw->AddRect({trackX,trackY},{trackX+39*scale,trackY+23*scale},row.on?strongBorder:line,20*scale);
                p.draw->AddCircleFilled({trackX+(row.on?27.5f:11.5f)*scale,trackY+11.5f*scale},7.5f*scale,row.on?amberLight:muted,24);
                if (help) {
                    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,{16*scale,13*scale});
                    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding,8*scale);
                    ImGui::PushStyleColor(ImGuiCol_PopupBg,IM_COL32(28,24,19,255));
                    ImGui::PushStyleColor(ImGuiCol_Border,strongBorder);
                    ImGui::PushFont(fonts.body,12*scale*FontMetricScale);
                    const float tooltipWidth=300*scale;
                    const float tooltipHeight=fonts.body->CalcTextSizeA(12*scale*FontMetricScale,100000,
                        tooltipWidth-32*scale,row.help).y+26*scale;
                    const auto display=ImGui::GetIO().DisplaySize;
                    float tooltipY=helpPos.y-tooltipHeight-10*scale;
                    if(tooltipY<12*scale)tooltipY=helpPos.y+38*scale;
                    ImGui::SetNextWindowPos({std::max(12*scale,std::min(helpPos.x+28*scale-tooltipWidth,
                        display.x-tooltipWidth-12*scale)),std::min(tooltipY,display.y-tooltipHeight-12*scale)});
                    ImGui::SetNextWindowSize({300*scale,0});
                    ImGui::BeginTooltip(); ImGui::PushTextWrapPos(0);
                    ImGui::TextUnformatted(row.help);
                    ImGui::PopTextWrapPos(); ImGui::EndTooltip();
                    ImGui::PopFont(); ImGui::PopStyleColor(2); ImGui::PopStyleVar(2);
                }
                ImGui::PopID();
            }
            y = bottom;
        };
        section("01","OUTFIT BEHAVIOR");
        const Row outfits[] = {
            {"disableFavorite", switches.disableFavorite, "Disable Tailor Favorite", "Tailor power in Favorites",
                "Takes the Tailor power out of your Favorites menu and keeps it out, in every save. Turn it off to put it back. On a controller the power is how you open Tailor; it can still be cast from the Magic menu."},
            {"hideWeapons", switches.hideWeapons, "Hide Weapons", "Weapons, shields & arrows",
                "Outside Adventuring and combat, you and NPCs with situations hide weapons, shields and arrows. They show again whenever weapons are drawn. Off: they hide only in Sleep and Swimming looks."},
            {"hideHelmets", switches.hideHelmets, "Hide Helmets", "Helmets & hoods outside adventuring",
                "Outside Adventuring and combat, you and NPCs with situations take off helmets and hoods, and put the same ones back on for adventuring or a fight. Circlets and hoods built into robes stay on."}
        };
        rows(outfits,3);
        ImGui::SetCursorScreenPos({start.x,y+(compact?12:30)*scale});
        ImGui::Dummy({1,1});
        ImGui::EndChild(); ImGui::PopStyleColor(); ImGui::PopStyleVar();
        return back;
    }
}
