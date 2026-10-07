#include "Theme.h"
#include <algorithm>
#include <cmath>
#include <filesystem>

namespace Tailor::ImGuiUI
{
    bool LoadFonts(ImGuiIO& io, const std::string& directory, Fonts& fonts)
    {
        auto load = [&](const char* file, float size) -> ImFont* {
            auto path = std::filesystem::path(directory) / file;
            if (!std::filesystem::is_regular_file(path)) return nullptr;
            return io.Fonts->AddFontFromFileTTF(path.string().c_str(), size);
        };
        fonts.body = load("Poppins-Regular.ttf", 14.0f);
        fonts.medium = load("Poppins-Medium.ttf", 14.0f);
        fonts.bold = load("Poppins-SemiBold.ttf", 16.0f);
        fonts.heading = load("Montserrat-Black.ttf", 18.0f);
        const bool complete = fonts.body && fonts.medium && fonts.bold && fonts.heading;
        if (!fonts.body) fonts.body = io.Fonts->AddFontDefault();
        if (!fonts.medium) fonts.medium = fonts.body;
        if (!fonts.bold) fonts.bold = fonts.body;
        if (!fonts.heading) fonts.heading = fonts.body;
        io.FontDefault = fonts.body;
        return complete;
    }

    void ApplyTheme(float scale)
    {
        ImGuiStyle style;
        // Forge radii ladder: 8 controls, 10 cards and popovers, 18 modals.
        style.WindowRounding = 0;
        style.ChildRounding = 10;
        style.FrameRounding = 8;
        style.PopupRounding = 10;
        style.GrabRounding = 6;
        style.ScrollbarRounding = 8;
        style.WindowBorderSize = 0;
        style.ChildBorderSize = 1;
        style.FrameBorderSize = 1;
        style.PopupBorderSize = 1;
        style.WindowPadding = ImVec2(24, 20);
        // Inputs, selects and buttons share one 40px control height so rows align.
        style.FramePadding = ImVec2(14, (40 - 14 * FontMetricScale) / 2);
        style.ItemSpacing = ImVec2(10, 10);
        style.ItemInnerSpacing = ImVec2(8, 6);
        // A parchment grab on a faint track: scrolling must be visible at couch distance.
        style.ScrollbarSize = 12;
        style.ScrollbarPadding = 3;
        style.GrabMinSize = 20;
        style.DisabledAlpha = 0.45f;
        style.SelectableTextAlign = ImVec2(0, 0.5f);
        auto set = [&](ImGuiCol index, ImU32 color) { style.Colors[index] = ImGui::ColorConvertU32ToFloat4(color); };
        set(ImGuiCol_Text, Palette::Text);
        set(ImGuiCol_TextDisabled, Palette::Ghost);
        set(ImGuiCol_WindowBg, Palette::Floor);
        set(ImGuiCol_ChildBg, IM_COL32(10, 8, 7, 90));
        set(ImGuiCol_PopupBg, Palette::Surface);
        set(ImGuiCol_ModalWindowDimBg, IM_COL32(5, 4, 2, 179));
        set(ImGuiCol_Border, Palette::Fill3);
        set(ImGuiCol_FrameBg, IM_COL32(244, 235, 216, 10));
        set(ImGuiCol_FrameBgHovered, Palette::Fill2);
        set(ImGuiCol_FrameBgActive, Palette::Fill2);
        set(ImGuiCol_Button, Palette::Fill1);
        set(ImGuiCol_ButtonHovered, Palette::Fill2);
        set(ImGuiCol_ButtonActive, Palette::Fill3);
        set(ImGuiCol_Header, IM_COL32(245, 158, 11, 31));
        set(ImGuiCol_HeaderHovered, Palette::Fill2);
        set(ImGuiCol_HeaderActive, Palette::Fill3);
        set(ImGuiCol_CheckMark, Palette::Amber);
        set(ImGuiCol_SliderGrab, Palette::Copper);
        set(ImGuiCol_SliderGrabActive, Palette::Amber);
        set(ImGuiCol_Separator, Palette::Border);
        set(ImGuiCol_SeparatorHovered, Palette::Amber);
        set(ImGuiCol_NavCursor, Palette::Amber);
        set(ImGuiCol_InputTextCursor, Palette::AmberLight);
        set(ImGuiCol_ScrollbarBg, Palette::Fill1);
        set(ImGuiCol_ScrollbarGrab, IM_COL32(244, 235, 216, 56));
        set(ImGuiCol_ScrollbarGrabHovered, Palette::Copper);
        set(ImGuiCol_ScrollbarGrabActive, Palette::Amber);
        set(ImGuiCol_TextSelectedBg, IM_COL32(245, 158, 11, 64));
        style.ScaleAllSizes(scale);
        ImGui::GetStyle() = style;
    }

    void DrawPageGlow(ImDrawList* draw, ImVec2 origin, ImVec2 size)
    {
        // The two radial CSS page-glow layers over the solid page floor.
        auto color = [&](float x, float y) {
            const float a = .09f * std::max(0.0f, 1.0f - std::hypot((x - .15f) / 1.20208f, (y + .1f) / 1.55563f) / .55f);
            const float b = .16f * std::max(0.0f, 1.0f - std::hypot((x - 1) / 1.41421f, (y - 1.1f) / 1.55563f) / .55f);
            auto channel = [&](int floor, int copper, int amber) { return static_cast<int>((floor * (1 - b) + copper * b) * (1 - a) + amber * a); };
            return IM_COL32(channel(26,184,245), channel(19,115,158), channel(12,51,11), 255);
        };
        constexpr int columns = 16, rows = 20;
        for (int y = 0; y < rows; ++y) for (int x = 0; x < columns; ++x) {
            const float x0 = float(x) / columns, x1 = float(x + 1) / columns;
            const float y0 = float(y) / rows, y1 = float(y + 1) / rows;
            draw->AddRectFilledMultiColor({origin.x + size.x * x0, origin.y + size.y * y0},
                {origin.x + size.x * x1, origin.y + size.y * y1}, color(x0,y0), color(x1,y0), color(x1,y1), color(x0,y1));
        }
    }
}
