#include "Theme.h"
#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <vector>

namespace Tailor::ImGuiUI
{
    namespace
    {
        std::filesystem::path WindowsFonts()
        {
            wchar_t windows[MAX_PATH]{};
            const auto length = GetWindowsDirectoryW(windows, MAX_PATH);
            if (length == 0 || length >= MAX_PATH) return {};
            return std::filesystem::path(windows) / L"Fonts";
        }

        // The first of these system fonts that exists, read once for the whole process: every Tailor font shares it,
        // and the atlas reads glyphs from it as they are first drawn, so it must outlive every atlas. Empty when none
        // exists.
        std::vector<unsigned char>& SystemFont(const std::filesystem::path& folder, const std::vector<const wchar_t*>& files)
        {
            static std::map<std::wstring, std::vector<unsigned char>> cache;
            static std::vector<unsigned char> none;
            for (const wchar_t* file : files) {
                const auto path = folder / file;
                if (const auto it = cache.find(path.native()); it != cache.end()) return it->second;
                std::ifstream in(path, std::ios::binary);
                if (!in) continue;
                std::vector<unsigned char> data{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
                if (data.empty()) continue;
                return cache.emplace(path.native(), std::move(data)).first->second;
            }
            return none;
        }

        // Chinese, Japanese and Korean share many characters but draw them differently, so the font of Windows' display
        // language comes first.
        std::vector<std::vector<const wchar_t*>> EastAsianFonts()
        {
            const std::vector<const wchar_t*> chinese{L"msyh.ttc", L"simsun.ttc"};
            const std::vector<const wchar_t*> japanese{L"YuGothR.ttc", L"meiryo.ttc", L"msgothic.ttc"};
            const std::vector<const wchar_t*> korean{L"malgun.ttf"};
            switch (PRIMARYLANGID(GetUserDefaultUILanguage())) {
            case LANG_JAPANESE: return {japanese, chinese, korean};
            case LANG_KOREAN: return {korean, chinese, japanese};
            default: return {chinese, japanese, korean};
            }
        }
    }

    bool LoadFonts(ImGuiIO& io, const std::string& directory, Fonts& fonts)
    {
        // Poppins and Montserrat draw everything they have. A letter they lack comes from Windows' own fonts, as the
        // HTML screen's sans-serif fallback did: Segoe UI at a close weight (Cyrillic, Greek), then Chinese, Japanese and
        // Korean. A system font that is missing is skipped.
        const auto system = WindowsFonts();
        const auto eastAsian = EastAsianFonts();
        auto merge = [&](float size, std::vector<unsigned char>& data) {
            if (data.empty()) return;
            ImFontConfig config;
            config.MergeMode = true;
            config.FontDataOwnedByAtlas = false;
            io.Fonts->AddFontFromMemoryTTF(data.data(), static_cast<int>(data.size()), size, &config);
        };
        auto load = [&](const char* file, float size, const wchar_t* fallback) -> ImFont* {
            auto path = std::filesystem::path(directory) / file;
            if (!std::filesystem::is_regular_file(path)) return nullptr;
            ImFont* font = io.Fonts->AddFontFromFileTTF(path.string().c_str(), size);
            if (!font || system.empty()) return font;
            merge(size, SystemFont(system, {fallback, L"segoeui.ttf"}));
            for (const auto& files : eastAsian) merge(size, SystemFont(system, files));
            return font;
        };
        fonts.body = load("Poppins-Regular.ttf", 14.0f, L"segoeui.ttf");
        fonts.medium = load("Poppins-Medium.ttf", 14.0f, L"seguisb.ttf");
        fonts.bold = load("Poppins-SemiBold.ttf", 16.0f, L"seguisb.ttf");
        fonts.heading = load("Montserrat-Black.ttf", 18.0f, L"seguibl.ttf");
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
