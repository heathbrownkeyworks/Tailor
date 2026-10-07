#pragma once
#include <imgui.h>
#include <string>

namespace Tailor::ImGuiUI
{
    // Measured native font-size adjustment for the bundled Poppins/Montserrat
    // faces, matching the existing HTML captures without changing panel geometry.
    inline constexpr float FontMetricScale = 4.0f / 3.0f;
    struct Fonts
    {
        ImFont* body = nullptr;
        ImFont* medium = nullptr;
        ImFont* bold = nullptr;
        ImFont* heading = nullptr;
    };
    bool LoadFonts(ImGuiIO& io, const std::string& directory, Fonts& fonts);
    void ApplyTheme(float scale);
    // Forge page ambient: amber glow from the top-left, copper from the
    // bottom-right, over the warm page floor. The background is never flat.
    void DrawPageGlow(ImDrawList* draw, ImVec2 origin, ImVec2 size);
    // Forge Design System tokens (tokens/colors.css). Dark only; every dark is
    // warm brown-tinted, every light is parchment.
    namespace Palette
    {
        inline constexpr ImU32 Obsidian = IM_COL32(10, 8, 7, 255);
        inline constexpr ImU32 ObsidianLight = IM_COL32(18, 16, 12, 255);
        inline constexpr ImU32 Floor = IM_COL32(26, 19, 12, 255);
        inline constexpr ImU32 Surface = IM_COL32(28, 24, 19, 255);
        inline constexpr ImU32 Raised = IM_COL32(42, 33, 24, 255);
        inline constexpr ImU32 Hover = IM_COL32(56, 44, 31, 255);
        inline constexpr ImU32 Amber = IM_COL32(245, 158, 11, 255);
        inline constexpr ImU32 AmberLight = IM_COL32(251, 191, 36, 255);
        inline constexpr ImU32 AmberDark = IM_COL32(217, 119, 6, 255);
        inline constexpr ImU32 Copper = IM_COL32(184, 115, 51, 255);
        inline constexpr ImU32 CopperLight = IM_COL32(217, 149, 99, 255);
        inline constexpr ImU32 Emerald = IM_COL32(52, 211, 153, 255);
        inline constexpr ImU32 Text = IM_COL32(244, 235, 216, 255);
        inline constexpr ImU32 Muted = IM_COL32(184, 162, 133, 255);
        inline constexpr ImU32 Faint = IM_COL32(244, 235, 216, 140);
        inline constexpr ImU32 Ghost = IM_COL32(244, 235, 216, 102);
        inline constexpr ImU32 Line1 = IM_COL32(244, 235, 216, 15);
        inline constexpr ImU32 Border = IM_COL32(244, 235, 216, 20);
        inline constexpr ImU32 Line3 = IM_COL32(244, 235, 216, 38);
        inline constexpr ImU32 Fill1 = IM_COL32(244, 235, 216, 8);
        inline constexpr ImU32 Fill2 = IM_COL32(244, 235, 216, 15);
        inline constexpr ImU32 Fill3 = IM_COL32(244, 235, 216, 26);
        inline constexpr ImU32 Danger = IM_COL32(239, 68, 68, 255);
        inline constexpr ImU32 DangerText = IM_COL32(253, 164, 175, 255);
        // Sex tags, from the original court's pills: rose for Female, steel for Male.
        inline constexpr ImU32 Rose = IM_COL32(224, 160, 142, 255);
        inline constexpr ImU32 Steel = IM_COL32(158, 184, 217, 255);
    }
}
