#include "TailorScreen.h"
#include "ControllerLegend.h"
#include "NameOrder.h"
#include "screens/HairPresets.h"
#include "screens/SettingsScreen.h"
#include "outfit/OutfitNamePolicy.h"
#include "outfit/OutfitSex.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <numbers>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include <imgui_internal.h>

namespace Tailor::ImGuiUI
{
    namespace
    {
        const Model emptyObject = Model::object(), emptyArray = Model::array();
        const Model& Get(const Model& data, const char* key)
        {
            auto it = data.find(key);
            return it == data.end() ? emptyObject : *it;
        }
        const Model& Rows(const Model& data)
        {
            return data.is_array() ? data : emptyArray;
        }
        std::string Text(const Model& data, const char* key, const char* fallback = "")
        {
            const auto& value = Get(data, key);
            return value.is_string() ? value.get<std::string>() : fallback;
        }
        int Number(const Model& data, const char* key, int fallback = 0)
        {
            const auto& value = Get(data, key);
            return value.is_number_integer() ? value.get<int>() : fallback;
        }
        bool Flag(const Model& data, const char* key)
        {
            const auto& value = Get(data, key);
            return value.is_boolean() && value.get<bool>();
        }
        // A search ignores capitals in any alphabet, as names do (FoldedName).
        bool Matches(const std::string& text, const std::string& query)
        {
            return Tailor::Outfits::FoldedName(text).find(Tailor::Outfits::FoldedName(query)) != std::string::npos;
        }
        // Lists sort by name as the HTML screen's localeCompare did (NameSortKey). A name's key is worked out once and
        // kept, so a long list costs a lookup per row each frame.
        void SortByName(Model& rows)
        {
            static std::unordered_map<std::string, std::string> keys;
            std::vector<std::pair<const std::string*, Model>> keyed;
            keyed.reserve(rows.size());
            for (auto& row : rows) {
                const auto name = Text(row, "name");
                auto it = keys.find(name);
                if (it == keys.end()) it = keys.emplace(name, NameSortKey(name)).first;
                keyed.emplace_back(&it->second, std::move(row));
            }
            std::sort(keyed.begin(), keyed.end(), [](const auto& a, const auto& b) { return *a.first < *b.first; });
            rows = Model::array();
            for (auto& entry : keyed) rows.push_back(std::move(entry.second));
        }
        // Puts `value` in a fixed text box, and says whether it had to be cut. A value too long for the box is cut
        // between UTF-8 characters, never inside one: half a character is bytes that aren't valid UTF-8, and sending
        // them would throw. Text that fits goes in as it is.
        template <std::size_t N> bool Copy(std::array<char, N>& dest, const std::string& value)
        {
            const std::string_view text(value.c_str());
            std::size_t length = std::min(text.size(), N - 1);
            if (length < text.size())
                while (length > 0 && (static_cast<unsigned char>(text[length]) & 0xC0) == 0x80) --length;
            std::memcpy(dest.data(), text.data(), length);
            dest[length] = '\0';
            return length < text.size();
        }
        // An action's payload, or a row's ID, as text. A string the game handed over may not be valid UTF-8 (a plugin's
        // file name in a Windows code page): its bad bytes are written as U+FFFD instead of throwing, which would end
        // the frame. Valid text comes out exactly as dump() writes it, so IDs and payloads don't change.
        std::string Serialize(const Model& value) { return value.dump(-1, ' ', false, Model::error_handler_t::replace); }
        // An outfit's sex as the bridge sends it: -1 Unisex, 0 Male, 1 Female. Older data has none.
        int SexOf(const Model& outfit)
        {
            const int sex = Number(outfit, "sex", -1);
            return sex == 0 || sex == 1 ? sex : -1;
        }
        const char* SexLabel(int sex) { return OutfitSexName(static_cast<OutfitSex>(sex)); }
        // A name of nothing but spaces is no name: the stores refuse it, so the screens never send it.
        bool BlankName(const char* name) { return Tailor::Outfits::TrimName(name).empty(); }
        // Whether an outfit in the published list other than `exceptId` (the one being edited; 0 for a
        // new one) has this name, by the store's rule: capitals and surrounding spaces don't count.
        bool OutfitNameTaken(const Model& outfits, const std::string& name, int exceptId = 0)
        {
            return std::any_of(outfits.begin(), outfits.end(), [&](const Model& outfit) {
                return Number(outfit, "id") != exceptId && Tailor::Outfits::SameName(Text(outfit, "name"), name);
            });
        }
        // The same for categories: every category counts, the defaults and the situation pools included.
        bool CategoryNameTaken(const Model& model, const std::string& name, int exceptId = 0)
        {
            const auto taken = [&](const Model& categories) {
                return std::any_of(categories.begin(), categories.end(), [&](const Model& category) {
                    return Number(category, "id") != exceptId && Tailor::Outfits::SameName(Text(category, "name"), name);
                });
            };
            return taken(Rows(Get(model, "tailorSetCategories"))) || taken(Rows(Get(Get(model, "tailorSetAllCategories"), "categories")));
        }
        // The first name for a copy that no outfit has: "<name> Copy", then "<name> Copy (2)", and so on.
        std::string CopyName(const Model& outfits, const std::string& name)
        {
            const auto base = name + " Copy";
            if (!OutfitNameTaken(outfits, base)) return base;
            return Tailor::Outfits::NumberedName(base, [&](const std::string& key) { return OutfitNameTaken(outfits, key); });
        }
        std::string OutfitTakenMessage(const std::string& name) { return "An outfit named '" + name + "' already exists"; }
        std::string CategoryTakenMessage(const std::string& name) { return "A category named '" + name + "' already exists"; }
        // The target's sex, from tailorSetTarget; -1 without a target.
        int TargetSex(const Model& target)
        {
            const auto sex = Text(target, "sex");
            return sex == "female" ? 1 : sex == "male" ? 0 : -1;
        }
        // Tag ink: rose for Female, steel for Male; 0 keeps the usual text color.
        ImU32 SexInk(int sex) { return sex == 1 ? Palette::Rose : sex == 0 ? Palette::Steel : 0; }
        float Scale(float height)
        {
            if (height <= 1080) return 1.0f;
            if (height <= 1440) return 1.0f + (height - 1080) / 1080;
            return std::min(1.75f, 4.0f / 3.0f + (height - 1440) * (1.75f - 4.0f / 3.0f) / 720);
        }
        // prefix + name when it fits maxWidth; otherwise the name cut between UTF-8
        // characters and ended with "...". Never shorter than prefix + "...".
        std::string FitLabel(ImFont* font, float size, const std::string& prefix, const std::string& name, float maxWidth)
        {
            const auto fits = [&](const std::string& text) { return font->CalcTextSizeA(size, FLT_MAX, 0, text.c_str()).x <= maxWidth; };
            if (fits(prefix + name)) return prefix + name;
            std::string label = prefix;
            for (const char *p = name.data(), *end = p + name.size(); p < end;) {
                unsigned int c; const int n = std::max(1, ImTextCharFromUtf8(&c, p, end));
                if (!fits(label + std::string(p, n) + "...")) break;
                label.append(p, n); p += n;
            }
            while (label.size() > prefix.size() && label.back() == ' ') label.pop_back();
            return label + "...";
        }
        struct Screen
        {
            const Model& model;
            ScreenState& state;
            const Fonts& fonts;
            FrameResult result;
            float scale;
            bool interactive = true;
            bool backConsumed = false;
            bool controllerBlocked = false;
            bool controllerFocusWork = false;
            bool controllerFocusChosen = false;
            bool selectAppearing = false, selectQueryChanged = false, selectOptions = false;

            void DefaultControllerFocus()
            {
                if (!controllerFocusWork || controllerFocusChosen) return;
                ImGui::FocusWindow(ImGui::GetCurrentWindow());
                ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
                ImGui::SetNavCursorVisible(true);
                auto& context = *ImGui::GetCurrentContext();
                context.NavInitRequest = false; context.NavInitResult.ID = 0;
                controllerFocusChosen = true;
            }

            bool Controller() const { return Flag(model, "_usingGamepad"); }
            bool BackPressed() const
            {
                return interactive && (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || (Controller() && ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)));
            }
            void ConfirmCycle()
            {
                const auto& data = Get(model, state.wigs ? "wiggySetCycleState" : "tailorSetCycleState");
                if (Number(data, "total") <= 0) return;
                const int situation = state.situation;
                if (situation) Emit(state.wigs ? "wiggyConfirmSituationCycle" : "tailorConfirmSituationCycle", {{"situation", situation}});
                else Emit(state.wigs ? "wiggyConfirmCycle" : "tailorConfirmCycle");
                state.page = situation ? Page::Situations : Page::Main;
                state.situation = 0;
                if (situation) Emit(state.wigs ? "wiggyRequestWigSituations" : "tailorRequestSituations");
            }
            void EnterRotation(bool focusButton = true)
            {
                auto& context = *ImGui::GetCurrentContext();
                state.controllerReturnWindow = context.NavWindow ? context.NavWindow->ID : 0;
                state.controllerReturnItem = context.NavId;
                if (context.NavWindow) {
                    const auto& rect = context.NavWindow->NavRectRel[ImGuiNavLayer_Main];
                    state.controllerReturnRect = ImVec4(rect.Min.x, rect.Min.y, rect.Max.x, rect.Max.y);
                }
                state.controllerRotating = true;
                if (auto* preview = ImGui::FindWindowByName("Tailor Live Preview"); focusButton && preview) {
                    ImGui::FocusWindow(preview);
                    ImGui::SetFocusID(ImHashStr("Rotate preview", 0, preview->ID), preview);
                    ImGui::SetNavCursorVisible(true);
                }
            }
            void ControllerActions()
            {
                constexpr ImGuiID controllerOwner = 0x7461696C;
                const bool tertiary = ImGui::IsKeyPressed(ImGuiKey_GamepadFaceUp, ImGuiInputFlags_None, controllerOwner);
                // FaceUp is Tailor's rotation action, while ImGui also treats it
                // as "activate for input". Consume that activation in every scope.
                if (Controller() && ImGui::IsKeyDown(ImGuiKey_GamepadFaceUp, controllerOwner)) {
                    ImGui::SetKeyOwner(ImGuiKey_GamepadFaceUp, controllerOwner, ImGuiInputFlags_LockThisFrame);
                    auto& context = *ImGui::GetCurrentContext();
                    if (!ImGui::IsKeyDown(ImGuiKey_GamepadFaceDown)) context.NavActivateId = context.NavActivateDownId = context.NavActivatePressedId = 0;
                }
                // NewFrame consumes navigation cancel before widgets are drawn.
                // Keep the previous scope so one B cannot also leave this page.
                const bool popup = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);
                controllerBlocked = state.controllerEditing || state.controllerPopupOpen || popup || state.confirmRequested || state.promptRequested;
                if (BackPressed() && controllerBlocked) backConsumed = true;
                if (state.controllerEditing && (BackPressed() || (Controller() && state.controllerTextEditing && ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown, false)))) {
                    ImGui::ClearActiveID();
                    auto& context = *ImGui::GetCurrentContext();
                    context.NavActivateId = context.NavActivateDownId = context.NavActivatePressedId = 0;
                }
                const bool changedPage = state.controllerPage != state.page || state.controllerWigs != state.wigs;
                controllerFocusWork = Controller() && (!state.controllerFocusInitialized || changedPage);
                if (!Controller() || Flag(model, "_controllerCursor") || !interactive || !Flag(Get(model, "tailorSetPreviewState"), "active") || changedPage || controllerBlocked) state.controllerRotating = false;
                if (!Controller() || !interactive || controllerBlocked) return;
                if (state.controllerRotating) {
                    if (BackPressed()) {
                        state.controllerRotating = false; backConsumed = true;
                        if (auto* previous = ImGui::FindWindowByID(state.controllerReturnWindow); previous && previous->WasActive) {
                            const auto rect = state.controllerReturnRect;
                            previous->NavRectRel[ImGuiNavLayer_Main] = ImRect(rect.x, rect.y, rect.z, rect.w);
                            ImGui::FocusWindow(previous); ImGui::SetFocusID(state.controllerReturnItem, previous);
                        } else controllerFocusWork = true;
                        return;
                    }
                    if (tertiary) state.yaw = 0;
                    else {
                        const auto& x = Get(model, "_controllerRightX");
                        if (x.is_number()) state.yaw += std::clamp(x.get<float>(), -1.0f, 1.0f) * std::min(ImGui::GetIO().DeltaTime, 0.05f) * std::numbers::pi_v<float> / 2;
                    }
                    return;
                }
                if (tertiary && Flag(Get(model, "tailorSetPreviewState"), "active") && !Flag(model, "_controllerCursor")) { EnterRotation(); return; }
                if (state.page == Page::Cycle && ImGui::IsKeyPressed(ImGuiKey_GamepadFaceLeft, false)) { ConfirmCycle(); return; }
                const bool previous = ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false), next = ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false);
                if (!previous && !next) return;
                if (state.page == Page::Cycle) Emit(state.wigs ? (next ? "wiggyCycleNext" : "wiggyCyclePrev") : (next ? "tailorCycleNext" : "tailorCyclePrev"));
                else {
                    constexpr std::pair<Page, bool> rails[] = {{Page::Main,false},{Page::Library,false},{Page::Discovered,false},{Page::Situations,false},{Page::Export,false},{Page::Import,false},{Page::Main,true},{Page::Library,true},{Page::HairColor,true},{Page::Situations,true}};
                    int selected = 0;
                    for (int i = 0; i < 10; ++i) if (rails[i].first == state.page && rails[i].second == state.wigs) selected = i;
                    const auto& target = rails[(selected + (next ? 1 : 9)) % 10];
                    Navigate(target.first, target.second);
                }
            }
            ControllerLegend Legend() const
            {
                ControllerLegend legend;
                legend.playstation = Text(model, "_controllerGlyphs", "xbox") == "playstation";
                const bool selection = state.controllerPopupOpen || state.confirmRequested || state.promptRequested || ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);
                if (state.controllerRotating) legend.title = "Preview rotation";
                else if (state.controllerEditing && !state.controllerTextEditing) legend.title = "Edit value";
                else if (selection) legend.title = "Selection";
                else switch (state.page) {
                case Page::Main: legend.title = state.wigs ? "Wigs" : "Outfits"; break;
                case Page::Cycle: legend.title = state.wigs ? "Wig preview" : "Dressing"; break;
                case Page::Library: legend.title = state.wigs ? "Manage wigs" : "Manage outfits"; break;
				case Page::Discovered: legend.title = "Discovered sets"; break;
                case Page::Create: legend.title = state.editId ? "Edit outfit" : "Create outfit"; break;
                case Page::Categories: legend.title = "Categories"; break;
                case Page::Blacklist: legend.title = state.wigs ? "Wig blacklist" : "Blacklist"; break;
                case Page::Situations: legend.title = state.wigs ? "Wig situations" : "Situations"; break;
                case Page::Export: legend.title = "Export"; break;
                case Page::Import: legend.title = "Import"; break;
                case Page::HairColor: legend.title = "Hair color"; break;
                case Page::CustomColors: legend.title = "Custom colors"; break;
                case Page::AddWigs: legend.title = "Add wigs"; break;
                case Page::Settings: legend.title = "Settings"; break;
                }
                auto add = [&](const char* action, const char* fallback, const char* label) { legend.hints.push_back({ControllerBinding(model, action, fallback), label}); };
                if (state.controllerRotating) {
                    legend.hints.push_back({"Right stick", "Rotate"}); add("tertiary", "North", "Front"); add("cancel", "East", "Return");
                } else {
                    add("accept", "South", state.controllerEditing && !state.controllerTextEditing ? "Done" : "Select / Edit"); add("cancel", "East", "Back");
                    if (state.controllerTextEditing) legend.hints.push_back({"", "Keyboard to type"});
                    else if (state.controllerEditing) legend.hints.push_back({"Directions", "Adjust"});
                    else if (!selection) {
                        add("previousTab", "LeftShoulder", state.page == Page::Cycle ? "Previous" : "Page"); add("nextTab", "RightShoulder", "Next");
                        if (state.page == Page::Cycle) add("secondary", "West", "Assign");
                        if (Flag(Get(model, "tailorSetPreviewState"), "active") && !Flag(model, "_controllerCursor")) add("tertiary", "North", "Rotate");
                        add("toggleCursor", "RightThumb", Flag(model, "_controllerCursor") ? "Navigation" : "Cursor");
                    }
                }
                return legend;
            }
            void RememberControllerScope()
            {
                auto& context = *ImGui::GetCurrentContext();
                if (Controller() && interactive && !Flag(model, "_controllerCursor") && !state.controllerRotating && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)) {
                    auto focus = [&](const char* name) {
                        if (auto* target = ImGui::FindWindowByName(name); target && target->Active) {
                            ImGui::FocusWindow(target);
                            ImGui::NavInitWindow(target, true);
                            ImGui::SetNavCursorVisible(true);
                        }
                    };
                    if (!controllerFocusChosen && (controllerFocusWork || state.controllerPage != state.page || state.controllerWigs != state.wigs)) focus("Tailor Work");
                    else if (context.NavWindow && context.NavMoveScoringItems && !context.NavMoveResultLocal.ID && !context.NavMoveResultOther.ID && !context.ActiveId) {
                        const auto* root = context.NavWindow->RootWindowForNav;
                        const auto direction = context.NavMoveDir;
                        const char* target = nullptr;
                        if (direction == ImGuiDir_Down) target = "Tailor Counter";
                        if (direction == ImGuiDir_Up) target = root == ImGui::FindWindowByName("Tailor Counter") ? "Tailor Work" : (state.wigs ? "Tailor Wig Rail" : "Tailor Outfit Rail");
                        if (direction == ImGuiDir_Left || direction == ImGuiDir_Right) {
                            const bool right = direction == ImGuiDir_Right;
                            if (root == ImGui::FindWindowByName("Tailor Work")) target = right ? (state.wigs ? "Tailor Wig Rail" : "Tailor Live Preview") : (state.wigs ? "Tailor Live Preview" : "Tailor Outfit Rail");
                            else if (root == ImGui::FindWindowByName("Tailor Live Preview")) target = right ? (state.wigs ? "Tailor Work" : "Tailor Wig Rail") : (state.wigs ? "Tailor Outfit Rail" : "Tailor Work");
                            else if (root == ImGui::FindWindowByName("Tailor Outfit Rail") && right) target = state.wigs ? "Tailor Live Preview" : "Tailor Work";
                            else if (root == ImGui::FindWindowByName("Tailor Wig Rail") && !right) target = state.wigs ? "Tailor Work" : "Tailor Live Preview";
                            else if (root == ImGui::FindWindowByName("Tailor Counter")) target = right ? "Tailor Wig Rail" : "Tailor Outfit Rail";
                        }
                        if (target && ImGui::FindWindowByName(target) != root) {
                            ImGui::NavMoveRequestCancel(); focus(target);
                        }
                    }
                    state.controllerFocusInitialized = true;
                }
                state.controllerTextEditing = context.ActiveId != 0 && context.InputTextState.ID == context.ActiveId;
                state.controllerEditing = context.ActiveId != 0 && (state.controllerTextEditing || context.ActiveIdUsingNavDirMask != 0);
                state.controllerPopupOpen = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);
                if (state.controllerPage != state.page || state.controllerWigs != state.wigs) state.controllerRotating = false;
                state.controllerPage = state.page; state.controllerWigs = state.wigs;
            }

            void Emit(const std::string& name, const Model& payload = Model())
            {
                result.actions.push_back({name, payload.is_null() ? "" : Serialize(payload)});
            }
            bool Changed(const char* key)
            {
                const auto& version = Get(Get(model, "_versions"), key);
                if (!version.is_number_unsigned() && !version.is_number_integer()) return false;
                const auto current = version.get<std::uint64_t>();
                auto& seen = state.consumed[key];
                if (seen == current) return false;
                seen = current;
                return true;
            }
            // Saves are off for outfits or categories this session (a file Tailor couldn't fully read at startup):
            // Create Outfit, Save, Delete, Set Sex, Copy, Import and the Categories page's Create, Save Name and Delete
            // Category are disabled where they stand, so nothing on the page moves. A value read every frame, not an
            // event, so the reset at each open keeps it.
            bool LibraryReadOnly() const { return Flag(Get(model, "tailorSetLibraryState"), "readOnly"); }
            // The name box holds 255 bytes. A longer name (the mod API sets no limit) goes in cut between characters,
            // and only then is it remembered in full. While the box still holds exactly the cut text, the full name is
            // the one checked and sent, so opening a long name and saving never cuts it (D6); typed text is sent as
            // typed. Every place that empties the box forgets it.
            void SetName(const std::string& value)
            {
                state.nameCut.clear(); state.nameFull.clear();
                if (Copy(state.name, value)) { state.nameCut = state.name.data(); state.nameFull = value; }
            }
            void ForgetName() { state.name.fill(0); state.nameCut.clear(); state.nameFull.clear(); }
            std::string EffectiveName() const
            {
                return !state.nameFull.empty() && state.nameCut == state.name.data() ? state.nameFull : std::string(state.name.data());
            }
            void Label(const std::string& text, ImU32 color = Palette::Text, float size = 14, ImFont* font = nullptr)
            {
                ImGui::PushFont(font ? font : fonts.body, size * scale * FontMetricScale);
                ImGui::PushStyleColor(ImGuiCol_Text, color);
                ImGui::TextUnformatted(text.c_str());
                ImGui::PopStyleColor();
                ImGui::PopFont();
            }
            void Wrapped(const std::string& text, ImU32 color = Palette::Text)
            {
                ImGui::PushStyleColor(ImGuiCol_Text, color);
                ImGui::PushTextWrapPos(0);
                ImGui::TextUnformatted(text.c_str());
                ImGui::PopTextWrapPos();
                ImGui::PopStyleColor();
            }
            float Px(float size) const { return size * scale * FontMetricScale; }
            // Letter-spaced text. Forge eyebrows, titles and the nameplate are tracked
            // uppercase, which ImGui cannot express through its own text calls.
            float TrackedWidth(const std::string& text, ImFont* font, float size, float tracking) const
            {
                float width = 0; int glyphs = 0;
                for (const char* p = text.c_str(); *p; ++glyphs) {
                    unsigned int c; const int n = std::max(1, ImTextCharFromUtf8(&c, p, nullptr));
                    width += font->CalcTextSizeA(Px(size), FLT_MAX, 0, p, p + n).x; p += n;
                }
                return width + std::max(0, glyphs - 1) * tracking * scale;
            }
            // A null stops list selects one color; otherwise the run blends across the stops.
            void Tracked(ImDrawList* draw, ImVec2 pos, const std::string& text, ImFont* font, float size, ImU32 color, float tracking, const ImU32* stops = nullptr, int stopCount = 0) const
            {
                const float total = stops ? std::max(1.0f, TrackedWidth(text, font, size, tracking)) : 1, left = pos.x;
                for (const char* p = text.c_str(); *p;) {
                    unsigned int c; const int n = std::max(1, ImTextCharFromUtf8(&c, p, nullptr));
                    ImU32 glyph = color;
                    if (stops) {
                        const float t = std::clamp((pos.x - left) / total, 0.0f, 1.0f) * (stopCount - 1);
                        const int i = std::min(static_cast<int>(t), stopCount - 2);
                        glyph = ImGui::ColorConvertFloat4ToU32(ImLerp(ImGui::ColorConvertU32ToFloat4(stops[i]), ImGui::ColorConvertU32ToFloat4(stops[i + 1]), t - i));
                    }
                    draw->AddText(font, Px(size), pos, glyph, p, p + n);
                    pos.x += font->CalcTextSizeA(Px(size), FLT_MAX, 0, p, p + n).x + tracking * scale; p += n;
                }
            }
            // Section eyebrow: short gold rule, tracked label, quiet count.
            void Eyebrow(const std::string& text, const std::string& meta = "", bool rule = true, ImU32 color = Palette::Amber)
            {
                const auto at = ImGui::GetCursorScreenPos();
                auto* draw = ImGui::GetWindowDrawList();
                const float height = Px(11);
                float x = at.x;
                if (rule) { draw->AddLine({x, at.y + height / 2}, {x + 22 * scale, at.y + height / 2}, IM_COL32(245, 158, 11, 150), scale); x += 32 * scale; }
                Tracked(draw, {x, at.y}, text, fonts.bold, 11, color, 2.2f);
                x += TrackedWidth(text, fonts.bold, 11, 2.2f);
                if (!meta.empty()) {
                    x += 12 * scale; Tracked(draw, {x, at.y}, meta, fonts.body, 11, Palette::Muted, 0.6f);
                    x += TrackedWidth(meta, fonts.body, 11, 0.6f);
                }
                ImGui::Dummy({x - at.x, height});
            }
            enum class Icon { None, Back, ChevronLeft, ChevronRight, ChevronDown, Close, RotateLeft, RotateRight, Plus, Gear, Grid, Layers, Check, Search };
            // Lucide-style stroke icons on a 16-unit box centered on c.
            void DrawIcon(ImDrawList* draw, Icon icon, ImVec2 c, float size, ImU32 color) const
            {
                const float u = size / 16, stroke = 1.5f * scale;
                auto p = [&](float x, float y) { return ImVec2(c.x + x * u, c.y + y * u); };
                auto line = [&](float x, float y, float xx, float yy) { draw->AddLine(p(x, y), p(xx, yy), color, stroke); };
                switch (icon) {
                case Icon::Back: line(6, 0, -6, 0); line(-6, 0, -1, -5); line(-6, 0, -1, 5); break;
                case Icon::ChevronLeft: line(2.5f, -5, -2.5f, 0); line(-2.5f, 0, 2.5f, 5); break;
                case Icon::ChevronRight: line(-2.5f, -5, 2.5f, 0); line(2.5f, 0, -2.5f, 5); break;
                case Icon::ChevronDown: line(-4, -2, 0, 2); line(0, 2, 4, -2); break;
                case Icon::Close: line(-4.5f, -4.5f, 4.5f, 4.5f); line(-4.5f, 4.5f, 4.5f, -4.5f); break;
                case Icon::Plus: line(-5, 0, 5, 0); line(0, -5, 0, 5); break;
                case Icon::Check: line(-5, 0, -1.5f, 3.5f); line(-1.5f, 3.5f, 5, -4); break;
                case Icon::RotateLeft: case Icon::RotateRight: {
                    // Open ring with a corner arrowhead, mirrored for the clockwise turn.
                    const float m = icon == Icon::RotateLeft ? 1.0f : -1.0f, pi = std::numbers::pi_v<float>;
                    draw->PathArcTo(c, 6 * u, m > 0 ? pi : 0, m > 0 ? -0.75f * pi : 1.75f * pi, 24);
                    draw->PathStroke(color, 0, stroke);
                    line(-6 * m, -6.5f, -6 * m, -2.5f); line(-6 * m, -2.5f, -2 * m, -2.5f);
                    break;
                }
                case Icon::Gear:
                    draw->AddCircle(c, 4.6f * u, color, 24, stroke); draw->AddCircle(c, 1.8f * u, color, 16, stroke);
                    for (int i = 0; i < 8; ++i) {
                        const float a = i * std::numbers::pi_v<float> / 4;
                        draw->AddLine({c.x + std::cos(a) * 4.6f * u, c.y + std::sin(a) * 4.6f * u}, {c.x + std::cos(a) * 7 * u, c.y + std::sin(a) * 7 * u}, color, stroke * 1.4f);
                    }
                    break;
                case Icon::Grid:
                    for (int y = 0; y < 2; ++y) for (int x = 0; x < 2; ++x) draw->AddRect(p(x * 8.0f - 7, y * 8.0f - 7), p(x * 8.0f - 1, y * 8.0f - 1), color, 1.2f * scale, 0, stroke);
                    break;
                case Icon::Layers:
                    line(-7, -2, 0, -6); line(0, -6, 7, -2); line(-7, -2, 0, 2); line(0, 2, 7, -2);
                    line(-7, 2, 0, 6); line(0, 6, 7, 2);
                    break;
                case Icon::Search: draw->AddCircle(p(-1, -1), 4.5f * u, color, 20, stroke); line(2.5f, 2.5f, 6, 6); break;
                case Icon::None: break;
                }
            }
            enum class Tone { Neutral, Amber, Danger, Primary };
            static std::string Visible(const char* label)
            {
                const char* end = std::strstr(label, "##");
                return end ? std::string(label, end) : std::string(label);
            }
            // Every push button. ImGui owns the hit target, focus and ID; the face is
            // drawn here so type, icons and the gold-leaf primary follow Forge.
            bool Press(const char* label, ImVec2 size, Tone tone = Tone::Neutral, bool enabled = true, Icon icon = Icon::None, float fontSize = 14, bool round = false, bool iconOnly = false)
            {
                const auto text = iconOnly ? std::string() : Visible(label);
                ImFont* font = tone == Tone::Primary || fontSize > 14 ? fonts.bold : fonts.medium;
                const float iconSize = icon == Icon::None ? 0 : (fontSize + 2) * scale, gap = icon != Icon::None && !text.empty() ? 8 * scale : 0;
                const float textWidth = text.empty() ? 0 : font->CalcTextSizeA(Px(fontSize), FLT_MAX, 0, text.c_str()).x;
                if (size.x == 0) size.x = textWidth + iconSize + gap + 28 * scale;
                static constexpr ImU32 faces[3][6] = {
                    // fill, hover fill, border, hover border, text, hover text
                    {IM_COL32(244, 235, 216, 8), IM_COL32(244, 235, 216, 18), IM_COL32(244, 235, 216, 24), IM_COL32(244, 235, 216, 46), IM_COL32(244, 235, 216, 235), IM_COL32(244, 235, 216, 255)},
                    {IM_COL32(245, 158, 11, 20), IM_COL32(245, 158, 11, 38), IM_COL32(245, 158, 11, 76), IM_COL32(245, 158, 11, 128), IM_COL32(245, 158, 11, 255), IM_COL32(251, 191, 36, 255)},
                    {IM_COL32(239, 68, 68, 26), IM_COL32(239, 68, 68, 46), IM_COL32(239, 68, 68, 76), IM_COL32(239, 68, 68, 115), IM_COL32(253, 164, 175, 255), IM_COL32(254, 205, 211, 255)}};
                const bool primary = tone == Tone::Primary;
                const auto& face = faces[primary ? 0 : static_cast<int>(tone)];
                ImGui::BeginDisabled(!enabled);
                ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(0, 0, 0, 0));
                ImGui::PushStyleColor(ImGuiCol_Button, primary ? IM_COL32(0, 0, 0, 0) : face[0]);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, primary ? IM_COL32(0, 0, 0, 0) : face[1]);
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, primary ? IM_COL32(0, 0, 0, 0) : face[1]);
                ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(0, 0, 0, 0));
                ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, round ? size.y / 2 : 8 * scale);
                ImGui::PushFont(font, Px(fontSize));
                const bool pressed = ImGui::Button(label, size);
                ImGui::PopFont(); ImGui::PopStyleVar(); ImGui::PopStyleColor(5);
                const bool lit = enabled && (ImGui::IsItemHovered() || ImGui::IsItemFocused());
                const auto min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
                const float rounding = round ? (max.y - min.y) / 2 : 8 * scale;
                auto* draw = ImGui::GetWindowDrawList();
                const int alpha = enabled ? 255 : 115;
                ImU32 ink = face[lit ? 5 : 4];
                if (primary) {
                    // Gold leaf: diagonal gradient, candle-lit top bevel, heat that grows on hover.
                    if (enabled) for (int ring = 3; ring >= 1; --ring)
                        draw->AddRect({min.x - ring * 2 * scale, min.y - ring * 2 * scale}, {max.x + ring * 2 * scale, max.y + ring * 2 * scale}, IM_COL32(245, 158, 11, lit ? 26 : 12), rounding + ring * 2 * scale, 0, 2 * scale);
                    const int first = draw->VtxBuffer.Size;
                    draw->AddRectFilled(min, max, IM_COL32(255, 255, 255, alpha), rounding);
                    ImGui::ShadeVertsLinearColorGradientKeepAlpha(draw, first, draw->VtxBuffer.Size, min, max, lit ? IM_COL32(253, 210, 80, 255) : IM_COL32(251, 191, 36, 255), IM_COL32(217, 119, 6, 255));
                    draw->AddLine({min.x + rounding, min.y + scale}, {max.x - rounding, min.y + scale}, IM_COL32(255, 255, 255, enabled ? 70 : 28), scale);
                    ink = IM_COL32(26, 18, 5, 255);
                } else draw->AddRect(min, max, face[lit ? 3 : 2], rounding);
                ink = (ink & ~IM_COL32_A_MASK) | (static_cast<ImU32>(((ink >> IM_COL32_A_SHIFT) & 0xFF) * alpha / 255) << IM_COL32_A_SHIFT);
                float x = (min.x + max.x - textWidth - iconSize - gap) / 2;
                if (icon != Icon::None) { DrawIcon(draw, icon, {x + iconSize / 2, (min.y + max.y) / 2}, iconSize, ink); x += iconSize + gap; }
                if (!text.empty()) draw->AddText(font, Px(fontSize), {x, (min.y + max.y - Px(fontSize)) / 2}, ink, text.c_str());
                ImGui::EndDisabled();
                return pressed;
            }
            bool Button(const char* label, float width = 0, bool enabled = true, bool danger = false, Icon icon = Icon::None)
            {
                return Press(label, ImVec2(width * scale, 40 * scale), danger ? Tone::Danger : Tone::Neutral, enabled, icon);
            }
            bool Primary(const char* label, float width, bool enabled = true)
            {
                return Press(label, ImVec2(width * scale, 40 * scale), Tone::Primary, enabled);
            }
            // Compact row action (Edit / Copy / Delete): 30px, never wider than its word.
            bool Small(const char* label, Tone tone = Tone::Neutral, bool enabled = true, Icon icon = Icon::None)
            {
                return Press(label, ImVec2(Visible(label).empty() ? 30 * scale : 0, 30 * scale), tone, enabled, icon, 13);
            }
            // Bottom action bar: hairline, then full-height 46px actions as in the HTML court.
            bool Action(const char* label, float width, Tone tone = Tone::Neutral, bool enabled = true, Icon icon = Icon::None)
            {
                return Press(label, ImVec2(width, 46 * scale), tone, enabled, icon, 15);
            }
            float BeginActions()
            {
                const float top = ImGui::GetWindowHeight() - 86 * scale;
                const auto origin = ImGui::GetWindowPos();
                ImGui::GetWindowDrawList()->AddLine({origin.x, origin.y + top}, {origin.x + ImGui::GetWindowWidth(), origin.y + top}, Palette::Border);
                ImGui::SetCursorPos({20 * scale, top + 16 * scale});
                return ImGui::GetWindowWidth() - 40 * scale;
            }
            bool Child(const char* id, ImVec2 size, ImGuiChildFlags flags = 0, ImGuiWindowFlags windowFlags = 0)
            {
                return ImGui::BeginChild(id, size, flags | ImGuiChildFlags_NavFlattened, windowFlags);
            }
            // Scrolling list well. Top and bottom shadows appear only while more rows
            // wait in that direction, so a full list never looks like the whole list.
            void BeginList(const char* id, ImVec2 size, bool bordered = true)
            {
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {0, 0});
                ImGui::PushStyleColor(ImGuiCol_ChildBg, bordered ? IM_COL32(10, 8, 7, 105) : IM_COL32(0, 0, 0, 0));
                ImGui::PushStyleColor(ImGuiCol_Border, Palette::Border);
                Child(id, size, bordered ? ImGuiChildFlags_Borders : ImGuiChildFlags_None);
            }
            void EndList()
            {
                const auto pos = ImGui::GetWindowPos(), size = ImGui::GetWindowSize();
                const float fade = 30 * scale, right = pos.x + size.x - (ImGui::GetScrollMaxY() > 0 ? ImGui::GetStyle().ScrollbarSize : 0) - scale;
                constexpr ImU32 shade = IM_COL32(8, 6, 4, 235), clear = IM_COL32(8, 6, 4, 0);
                auto* draw = ImGui::GetWindowDrawList();
                draw->PushClipRect(pos, {pos.x + size.x, pos.y + size.y});
                if (ImGui::GetScrollY() > 1) draw->AddRectFilledMultiColor({pos.x + scale, pos.y + scale}, {right, pos.y + fade}, shade, shade, clear, clear);
                if (ImGui::GetScrollY() < ImGui::GetScrollMaxY() - 1) draw->AddRectFilledMultiColor({pos.x + scale, pos.y + size.y - fade}, {right, pos.y + size.y - scale}, clear, clear, shade, shade);
                draw->PopClipRect();
                ImGui::EndChild(); ImGui::PopStyleColor(2); ImGui::PopStyleVar(2);
            }
            // Full-width list row. Returns the press; the caller paints the content at `at`.
            bool Row(const char* id, float height, bool selected, ImVec2& at, float& width, bool overlap = false, bool zebra = false)
            {
                at = ImGui::GetCursorScreenPos(); width = ImGui::GetContentRegionAvail().x;
                ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(0, 0, 0, 0));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(0, 0, 0, 0));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(0, 0, 0, 0));
                ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(0, 0, 0, 0));
                ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0);
                if (overlap) ImGui::SetNextItemAllowOverlap();
                const bool pressed = ImGui::Button(id, {width, height});
                ImGui::PopStyleVar(); ImGui::PopStyleColor(4);
                auto* draw = ImGui::GetWindowDrawList();
                const bool lit = ImGui::IsItemHovered() || ImGui::IsItemFocused();
                if (selected) {
                    draw->AddRectFilled(at, {at.x + width, at.y + height}, IM_COL32(245, 158, 11, 20));
                    draw->AddRectFilled(at, {at.x + 2 * scale, at.y + height}, Palette::Amber);
                } else if (lit) draw->AddRectFilled(at, {at.x + width, at.y + height}, Palette::Fill2);
                else if (zebra) draw->AddRectFilled(at, {at.x + width, at.y + height}, IM_COL32(244, 235, 216, 5));
                draw->AddLine({at.x, at.y + height - scale}, {at.x + width, at.y + height - scale}, Palette::Line1);
                return pressed;
            }
            // Category tag under an outfit name. Advances x past the chip.
            void Chip(ImDrawList* draw, ImVec2& at, const std::string& text) const
            {
                const float width = fonts.bold->CalcTextSizeA(Px(11), FLT_MAX, 0, text.c_str()).x + 14 * scale, height = 19 * scale;
                draw->AddRectFilled(at, {at.x + width, at.y + height}, IM_COL32(184, 115, 51, 36), 4 * scale);
                draw->AddRect(at, {at.x + width, at.y + height}, IM_COL32(184, 115, 51, 64), 4 * scale);
                draw->AddText(fonts.bold, Px(11), {at.x + 7 * scale, at.y + (height - Px(11)) / 2}, Palette::CopperLight, text.c_str());
                at.x += width + 6 * scale;
            }
            // Sex tag leading an outfit's chip line: the original court's rose and steel
            // pills. Unisex fits everyone and carries none. Advances x past the tag.
            void SexTag(ImDrawList* draw, ImVec2& at, int sex) const
            {
                if (sex != 0 && sex != 1) return;
                const char* text = sex == 1 ? "FEMALE" : "MALE";
                const ImU32 ink = sex == 1 ? Palette::Rose : Palette::Steel;
                const auto tint = [&](int alpha) { return (ink & ~IM_COL32_A_MASK) | (static_cast<ImU32>(alpha) << IM_COL32_A_SHIFT); };
                const float textWidth = TrackedWidth(text, fonts.bold, 10, 1.4f), height = 19 * scale, width = std::max(56 * scale, textWidth + 20 * scale);
                draw->AddRectFilled(at, {at.x + width, at.y + height}, tint(31), height / 2);
                draw->AddRect(at, {at.x + width, at.y + height}, tint(64), height / 2);
                Tracked(draw, {at.x + (width - textWidth) / 2, at.y + (height - Px(10)) / 2}, text, fonts.bold, 10, ink, 1.4f);
                at.x += width + 6 * scale;
            }
            // Armor-rating disc beside an outfit name.
            void Rating(ImDrawList* draw, ImVec2 center, int rating) const
            {
                const auto number = std::to_string(rating);
                const auto size = fonts.bold->CalcTextSizeA(Px(11), FLT_MAX, 0, number.c_str());
                draw->AddCircleFilled(center, std::max(13 * scale, size.x / 2 + 5 * scale), IM_COL32(43, 57, 63, 255), 28);
                draw->AddText(fonts.bold, Px(11), {center.x - size.x / 2, center.y - size.y / 2}, IM_COL32(131, 202, 230, 255), number.c_str());
            }
            void Check(ImDrawList* draw, ImVec2 at, bool checked, bool lit = false) const
            {
                const ImVec2 max{at.x + 18 * scale, at.y + 18 * scale};
                draw->AddRectFilled(at, max, checked ? Palette::Amber : IM_COL32(244, 235, 216, 10), 4 * scale);
                draw->AddRect(at, max, checked ? Palette::Amber : (lit ? Palette::Line3 : Palette::Fill3), 4 * scale);
                if (checked) DrawIcon(draw, Icon::Check, {at.x + 9 * scale, at.y + 9 * scale}, 13 * scale, IM_COL32(26, 18, 5, 255));
            }
            // Select: a BeginCombo with the Forge face. The ID, popup and controller
            // scope stay ImGui's; the chevron, popover and option rows are ours.
            // `ink` tints the chosen value, as the Sex selects do; 0 keeps the usual color.
            bool BeginSelect(const char* id, const std::string& preview, bool placeholder = false, ImU32 ink = 0)
            {
                const auto at = ImGui::GetCursorScreenPos();
                const float width = ImGui::CalcItemWidth(), height = ImGui::GetFrameHeight();
                auto* draw = ImGui::GetWindowDrawList();
                const bool wasOpen = ImGui::IsPopupOpen(ImGui::GetID(id), ImGuiPopupFlags_None);
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {6 * scale, 6 * scale});
                ImGui::PushStyleColor(ImGuiCol_PopupBg, IM_COL32(24, 20, 15, 252));
                ImGui::PushStyleColor(ImGuiCol_Border, wasOpen ? IM_COL32(245, 158, 11, 128) : Palette::Fill3);
                ImGui::PushStyleColor(ImGuiCol_Text, ink ? ink : placeholder ? Palette::Ghost : Palette::Text);
                const bool open = ImGui::BeginCombo(id, preview.c_str(), ImGuiComboFlags_NoArrowButton | ImGuiComboFlags_HeightLarge);
                ImGui::PopStyleColor(2);
                DrawIcon(draw, Icon::ChevronDown, {at.x + width - 20 * scale, at.y + height / 2 + (open ? -scale : scale)}, 12 * scale, open ? Palette::Amber : Palette::Muted);
                if (open) { ImGui::PushStyleColor(ImGuiCol_Border, Palette::Fill3); ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {0, 2 * scale}); selectAppearing = ImGui::IsWindowAppearing(); }
                else { ImGui::PopStyleColor(); ImGui::PopStyleVar(); }
                return open;
            }
            void EndSelect()
            {
                if (selectOptions) { ImGui::EndChild(); ImGui::PopStyleColor(); ImGui::PopStyleVar(); selectOptions = false; }
                ImGui::EndCombo(); ImGui::PopStyleColor(2); ImGui::PopStyleVar(2);
            }
            // Filter pinned to the top of a long select. Opening starts from a clean
            // query with the caret ready, so stale text never hides the options.
            void SelectSearch(const char* id, const char* hint, char* buffer, std::size_t size)
            {
                if (selectAppearing) { buffer[0] = 0; if (!Controller()) ImGui::SetKeyboardFocusHere(); }
                ImGui::SetNextItemWidth(-1);
                selectQueryChanged = TextInput(id, hint, buffer, size);
                ImGui::Dummy({0, 4 * scale});
            }
            // Options of a searchable select, `count` of them, in a well of their own:
            // they scroll beneath the filter instead of carrying it out of view, and a
            // new query shows its first match rather than the old scroll position.
            void SelectOptions(int count)
            {
                const float row = 34 * scale, gap = 2 * scale;
                if (!count) {
                    const auto at = ImGui::GetCursorScreenPos();
                    ImGui::Dummy({ImGui::GetContentRegionAvail().x, row});
                    ImGui::GetWindowDrawList()->AddText(fonts.body, Px(14), {at.x + 12 * scale, at.y + (row - Px(14)) / 2}, Palette::Muted, "No matches");
                    return;
                }
                const int shown = std::min(count, 12);
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
                ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0));
                Child("options", {0, shown * row + (shown - 1) * gap});
                if (selectQueryChanged) ImGui::SetScrollY(0);
                selectOptions = true;
            }
            bool SelectItem(const std::string& label, bool selected, bool keepOpen = false, bool enabled = true)
            {
                const auto at = ImGui::GetCursorScreenPos();
                const float width = ImGui::GetContentRegionAvail().x, height = 34 * scale;
                for (const auto color : {ImGuiCol_Header, ImGuiCol_HeaderHovered, ImGuiCol_HeaderActive}) ImGui::PushStyleColor(color, IM_COL32(0, 0, 0, 0));
                const bool pressed = ImGui::Selectable(("##" + label).c_str(), selected, (keepOpen ? ImGuiSelectableFlags_NoAutoClosePopups : 0) | (enabled ? 0 : ImGuiSelectableFlags_Disabled), {0, height});
                ImGui::PopStyleColor(3);
                // A Selectable only closes the popup it sits in directly, not one above its well.
                if (pressed && !keepOpen && selectOptions) ImGui::CloseCurrentPopup();
                if (selected && selectAppearing) ImGui::SetScrollHereY();
                auto* draw = ImGui::GetWindowDrawList();
                const bool lit = enabled && (ImGui::IsItemHovered() || ImGui::IsItemFocused());
                if (selected || lit) draw->AddRectFilled(at, {at.x + width, at.y + height}, selected ? IM_COL32(245, 158, 11, lit ? 40 : 26) : Palette::Fill2, 6 * scale);
                draw->AddText(selected ? fonts.medium : fonts.body, Px(14), {at.x + 12 * scale, at.y + (height - Px(14)) / 2}, !enabled ? Palette::Ghost : selected ? Palette::AmberLight : Palette::Text, label.c_str());
                if (selected) DrawIcon(draw, Icon::Check, {at.x + width - 18 * scale, at.y + height / 2}, 13 * scale, Palette::Amber);
                return pressed;
            }
            void Search(const char* hint = "Filter...")
            {
                ImGui::SetNextItemWidth(-1);
                TextInput("##search", hint, state.search.data(), state.search.size());
            }
            bool TextInput(const char* label, const char* hint, char* buffer, std::size_t size)
            {
                auto& context = *ImGui::GetCurrentContext();
                const auto flags = context.NavActivateFlags;
                if (Controller() && context.NavActivateId == ImGui::GetID(label)) context.NavActivateFlags |= ImGuiActivateFlags_PreferInput;
                const bool changed = hint ? ImGui::InputTextWithHint(label, hint, buffer, size) : ImGui::InputText(label, buffer, size);
                context.NavActivateFlags = flags;
                return changed;
            }
            void Confirm(std::string title, std::string message, std::string action, Model payload = Model())
            {
                state.confirmTitle = std::move(title);
                state.confirmMessage = std::move(message);
                state.pendingAction = std::move(action);
                state.pendingPayload = std::move(payload);
                state.confirmRequested = true;
            }
            void Leave()
            {
                state.controllerRotating = false;
                if (state.page == Page::Cycle) Emit(state.wigs ? "wiggyCancelCycle" : "tailorCancelCycle");
                if (state.outfitPreview) Emit("tailorCancelCreateOutfit");
                if (state.wigPreview) Emit("wiggyEndPreview");
                if (state.hairEditor) Emit("wiggyCloseHairColor", Model::object());
                state.outfitPreview = state.wigPreview = state.hairEditor = false;
                state.previewId = 0;
                state.createPreview = nullptr;
                state.pickerSituation = state.situation = 0;
                state.search.fill(0);
            }
            // The library's sub-pages reuse its search and category state for their own lists.
            static bool LibrarySubpage(Page page) { return page == Page::Create || page == Page::Categories || page == Page::Blacklist || page == Page::AddWigs; }
            void RestoreLibrary()
            {
                state.search = state.librarySearch;
                state.category = state.libraryCategory;
                state.categoryName = state.libraryCategoryName;
                state.libraryReveal = true;
            }
            void Navigate(Page page, bool wigs)
            {
                controllerFocusWork = Controller();
                if (page == Page::Settings && state.page != Page::Settings) {
                    state.settingsReturn = state.page == Page::Create ? Page::Library : state.page;
                    state.settingsReturnWigs = state.wigs;
                }
                const bool returning = page == Page::Library && LibrarySubpage(state.page) && state.libraryStashed && state.libraryWigs == wigs;
                if (state.page == Page::Library && LibrarySubpage(page) && state.wigs == wigs) {
                    state.librarySearch = state.search;
                    state.libraryCategory = state.category;
                    state.libraryCategoryName = state.categoryName;
                    state.libraryWigs = wigs;
                    state.libraryStashed = true;
                }
                Leave();
                state.page = page;
                state.wigs = wigs;
                state.category = -1;
                if (returning) RestoreLibrary();
                else if (page == Page::Library) { state.libraryStashed = false; state.libraryFocusId = 0; state.librarySex.reset(); }
                switch (page) {
                case Page::Library: if (!wigs) Emit("tailorRequestOutfits"); break;
                case Page::Discovered: if (!wigs) Emit("tailorRequestDiscovered"); break;
                case Page::Categories: Emit("tailorRequestAllCategories"); break;
                case Page::Blacklist: Emit(wigs ? "wiggyRequestBlacklist" : "tailorRequestBlacklist"); break;
                case Page::Situations: Emit(wigs ? "wiggyRequestWigSituations" : "tailorRequestSituations"); break;
                case Page::Export: case Page::Import: state.transferLoading = true; Emit("tailorRequestTransferData"); break;
                case Page::AddWigs: state.wigRowCategories.clear(); Emit("wiggyRequestMods"); break;
                case Page::HairColor: case Page::CustomColors:
                    Emit("wiggyOpenHairColor", Model::object()); state.hairEditor = true; break;
                default: break;
                }
            }
            void StartCycle(const Model& category, int situation = 0)
            {
                state.categoryName = Text(category, "displayName", Text(category, "name").c_str());
                state.situation = situation;
                state.pickerSituation = 0;
                state.page = Page::Cycle;
                if (state.wigs) Emit("wiggySelectCategory", {{"category", Number(category, "index")}});
                else {
                    Model payload = {{"id", Number(category, "id")}};
                    if (situation) payload["situation"] = situation;
                    Emit("tailorSelectCategory", payload);
                }
            }
            void Events()
            {
                if (Changed("wiggyDefaultHairResult")) {
                    state.defaultHairPending = false;
                    const auto& value = Get(model, "wiggyDefaultHairResult");
                    if (value.is_boolean() && value.get<bool>()) {
                        Navigate(Page::Main, true);
                        state.message = "Default hair restored; wigs removed"; state.messageDanger = false;
                    } else { state.message = "Could not remove all wigs. Please try Default Hair again."; state.messageDanger = true; }
                }
                if (Changed("tailorCycleUnavailable") && state.page == Page::Cycle) {
                    state.page = state.situation ? Page::Situations : Page::Main;
                    state.situation = 0;
                    state.message = "No matching outfits are available in this category"; state.messageDanger = false;
                }
                if (Changed("tailorSetOutfitData") && state.page == Page::Create) {
                    const auto& data = Get(model, "tailorSetOutfitData");
                    state.editId = Number(data, "outfitId");
                    SetName(Text(data, "name"));
                    state.items = Rows(Get(data, "items"));
                    // Loading the outfit replaced the NPC's preview items, so no row is on her now.
                    state.createPreview = nullptr;
                    state.outfitSex = SexOf(data);
                    state.categoryIds.clear();
                    for (const auto& id : Rows(Get(data, "categoryIds"))) if (id.is_number_integer()) state.categoryIds.insert(id.get<std::uint32_t>());
                    if (state.categoryIds.empty() && Number(data, "categoryId") > 0) state.categoryIds.insert(Number(data, "categoryId"));
                }
                if (Changed("tailorSetOutfitUsage") && state.deleteId) {
                    const auto& data = Get(model, "tailorSetOutfitUsage");
                    if (Number(data, "outfitId") == static_cast<int>(state.deleteId)) {
                        std::string message = "Delete this outfit?";
                        const auto& actors = Rows(Get(data, "actors"));
                        if (!actors.empty()) {
                            message = "This outfit is assigned to: ";
                            for (const auto& actor : actors) message += Text(actor, "name") + "; ";
                            message += "Deleting removes those assignments. Continue?";
                        }
                        Confirm("Delete Outfit", message, "tailorDeleteOutfit", {{"outfitId", state.deleteId}});
                        state.deleteId = 0;
                    }
                }
                if (Changed("tailorTransferResult")) {
                    state.transferPending = false;
                    const auto& data = Get(model, "tailorTransferResult");
                    state.transferError = Flag(data, "ok") ? "" : Text(data, "error", "Transfer failed. Please try again.");
                    state.exportSucceeded = Flag(data, "ok") && (Text(data, "operation") == "export" || data.contains("exported"));
                    state.messageDanger = !Flag(data, "ok");
                    if (!Flag(data, "ok")) state.message = state.transferError;
                    else if (Text(data, "operation") == "export" || data.contains("exported")) state.message = "Saved " + std::to_string(Number(data, "exported")) + " outfits to " + Text(data, "file");
                    else {
                        state.message = "Added " + std::to_string(Number(data, "added")) + "; duplicates " + std::to_string(Number(data, "duplicates"));
                        for (const auto& item : Rows(Get(data, "skipped"))) state.message += "\n" + Text(item, "name") + ": " + Text(item, "reason");
                    }
                }
                if (Changed("tailorSetTransferData")) {
                    state.transferLoading = false;
                    std::set<std::uint32_t> valid;
                    for (const auto& outfit : Rows(Get(Get(model, "tailorSetTransferData"), "outfits"))) valid.insert(Number(outfit, "id"));
                    std::erase_if(state.exportIds, [&](auto id) { return !valid.contains(id); });
                }
                if (Changed("wiggySetHairColor")) {
                    const auto& color = Get(model, "wiggySetHairColor");
                    if (!Flag(color, "isDefault")) {
                        state.rgb[0] = Number(color, "r") / 255.0f;
                        state.rgb[1] = Number(color, "g") / 255.0f;
                        state.rgb[2] = Number(color, "b") / 255.0f;
                    }
                }
                if (Changed("toast")) {
                    const auto& value = Get(model, "toast");
                    if (value.is_string()) state.message = value.get<std::string>();
                    else if (value.is_array() && !value.empty() && value[0].is_string()) state.message = value[0].get<std::string>();
                    else if (value.is_object()) state.message = Text(value, "message");
                    state.messageDanger = value.is_object() && Text(value, "kind") == "danger";
                }
            }
            // Shared 62px page header: optional Back, tracked ceremonial title, quiet
            // subtitle, round help. Content begins at y=80 on every page. Returns the
            // Back press so each page chooses where Back leads.
            bool Header(const char* title, bool back = false, const std::string& subtitle = "", Icon icon = Icon::None, bool help = true)
            {
                const auto origin = ImGui::GetWindowPos();
                auto* draw = ImGui::GetWindowDrawList();
                float x = 20 * scale;
                bool pressed = false;
                if (back) {
                    ImGui::SetCursorPos({x, 16 * scale});
                    // Its own ID: the hair pages also end in a plain "Back" action.
                    pressed = Press("Back##header", {78 * scale, 32 * scale}, Tone::Neutral, true, Icon::Back, 13);
                    x += 92 * scale;
                }
                if (icon != Icon::None) { DrawIcon(draw, icon, {origin.x + x + 8 * scale, origin.y + 32 * scale}, 16 * scale, Palette::Amber); x += 26 * scale; }
                Tracked(draw, {origin.x + x, origin.y + 32 * scale - Px(15) / 2}, title, fonts.heading, 15, Palette::Amber, 1.6f);
                x += TrackedWidth(title, fonts.heading, 15, 1.6f) + 12 * scale;
                if (!subtitle.empty()) draw->AddText(fonts.body, Px(13), {origin.x + x, origin.y + 32 * scale - Px(13) / 2}, Palette::Muted, subtitle.c_str());
                if (help && PageHelp()[0]) { ImGui::SetCursorPos({ImGui::GetWindowWidth() - 50 * scale, 17 * scale}); Help(PageHelp()); }
                draw->AddLine({origin.x, origin.y + 62 * scale}, {origin.x + ImGui::GetWindowWidth(), origin.y + 62 * scale}, Palette::Border);
                ImGui::SetCursorPos({20 * scale, 80 * scale});
                return pressed;
            }
            void Main()
            {
                const auto& target = Get(model, state.wigs ? "wiggySetTarget" : "tailorSetTarget");
                const auto name = Text(target, "name");
                const auto origin = ImGui::GetWindowPos();
                auto* draw = ImGui::GetWindowDrawList();
                // Seal: the tailor's mark in a gold-ruled tile.
                draw->AddRectFilled({origin.x + 24 * scale, origin.y + 20 * scale}, {origin.x + 56 * scale, origin.y + 52 * scale}, IM_COL32(245, 158, 11, 18), 8 * scale);
                draw->AddRect({origin.x + 24 * scale, origin.y + 20 * scale}, {origin.x + 56 * scale, origin.y + 52 * scale}, IM_COL32(245, 158, 11, 90), 8 * scale);
                const ImVec2 shirt[] = {{origin.x + 35 * scale, origin.y + 28 * scale}, {origin.x + 29 * scale, origin.y + 31 * scale}, {origin.x + 32 * scale, origin.y + 37 * scale}, {origin.x + 35 * scale, origin.y + 35 * scale}, {origin.x + 37 * scale, origin.y + 42 * scale}, {origin.x + 44 * scale, origin.y + 42 * scale}, {origin.x + 46 * scale, origin.y + 35 * scale}, {origin.x + 49 * scale, origin.y + 37 * scale}, {origin.x + 52 * scale, origin.y + 31 * scale}, {origin.x + 46 * scale, origin.y + 28 * scale}, {origin.x + 40 * scale, origin.y + 32 * scale}};
                draw->AddPolyline(shirt, 11, Palette::Amber, ImDrawFlags_Closed, 1.5f * scale);
                Tracked(draw, {origin.x + 70 * scale, origin.y + 13 * scale}, "TAILOR", fonts.heading, 19, Palette::Amber, 1.2f);
                draw->AddText(fonts.medium, Px(12), {origin.x + 70 * scale, origin.y + 38 * scale}, Palette::Muted, name.c_str());
                const char* kicker = state.wigs ? "TAILOR / WIGS" : "TAILOR / OUTFITS";
                Tracked(draw, {origin.x + ImGui::GetWindowWidth() - 24 * scale - TrackedWidth(kicker, fonts.bold, 11, 2.2f), origin.y + 30 * scale}, kicker, fonts.bold, 11, IM_COL32(184, 134, 58, 255), 2.2f);
                draw->AddLine({origin.x, origin.y + 70 * scale}, {origin.x + ImGui::GetWindowWidth(), origin.y + 70 * scale}, Palette::Border);
                ImGui::SetCursorPos(ImVec2(24 * scale, 91 * scale));
                if (name.empty()) {
                    ImGui::Dummy(ImVec2(0, 28 * scale));
                    const auto notice = Text(target, "notice");
                    Wrapped(notice.empty() ? "Target an NPC to get started.\nLook at an NPC, then reopen this menu." : notice, Palette::Muted);
                    return;
                }
                // Garment label: gold border with heat behind it, and a running stitch
                // sewn just inside the edge. The one place the tailor's hand shows.
                const ImVec2 tagMin{origin.x + 24 * scale, origin.y + 91 * scale}, tagMax{origin.x + ImGui::GetWindowWidth() - 24 * scale, origin.y + 145 * scale};
                for (int ring = 3; ring >= 1; --ring) draw->AddRect({tagMin.x - ring * 2 * scale, tagMin.y - ring * 2 * scale}, {tagMax.x + ring * 2 * scale, tagMax.y + ring * 2 * scale}, IM_COL32(245, 158, 11, 9), (10 + ring * 2) * scale, 0, 2 * scale);
                draw->AddRectFilled(tagMin, tagMax, IM_COL32(10, 8, 7, 120), 10 * scale);
                draw->AddRect(tagMin, tagMax, IM_COL32(245, 158, 11, 76), 10 * scale);
                for (float x = tagMin.x + 12 * scale; x < tagMax.x - 16 * scale; x += 9 * scale) {
                    draw->AddLine({x, tagMin.y + 5 * scale}, {x + 5 * scale, tagMin.y + 5 * scale}, IM_COL32(245, 158, 11, 58), scale);
                    draw->AddLine({x, tagMax.y - 5 * scale}, {x + 5 * scale, tagMax.y - 5 * scale}, IM_COL32(245, 158, 11, 58), scale);
                }
                ImGui::SetCursorPos({44 * scale, 118 * scale - Px(11) / 2});
                Eyebrow("CURRENTLY WEARING");
                const auto wearing = Text(target, state.wigs ? "currentWig" : "currentOutfit");
                const auto eyebrowEnd = ImGui::GetItemRectMax();
                draw->AddText(fonts.bold, Px(16), {eyebrowEnd.x + 12 * scale, origin.y + 118 * scale - Px(16) / 2}, Palette::Text, (wearing.empty() ? (state.wigs ? "Default Hair" : Flag(target, "isPlayer") ? "Own Gear" : "Default Outfit") : wearing).c_str());
                if (state.wigs && Flag(target, "isNFFManaged")) { ImGui::SetCursorPos({24 * scale, 150 * scale}); Label("NFF managed: wig may reset during outfit changes", Palette::CopperLight, 12); }
                auto categories = Rows(Get(model, state.wigs ? "wiggySetCategories" : "tailorSetCategories"));
                if (!state.wigs) {
                    // Situation pools are assigned from Situations; the dressing list shows wardrobes only.
                    categories.erase(std::remove_if(categories.begin(), categories.end(), [](const auto& category) { return !Text(category, "situationType").empty(); }), categories.end());
                    SortByName(categories);
                    ImGui::SetCursorPos({24 * scale, 176 * scale - Px(11) / 2});
                    Eyebrow("CATEGORIES", std::to_string(categories.size()) + " categories");
                    ImGui::SetCursorPos({ImGui::GetWindowWidth() - 227 * scale, 156 * scale});
                    ImGui::SetNextItemWidth(203 * scale);
                    TextInput("##search", "Filter...", state.search.data(), state.search.size());
                    // Pinned column header above the scrolling rows.
                    const float listTop = 205 * scale, headerHeight = 34 * scale, listBottom = ImGui::GetWindowHeight() - 102 * scale;
                    const ImVec2 wellMin{origin.x + 24 * scale, origin.y + listTop}, wellMax{origin.x + ImGui::GetWindowWidth() - 24 * scale, origin.y + listBottom};
                    draw->AddRectFilled(wellMin, wellMax, IM_COL32(10, 8, 7, 105), 10 * scale);
                    draw->AddRectFilled(wellMin, {wellMax.x, wellMin.y + headerHeight}, IM_COL32(10, 8, 7, 150), 10 * scale, ImDrawFlags_RoundCornersTop);
                    draw->AddLine({wellMin.x, wellMin.y + headerHeight}, {wellMax.x, wellMin.y + headerHeight}, Palette::Border);
                    draw->AddRect(wellMin, wellMax, Palette::Border, 10 * scale);
                    Tracked(draw, {wellMin.x + 16 * scale, wellMin.y + (headerHeight - Px(10)) / 2}, "CATEGORY", fonts.bold, 10, Palette::Muted, 2.0f);
                    Tracked(draw, {wellMax.x - 110 * scale, wellMin.y + (headerHeight - Px(10)) / 2}, "OUTFITS", fonts.bold, 10, Palette::Muted, 2.0f);
                    ImGui::SetCursorPos({24 * scale, listTop + headerHeight});
                    BeginList("categories", {ImGui::GetWindowWidth() - 48 * scale, std::max(60 * scale, listBottom - listTop - headerHeight - scale)}, false);
                    bool any = false;
                    for (const auto& category : categories) {
                        const auto catName = Text(category, "displayName", Text(category, "name").c_str());
                        if (!Matches(catName, state.search.data())) continue;
                        any = true;
                        ImGui::PushID(Number(category, "id"));
                        // What the target can wear: a category holding only the other sex's outfits greys out.
                        const int count = Number(category, "fitCount", Number(category, "outfitCount"));
                        ImVec2 row; float rowWidth;
                        ImGui::BeginDisabled(count == 0);
                        if (Row("##category", 47 * scale, false, row, rowWidth)) StartCycle(category);
                        if (count > 0) DefaultControllerFocus();
                        const bool lit = count && (ImGui::IsItemHovered() || ImGui::IsItemFocused());
                        auto* rows = ImGui::GetWindowDrawList();
                        rows->AddText(fonts.bold, Px(15), {row.x + 16 * scale, row.y + (47 * scale - Px(15)) / 2}, count ? Palette::Text : Palette::Ghost, catName.c_str());
                        const auto countText = std::to_string(count);
                        const float countWidth = fonts.bold->CalcTextSizeA(Px(14), FLT_MAX, 0, countText.c_str()).x;
                        rows->AddText(fonts.bold, Px(14), {row.x + rowWidth - 82 * scale - countWidth / 2, row.y + (47 * scale - Px(14)) / 2}, count ? Palette::Amber : Palette::Ghost, countText.c_str());
                        DrawIcon(rows, Icon::ChevronRight, {row.x + rowWidth - 20 * scale, row.y + 23.5f * scale}, 11 * scale, lit ? Palette::Amber : Palette::Ghost);
                        ImGui::EndDisabled();
                        ImGui::PopID();
                    }
                    if (!any) { ImGui::SetCursorPos({16 * scale, 18 * scale}); Label(categories.empty() ? "No categories yet. Create one from Manage Outfits." : "No category matches that filter.", Palette::Muted, 13); }
                    EndList();
                    const float bar = BeginActions(), third = (bar - 20 * scale) / 3;
                    if (Action("Reset Outfit", third, Tone::Danger, true, Icon::RotateLeft)) Emit("tailorResetOutfit");
                    ImGui::SameLine(); if (Action("Situations", third, Tone::Neutral, true, Icon::Layers)) Navigate(Page::Situations, false);
                    ImGui::SameLine(); if (Action("Manage Outfits", third, Tone::Neutral, true, Icon::Grid)) Navigate(Page::Library, false);
                } else {
                    ImGui::SetCursorPos({24 * scale, 170 * scale});
                    for (int sex = 0; sex < 2; ++sex) {
                        const auto targetSex = Text(target, "sex");
                        if ((sex == 0 && targetSex == "male") || (sex == 1 && targetSex == "female")) continue;
                        ImGui::SetCursorPosX(24 * scale);
                        Eyebrow(sex == 0 ? "FEMALE" : "MALE");
                        ImGui::Dummy({0, 2 * scale});
                        int column = 0;
                        const float width = (ImGui::GetWindowWidth() - 48 * scale - 30 * scale) / 4;
                        for (const auto& category : categories) {
                            const int index = Number(category, "index");
                            if ((index < 4 ? 0 : 1) != sex || !Matches(Text(category, "name"), state.search.data())) continue;
                            ImGui::PushID(index);
                            if (column++ % 4) ImGui::SameLine(); else ImGui::SetCursorPosX(24 * scale);
                            auto caption = Text(category, "name");
                            if (caption.starts_with("Female - ")) caption.erase(0, 9);
                            if (caption.starts_with("Male - ")) caption.erase(0, 7);
                            const auto pos = ImGui::GetCursorScreenPos();
                            const int count = Number(category, "count");
                            ImGui::BeginDisabled(count == 0);
                            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 10 * scale);
                            if (ImGui::Button("##wigcategory", ImVec2(width, 53 * scale))) StartCycle(category);
                            ImGui::PopStyleVar();
                            if (count > 0) DefaultControllerFocus();
                            const bool lit = count && (ImGui::IsItemHovered() || ImGui::IsItemFocused());
                            if (lit) draw->AddRect(pos, {pos.x + width, pos.y + 53 * scale}, IM_COL32(245, 158, 11, 110), 10 * scale);
                            draw->AddText(fonts.bold, Px(15), {pos.x + 18 * scale, pos.y + (53 * scale - Px(15)) / 2}, count ? Palette::Text : Palette::Ghost, caption.c_str());
                            const auto number = std::to_string(count);
                            const float numberWidth = fonts.bold->CalcTextSizeA(Px(13), FLT_MAX, 0, number.c_str()).x;
                            const ImVec2 disc{pos.x + width - 30 * scale, pos.y + 26.5f * scale};
                            draw->AddCircleFilled(disc, 14 * scale, IM_COL32(245, 158, 11, count ? 26 : 10), 28);
                            draw->AddText(fonts.bold, Px(13), {disc.x - numberWidth / 2, disc.y - Px(13) / 2}, count ? Palette::Amber : Palette::Ghost, number.c_str());
                            ImGui::EndDisabled(); ImGui::PopID();
                        }
                        ImGui::Dummy({0, 12 * scale});
                    }
                    const float bar = BeginActions(), quarter = (bar - 30 * scale) / 4;
                    if (Action("Default Hair", quarter, Tone::Danger, !state.defaultHairPending, Icon::RotateLeft)) { Emit("wiggyResetWig"); state.defaultHairPending = true; }
                    ImGui::SameLine(); if (Action("Hair Color", quarter)) Navigate(Page::HairColor, true);
                    ImGui::SameLine(); if (Action("Situations", quarter, Tone::Neutral, true, Icon::Layers)) Navigate(Page::Situations, true);
                    ImGui::SameLine(); if (Action("Manage Wigs", quarter, Tone::Neutral, true, Icon::Grid)) Navigate(Page::Library, true);
                }
            }
            void Cycle()
            {
                Header(state.wigs ? "HAIR DRESSER" : "DRESSING ROOM", false, state.categoryName);
                const auto& data = Get(model, state.wigs ? "wiggySetCycleState" : "tailorSetCycleState");
                const auto origin = ImGui::GetWindowPos();
                auto* draw = ImGui::GetWindowDrawList();
                const float middle = ImGui::GetWindowWidth() / 2;
                const int index = Number(data, "index"), total = Number(data, "total"), rating = Number(data, "armorRating");
                // Hero: the garment on the stand. Name, rating disc, position in the rack.
                const auto title = Text(data, "name", "---");
                const float titleWidth = fonts.bold->CalcTextSizeA(Px(30), FLT_MAX, 0, title.c_str()).x, disc = rating > 0 ? 44 * scale : 0;
                const float titleX = std::max(24 * scale, middle - (titleWidth + disc) / 2);
                draw->AddText(fonts.bold, Px(30), {origin.x + titleX, origin.y + 106 * scale}, Palette::Text, title.c_str());
                if (rating > 0) Rating(draw, {origin.x + titleX + titleWidth + 28 * scale, origin.y + 106 * scale + Px(30) / 2}, rating);
                const auto position = std::to_string(index + 1) + (state.wigs ? " / " : " of ") + std::to_string(total);
                draw->AddText(fonts.medium, Px(15), {origin.x + middle - fonts.medium->CalcTextSizeA(Px(15), FLT_MAX, 0, position.c_str()).x / 2, origin.y + 160 * scale}, Palette::Muted, position.c_str());
                // Rack pips: one per garment while they fit, the current one lit.
                if (total > 1 && total <= 24) for (int pip = 0; pip < total; ++pip) {
                    const float x = origin.x + middle + (pip - (total - 1) / 2.0f) * 12 * scale;
                    draw->AddCircleFilled({x, origin.y + 194 * scale}, (pip == index ? 3.5f : 2.0f) * scale, pip == index ? Palette::Amber : Palette::Line3, 12);
                }
                ImGui::SetCursorPos(ImVec2(middle - 200 * scale, 210 * scale));
                ImGui::SetNextItemWidth(400 * scale);
                if (BeginSelect("##cycleSearch", state.wigs ? "Jump to a wig..." : "Jump to an outfit...", true)) {
                    SelectSearch("##search", state.wigs ? "Search wigs..." : "Search outfits...", state.search.data(), state.search.size());
                    const auto& rack = Rows(Get(data, "items"));
                    SelectOptions(static_cast<int>(std::count_if(rack.begin(), rack.end(), [&](const Model& item) { return Matches(Text(item, "name"), state.search.data()); })));
                    for (const auto& item : rack) {
                        if (!Matches(Text(item, "name"), state.search.data())) continue;
                        ImGui::PushID(Number(item, "index"));
                        if (SelectItem(Text(item, "name"), Number(item, "index") == index)) Emit(state.wigs ? "wiggyCycleToIndex" : "tailorCycleToIndex", {{"index", Number(item, "index")}});
                        ImGui::PopID();
                    }
                    EndSelect();
                }
                const float actionsTop = ImGui::GetWindowHeight() - 86 * scale;
                ImGui::SetCursorPos(ImVec2(middle - 72 * scale, actionsTop - 118 * scale));
                if (Press("##cyclePrev", {62 * scale, 62 * scale}, Tone::Neutral, total > 1, Icon::ChevronLeft, 18)) Emit(state.wigs ? "wiggyCyclePrev" : "tailorCyclePrev");
                ImGui::SameLine(0, 20 * scale);
                if (Press("##cycleNext", {62 * scale, 62 * scale}, Tone::Neutral, total > 1, Icon::ChevronRight, 18)) Emit(state.wigs ? "wiggyCycleNext" : "tailorCycleNext");
                if (!Controller()) {
                    const char* hint = "Arrow keys or A/D to cycle  \xC2\xB7  Enter to confirm";
                    draw->AddText(fonts.body, Px(13), {origin.x + middle - fonts.body->CalcTextSizeA(Px(13), FLT_MAX, 0, hint).x / 2, origin.y + actionsTop - 36 * scale}, Palette::Muted, hint);
                }
                const bool keyboard = interactive && !ImGui::GetIO().WantTextInput && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);
                if (keyboard && (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) || ImGui::IsKeyPressed(ImGuiKey_A))) Emit(state.wigs ? "wiggyCyclePrev" : "tailorCyclePrev");
                if (keyboard && (ImGui::IsKeyPressed(ImGuiKey_RightArrow) || ImGui::IsKeyPressed(ImGuiKey_D))) Emit(state.wigs ? "wiggyCycleNext" : "tailorCycleNext");
                const float bar = BeginActions(), third = (bar - 20 * scale) / 3;
                const bool confirm = Action(state.wigs ? "Set Wig" : "Set Outfit", third, Tone::Primary, total > 0);
                if (confirm || (keyboard && ImGui::IsKeyPressed(ImGuiKey_Enter))) ConfirmCycle();
                ImGui::SameLine(); if (Action("Cancel", third)) { const auto page = state.situation ? Page::Situations : Page::Main; Navigate(page, state.wigs); }
                ImGui::SameLine(); if (Action(state.wigs ? "Default Hair" : "Reset to Default", third, Tone::Danger, !state.defaultHairPending)) {
                    if (state.wigs) { Emit("wiggyResetWig"); state.defaultHairPending = true; }
                    else { Emit("tailorCancelCycle"); Emit("tailorResetOutfit"); state.page = Page::Main; }
                }
            }
            void Edit(std::uint32_t id)
            {
                Navigate(Page::Create, false);
                state.libraryFocusId = id;
                state.editId = id; ForgetName(); state.items = Model::array(); state.categoryIds.clear(); state.outfitSex = -1;
                state.selectedPlugin.clear(); state.slotFilter.clear();
                state.pluginSearch.fill(0);
                state.outfitPreview = true;
                Emit("tailorBeginCreateOutfit"); Emit("tailorRequestArmorPlugins");
                if (id) Emit("tailorRequestOutfitData", {{"outfitId", id}});
            }
            void Library()
            {
                if (Header(state.wigs ? "MANAGE WIGS" : "MANAGE OUTFITS", true, "", Icon::Grid)) Navigate(Page::Main, state.wigs);
                const float content = ImGui::GetWindowWidth() - 40 * scale, listBottom = ImGui::GetWindowHeight() - 102 * scale;
                if (state.wigs) {
                    const auto& cats = Rows(Get(model, "wiggySetCategories"));
                    // Category tabs: four to a row, count on the right, the open one lit.
                    int column = 0;
                    const float tab = (content - 18 * scale) / 4;
                    for (const auto& cat : cats) {
                        if (column++ % 4) ImGui::SameLine(0, 6 * scale); else ImGui::SetCursorPosX(20 * scale);
                        auto name = Text(cat, "name");
                        if (name.starts_with("Female - ")) name.replace(0, 9, "F \xC2\xB7 ");
                        if (name.starts_with("Male - ")) name.replace(0, 7, "M \xC2\xB7 ");
                        const int index = Number(cat, "index");
                        ImGui::PushID(index);
                        const auto at = ImGui::GetCursorScreenPos();
                        if (Press("##wigTab", {tab, 36 * scale}, state.category == index ? Tone::Amber : Tone::Neutral)) {
                            if (state.wigPreview) Emit("wiggyEndPreview");
                            state.wigPreview = false; state.category = index; state.categoryName = Text(cat, "name");
                        }
                        auto* draw = ImGui::GetWindowDrawList();
                        const auto count = std::to_string(Number(cat, "count"));
                        draw->AddText(fonts.medium, Px(13), {at.x + 12 * scale, at.y + (36 * scale - Px(13)) / 2}, state.category == index ? Palette::AmberLight : Palette::Text, name.c_str());
                        draw->AddText(fonts.bold, Px(12), {at.x + tab - 12 * scale - fonts.bold->CalcTextSizeA(Px(12), FLT_MAX, 0, count.c_str()).x, at.y + (36 * scale - Px(12)) / 2}, state.category == index ? Palette::Amber : Palette::Muted, count.c_str());
                        ImGui::PopID();
                    }
                    ImGui::SetCursorPosX(20 * scale); ImGui::SetNextItemWidth(content);
                    TextInput("##search", "Filter wigs...", state.search.data(), state.search.size());
                    ImGui::SetCursorPosX(20 * scale);
                    BeginList("wigLibraryList", {content, std::max(100 * scale, listBottom - ImGui::GetCursorPosY())});
                    if (state.category < 0) { ImGui::SetCursorPos({18 * scale, 18 * scale}); Label("Choose a category above to see its wigs.", Palette::Muted, 13); }
                    for (const auto& cat : cats) {
                        const int category = Number(cat, "index");
                        if (state.category != category) continue;
                        ImGui::PushID(category);
                        int shown = 0;
                        for (const auto& wig : Rows(Get(cat, "wigs"))) {
                            if (!Matches(Text(wig, "name"), state.search.data())) continue;
                            ImGui::PushID(Serialize(wig).c_str());
                            ImVec2 row; float rowWidth; const float rowHeight = 48 * scale;
                            if (Row("##wig", rowHeight, false, row, rowWidth, true, shown++ % 2)) { Emit("wiggyPreviewWig", wig); state.wigPreview = true; }
                            ImGui::GetWindowDrawList()->AddText(fonts.medium, Px(14), {row.x + 16 * scale, row.y + (rowHeight - Px(14)) / 2}, Palette::Text, Text(wig, "name").c_str());
                            ImGui::SetCursorScreenPos({row.x + rowWidth - 206 * scale, row.y + 4 * scale});
                            ImGui::SetNextItemWidth(150 * scale);
                            if (BeginSelect("##move", "Move to...", true)) {
                                for (const auto& dest : cats) if (Number(dest, "index") != category && SelectItem(Text(dest, "name"), false)) {
                                    auto payload = wig; payload["fromCategory"] = category; payload["toCategory"] = Number(dest, "index"); Emit("wiggyMoveWig", payload);
                                }
                                EndSelect();
                            }
                            ImGui::SetCursorScreenPos({row.x + rowWidth - 46 * scale, row.y + 9 * scale});
                            if (Small("##remove", Tone::Danger, true, Icon::Close)) { auto payload = wig; payload["category"] = category; Emit("wiggyRemoveWig", payload); }
                            ImGui::SetCursorScreenPos({row.x, row.y + rowHeight}); ImGui::Dummy({0, 0});
                            ImGui::PopID();
                        }
                        if (!shown) { ImGui::SetCursorPos({18 * scale, 18 * scale}); Label("No wigs here yet. Add some from your installed mods.", Palette::Muted, 13); }
                        ImGui::PopID();
                    }
                    EndList();
                } else {
                    // Saves are off: say why in the band between the header rule and the filters, as the editor says a
                    // name is taken, so the rows below (which the screen tests click by position) don't move.
                    if (const auto note = Text(Get(model, "tailorSetLibraryState"), "note"); !note.empty()) {
                        const auto at = ImGui::GetCursorScreenPos();
                        const ImVec4 clip{at.x, at.y - 18 * scale, at.x + content, at.y};
                        ImGui::GetWindowDrawList()->AddText(fonts.body, Px(12), {at.x, at.y - 15 * scale}, Palette::DangerText, note.c_str(), nullptr, 0, &clip);
                    }
                    // Category, Sex and search share the row; the Sex filter needs the least room.
                    const float fields = content - 32 * scale, categoryWidth = fields * 0.38f, sexWidth = fields * 0.24f;
                    ImGui::SetNextItemWidth(categoryWidth);
                    if (BeginSelect("##libraryCategory", state.category < 0 ? "All Categories" : state.categoryName)) {
                        if (SelectItem("All Categories", state.category < 0)) state.category = -1;
                        for (const auto& cat : Rows(Get(model, "tailorSetCategories"))) {
                            ImGui::PushID(Number(cat, "id"));
                            const auto label = Text(cat, "displayName", Text(cat, "name").c_str());
                            if (SelectItem(label, state.category == Number(cat, "id"))) { state.category = Number(cat, "id"); state.categoryName = label; }
                            ImGui::PopID();
                        }
                        EndSelect();
                    }
                    ImGui::SameLine(0, 16 * scale); ImGui::SetNextItemWidth(sexWidth);
                    if (BeginSelect("##librarySex", state.librarySex ? SexLabel(*state.librarySex) : "All Sexes", false, state.librarySex ? SexInk(*state.librarySex) : 0)) {
                        if (SelectItem("All Sexes", !state.librarySex)) state.librarySex.reset();
                        for (const int sex : {-1, 1, 0}) if (SelectItem(SexLabel(sex), state.librarySex == sex)) state.librarySex = sex;
                        EndSelect();
                    }
                    ImGui::SameLine(0, 16 * scale); ImGui::SetNextItemWidth(fields - categoryWidth - sexWidth);
                    TextInput("##search", "Search outfits...", state.search.data(), state.search.size());
                    ImGui::SetCursorPosX(20 * scale);
                    BeginList("outfitLibrary", {content, std::max(100 * scale, listBottom - ImGui::GetCursorPosY())});
                    auto outfits = Rows(Get(model, "tailorSetOutfits"));
                    SortByName(outfits);
                    if (state.libraryRevealOnRefresh && Changed("tailorSetOutfits")) { state.libraryReveal = true; state.libraryRevealOnRefresh = false; }
                    int shown = 0;
                    std::vector<int> shownIds;
                    const int targetSex = TargetSex(Get(model, "tailorSetTarget"));
                    for (const auto& outfit : outfits) {
                        if (!Matches(Text(outfit, "name"), state.search.data())) continue;
                        if (state.librarySex && SexOf(outfit) != *state.librarySex) continue;
                        if (state.category >= 0) {
                            const auto& ids = Rows(Get(outfit, "categoryIds"));
                            const auto& names = Rows(Get(outfit, "categories"));
                            if (std::find(ids.begin(), ids.end(), Model(state.category)) == ids.end() && std::find(names.begin(), names.end(), Model(state.categoryName)) == names.end()) continue;
                        }
                        const auto id = static_cast<std::uint32_t>(Number(outfit, "id"));
                        shownIds.push_back(static_cast<int>(id));
                        ImGui::PushID(static_cast<int>(id));
                        // One row: name over its category tags; rating and actions ride the right edge.
                        ImVec2 row; float rowWidth; const float rowHeight = 72 * scale;
                        const bool edited = !state.previewId && state.libraryFocusId == id;
                        if (Row("##outfit", rowHeight, state.previewId == id || edited, row, rowWidth, true, shown++ % 2)) {
                            if (!OutfitFits(static_cast<OutfitSex>(SexOf(outfit)), targetSex)) {
                                // Tagged for the other sex: say so instead of dressing the target in it.
                                state.message = "Outfit can't be previewed by the currently selected NPC due to gender";
                                state.messageDanger = false;
                            } else {
                                if (!state.outfitPreview) Emit("tailorBeginCreateOutfit");
                                state.outfitPreview = true; state.previewId = id; state.libraryFocusId = 0;
                                Emit("tailorPreviewOutfit", {{"outfitId", id}});
                            }
                        }
                        if (edited && state.libraryReveal) {
                            if (!ImGui::IsItemVisible()) ImGui::SetScrollHereY(0.5f);
                            DefaultControllerFocus();
                        }
                        auto* draw = ImGui::GetWindowDrawList();
                        draw->AddText(fonts.medium, Px(15), {row.x + 16 * scale, row.y + 13 * scale}, Flag(outfit, "hasEnchanted") ? Palette::Emerald : Palette::Text, Text(outfit, "name").c_str());
                        ImVec2 chip{row.x + 16 * scale, row.y + 42 * scale};
                        SexTag(draw, chip, SexOf(outfit));
                        for (const auto& value : Rows(Get(outfit, "categories"))) if (value.is_string()) Chip(draw, chip, value.get<std::string>());
                        float actionsWidth = 16 * scale + 3 * 28 * scale + 2 * 8 * scale;
                        for (const char* word : {"Edit", "Copy", "Delete"}) actionsWidth += fonts.medium->CalcTextSizeA(Px(13), FLT_MAX, 0, word).x;
                        const float actions = row.x + rowWidth - actionsWidth;
                        if (const int rating = Number(outfit, "armorRating"); rating > 0) Rating(draw, {actions - 24 * scale, row.y + 26 * scale}, rating);
                        ImGui::SetCursorScreenPos({actions, row.y + 11 * scale});
                        if (Small("Edit", Tone::Amber)) Edit(id);
                        ImGui::SameLine(0, 8 * scale); if (Small("Copy", Tone::Neutral, !LibraryReadOnly())) {
                            state.pendingAction = "tailorCopyOutfit"; state.pendingPayload = {{"outfitId", id}};
                            SetName(CopyName(outfits, Text(outfit, "name"))); state.promptRequested = true;
                        }
                        ImGui::SameLine(0, 8 * scale); if (Small("Delete", Tone::Danger, !LibraryReadOnly())) { state.deleteId = id; Emit("tailorCheckOutfitUsage", {{"outfitId", id}}); }
                        ImGui::SetCursorScreenPos({row.x, row.y + rowHeight}); ImGui::Dummy({0, 0});
                        ImGui::PopID();
                    }
                    if (!shown) { ImGui::SetCursorPos({18 * scale, 18 * scale}); Label(outfits.empty() ? "No outfits yet. Create one to start a wardrobe." : "No outfit matches that search.", Palette::Muted, 13); }
                    EndList();
                    // Set Sex rides the header's free space. It tags every outfit the filters
                    // above show, so it takes its count from the list just drawn.
                    const auto resume = ImGui::GetCursorPos();
                    const auto count = std::to_string(shownIds.size());
                    const char* noun = shownIds.size() == 1 ? " outfit" : " outfits";
                    // Labelled like the Adventuring card's ARMOR TYPE: tracked caps, centred on the select.
                    const float setSexX = ImGui::GetWindowWidth() - 290 * scale;
                    const auto header = ImGui::GetWindowPos();
                    Tracked(ImGui::GetWindowDrawList(), {header.x + setSexX - 12 * scale - TrackedWidth("GENDER", fonts.bold, 10, 1.8f), header.y + 31 * scale - Px(10) / 2},
                        "GENDER", fonts.bold, 10, Palette::Muted, 1.8f);
                    ImGui::SetCursorPos({setSexX, 15 * scale});
                    ImGui::SetNextItemWidth(228 * scale);
                    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {14 * scale, (32 * scale - Px(14)) / 2});
                    ImGui::BeginDisabled(shownIds.empty() || LibraryReadOnly());
                    if (BeginSelect("##setSex", "Set " + count + noun + " to...", true)) {
                        for (const int sex : {-1, 1, 0}) if (SelectItem(SexLabel(sex), false)) {
                            Confirm("Set Sex", "Set the " + count + noun + " shown to " + SexLabel(sex) + "?", "tailorSetOutfitSex", {{"outfitIds", shownIds}, {"sex", sex}});
                        }
                        EndSelect();
                    }
                    ImGui::EndDisabled();
                    ImGui::PopStyleVar();
                    ImGui::SetCursorPos(resume);
                }
                state.libraryReveal = false;
                const int count = state.wigs ? 2 : 3;
                const float bar = BeginActions(), buttonWidth = (bar - (count - 1) * 10 * scale) / count;
                const bool create = Action(state.wigs ? "Add from Mods" : "Create Outfit", buttonWidth, Tone::Primary, state.wigs || !LibraryReadOnly());
                if (create) { if (state.wigs) Navigate(Page::AddWigs, true); else Edit(0); }
                if (!state.wigs) { ImGui::SameLine(); if (Action("Categories", buttonWidth)) Navigate(Page::Categories, false); }
                ImGui::SameLine(); if (Action(state.wigs ? "Blacklist Mods" : "Blacklist Plugins", buttonWidth)) Navigate(Page::Blacklist, state.wigs);
            }
            void Create()
            {
                const float fullWidth = ImGui::GetWindowWidth();
                const float fullHeight = ImGui::GetWindowHeight();
                const float primary = fullWidth * (840.0f / 1380.0f);
                const auto& armorData = Get(model, "tailorSetArmorForPlugin");
                // Tooltip listing an item's enchantments, shared by both panes.
                const auto effects = [&](const Model& item) {
                    if (!ImGui::IsItemHovered() || Rows(Get(item, "enchantments")).empty()) return;
                    ImGui::BeginTooltip();
                    for (const auto& effect : Rows(Get(item, "enchantments"))) if (effect.is_string()) Label(effect.get<std::string>(), Palette::Emerald, 13);
                    ImGui::EndTooltip();
                };
                const auto meta = [](const Model& item) { return Text(item, "type") + "  \xC2\xB7  " + std::to_string(Number(item, "armorRating")) + " armor"; };
                ImGui::SetCursorPos(ImVec2(0, 0));
                ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0));
                // Borderless children drop WindowPadding unless asked to keep it.
                Child("Create Browser", ImVec2(primary, fullHeight), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
                if (Header(state.editId ? "EDIT OUTFIT" : "CREATE OUTFIT", true, "", Icon::Plus)) Navigate(Page::Library, false);
                const float content = primary - 40 * scale;
                ImGui::SetNextItemWidth(content);
                TextInput("##name", "Outfit name...", state.name.data(), state.name.size());
                // A name another outfit has keeps Save off, so nothing leaves the editor. The rows below the
                // field are pinned, so the message takes the band between the header rule and the field.
                const bool nameTaken = OutfitNameTaken(Rows(Get(model, "tailorSetOutfits")), EffectiveName(), static_cast<int>(state.editId));
                if (nameTaken) {
                    const auto box = ImGui::GetItemRectMin();
                    const auto message = OutfitTakenMessage(EffectiveName());
                    const ImVec4 clip{box.x, box.y - 18 * scale, box.x + content, box.y};
                    ImGui::GetWindowDrawList()->AddText(fonts.body, Px(12), {box.x, box.y - 15 * scale}, Palette::DangerText, message.c_str(), nullptr, 0, &clip);
                }
                ImGui::SetCursorPosX(20 * scale); ImGui::SetNextItemWidth(content);
                if (BeginSelect("##plugin", state.selectedPlugin.empty() ? "Choose an armor mod..." : state.selectedPlugin, state.selectedPlugin.empty())) {
                    SelectSearch("##pluginSearch", "Search mods...", state.pluginSearch.data(), state.pluginSearch.size());
                    const auto& plugins = Rows(Get(model, "tailorSetArmorPlugins"));
                    const auto listed = [&](const Model& plugin) { return plugin.is_string() && Matches(plugin.get<std::string>(), state.pluginSearch.data()); };
                    SelectOptions(static_cast<int>(std::count_if(plugins.begin(), plugins.end(), listed)));
                    for (const auto& plugin : plugins) {
                        if (!listed(plugin)) continue;
                        const auto name = plugin.get<std::string>();
                        if (SelectItem(name, name == state.selectedPlugin)) {
                            // The previewed piece's row leaves with its mod, so take the piece off.
                            if (name != state.selectedPlugin && state.createPreview.is_object()) {
                                Emit("tailorUnequipArmorItem", state.createPreview);
                                state.createPreview = nullptr;
                            }
                            state.selectedPlugin = name; state.search.fill(0);
                            Emit("tailorRequestArmorForPlugin", {{"plugin", name}});
                        }
                    }
                    EndSelect();
                }
                ImGui::SetCursorPos({20 * scale, 186 * scale}); Eyebrow("AVAILABLE ARMOR");
                ImGui::SetCursorPos({20 * scale, 206 * scale}); ImGui::SetNextItemWidth(content);
                TextInput("##search", "Search items in this mod...", state.search.data(), state.search.size());
                ImGui::SetCursorPos({20 * scale, 256 * scale});
                BeginList("armor", {content, std::max(100 * scale, fullHeight - 102 * scale - 256 * scale)});
                int available = 0;
                if (Text(armorData, "plugin") == state.selectedPlugin && !state.selectedPlugin.empty()) for (const auto& armor : Rows(Get(armorData, "armors"))) {
                    if (!Matches(Text(armor, "name"), state.search.data()) || (!state.slotFilter.empty() && Text(armor, "slot") != state.slotFilter)) continue;
                    const auto match = [&](const Model& item) { return Number(item, "formId") == Number(armor, "formId") && Text(item, "plugin") == Text(armor, "plugin"); };
                    if (std::any_of(state.items.begin(), state.items.end(), match)) continue;
                    ImGui::PushID(Serialize(armor).c_str());
                    ImVec2 row; float rowWidth; const float rowHeight = 56 * scale;
                    const bool previewed = state.createPreview.is_object() && match(state.createPreview);
                    const bool pressed = Row("##armor", rowHeight, previewed, row, rowWidth, true, available++ % 2);
                    effects(armor);
                    auto* draw = ImGui::GetWindowDrawList();
                    draw->AddText(fonts.medium, Px(14), {row.x + 16 * scale, row.y + 10 * scale}, Flag(armor, "enchanted") ? Palette::Emerald : Palette::Text, Text(armor, "name").c_str());
                    draw->AddText(fonts.body, Px(12), {row.x + 16 * scale, row.y + 32 * scale}, Palette::CopperLight, meta(armor).c_str());
                    ImGui::SetCursorScreenPos({row.x + rowWidth - 84 * scale, row.y + 13 * scale});
                    const bool add = Press("Add", {68 * scale, 30 * scale}, Tone::Amber, true, Icon::Plus, 13);
                    ImGui::SetCursorScreenPos({row.x, row.y + rowHeight}); ImGui::Dummy({0, 0});
                    if (add) {
                        // A previewed piece is already on the NPC; Add only puts it in the outfit.
                        state.items.push_back(armor);
                        if (previewed) state.createPreview = nullptr;
                        else Emit("tailorEquipArmorItem", armor);
                    } else if (pressed) {
                        // A row click only tries the piece on, in place of the last one tried.
                        if (state.createPreview.is_object()) Emit("tailorUnequipArmorItem", state.createPreview);
                        if (previewed) state.createPreview = nullptr;
                        else { state.createPreview = armor; Emit("tailorEquipArmorItem", armor); }
                    }
                    ImGui::PopID();
                }
                if (!available) {
                    ImGui::SetCursorPos({18 * scale, 18 * scale});
                    Label(state.selectedPlugin.empty() ? "Choose an armor mod above to browse its pieces." : "Nothing left to add from this mod with these filters.", Palette::Muted, 13);
                }
                EndList();
                BeginActions();
                if (Action(state.editId ? "Save Changes" : "Save Outfit", content, Tone::Primary, !LibraryReadOnly() && !BlankName(state.name.data()) && !nameTaken && !state.categoryIds.empty() && !state.items.empty())) {
                    Model payload = {{"name", EffectiveName()}, {"categoryIds", state.categoryIds}, {"categoryId", *state.categoryIds.begin()}, {"sex", state.outfitSex}, {"items", state.items}};
                    if (state.editId) payload["outfitId"] = state.editId;
                    Emit(state.editId ? "tailorUpdateOutfit" : "tailorSaveOutfit", payload);
                    state.outfitPreview = false; state.createPreview = nullptr; state.page = Page::Library;
                    state.search.fill(0); state.category = -1;
                    if (state.libraryStashed && !state.libraryWigs) RestoreLibrary();
                    // The refreshed list may re-sort a renamed outfit; reveal it again then.
                    (void)Changed("tailorSetOutfits");
                    state.libraryRevealOnRefresh = true;
                    Emit("tailorRequestOutfits");
                }
                ImGui::EndChild();
                ImGui::SetCursorPos(ImVec2(primary, 0));
                ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(10, 8, 7, 70));
                Child("Outfit Companion", ImVec2(fullWidth - primary, fullHeight), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
                const auto side = ImGui::GetWindowPos();
                ImGui::GetWindowDrawList()->AddLine(side, {side.x, side.y + fullHeight}, Palette::Border);
                Header("OUTFIT ITEMS", false, "", Icon::None, false);
                const float sideContent = fullWidth - primary - 40 * scale;
                ImGui::SetNextItemWidth(sideContent);
                std::string selected = "Choose categories...";
                if (!state.categoryIds.empty()) {
                    selected.clear();
                    for (const auto& category : Rows(Get(model, "tailorSetCategories"))) if (state.categoryIds.contains(static_cast<std::uint32_t>(Number(category, "id"))))
                        selected += (selected.empty() ? "" : ", ") + Text(category, "displayName", Text(category, "name").c_str());
                    if (selected.empty()) selected = std::to_string(state.categoryIds.size()) + " selected";
                }
                if (BeginSelect("##memberships", selected, state.categoryIds.empty())) {
                    for (const auto& category : Rows(Get(model, "tailorSetCategories"))) {
                        const auto id = static_cast<std::uint32_t>(Number(category, "id"));
                        ImGui::PushID(static_cast<int>(id));
                        if (SelectItem(Text(category, "displayName", Text(category, "name").c_str()), state.categoryIds.contains(id), true)) {
                            if (!state.categoryIds.erase(id)) state.categoryIds.insert(id);
                        }
                        ImGui::PopID();
                    }
                    EndSelect();
                }
                // Who the outfit is for, tinted like the tags on Manage Outfits.
                ImGui::SetCursorPosX(20 * scale); ImGui::SetNextItemWidth(sideContent);
                const char* sexChoice = state.outfitSex == 1 ? "Female only" : state.outfitSex == 0 ? "Male only" : "Unisex \xC2\xB7 fits any NPC";
                if (BeginSelect("##outfitSex", sexChoice, false, SexInk(state.outfitSex))) {
                    for (const int sex : {-1, 1, 0}) if (SelectItem(SexLabel(sex), state.outfitSex == sex)) state.outfitSex = sex;
                    EndSelect();
                }
                ImGui::SetCursorPosX(20 * scale); ImGui::SetNextItemWidth(sideContent);
                if (BeginSelect("##slot", state.slotFilter.empty() ? "All Slots" : state.slotFilter)) {
                    if (SelectItem("All Slots", state.slotFilter.empty())) state.slotFilter.clear();
                    std::set<std::string> slots;
                    for (const auto& item : Rows(Get(armorData, "armors"))) slots.insert(Text(item, "slot"));
                    for (const auto& item : state.items) slots.insert(Text(item, "slot"));
                    for (const auto& slot : slots) if (!slot.empty() && SelectItem(slot, slot == state.slotFilter)) state.slotFilter = slot;
                    EndSelect();
                }
                // Three selects above, so this list starts level with the armor list beside it.
                ImGui::SetCursorPos({20 * scale, 236 * scale}); Eyebrow("IN THIS OUTFIT", std::to_string(state.items.size()) + (state.items.size() == 1 ? " piece" : " pieces"));
                ImGui::SetCursorPos({20 * scale, 256 * scale});
                BeginList("chosen", {sideContent, std::max(100 * scale, fullHeight - 102 * scale - 256 * scale)});
                if (state.items.empty()) { ImGui::SetCursorPos({18 * scale, 18 * scale}); ImGui::PushTextWrapPos(sideContent - 18 * scale); Label("Add armor pieces from the left to build this outfit.", Palette::Muted, 13); ImGui::PopTextWrapPos(); }
                for (std::size_t i = 0; i < state.items.size();) {
                    const auto item = state.items[i];
                    if (!state.slotFilter.empty() && Text(item, "slot") != state.slotFilter) { ++i; continue; }
                    ImGui::PushID(static_cast<int>(i));
                    const auto row = ImGui::GetCursorScreenPos();
                    const float rowWidth = ImGui::GetContentRegionAvail().x, rowHeight = 56 * scale;
                    ImGui::Dummy({rowWidth - 60 * scale, rowHeight});
                    effects(item);
                    auto* draw = ImGui::GetWindowDrawList();
                    draw->PushClipRect(row, {row.x + rowWidth - 56 * scale, row.y + rowHeight}, true);
                    draw->AddText(fonts.medium, Px(14), {row.x + 16 * scale, row.y + 10 * scale}, Flag(item, "enchanted") ? Palette::Emerald : Palette::Text, Text(item, "name").c_str());
                    draw->AddText(fonts.body, Px(12), {row.x + 16 * scale, row.y + 32 * scale}, Palette::CopperLight, meta(item).c_str());
                    draw->PopClipRect();
                    draw->AddLine({row.x, row.y + rowHeight - scale}, {row.x + rowWidth, row.y + rowHeight - scale}, Palette::Line1);
                    ImGui::SetCursorScreenPos({row.x + rowWidth - 46 * scale, row.y + 13 * scale});
                    const bool remove = Small("##remove", Tone::Danger, true, Icon::Close);
                    ImGui::SetCursorScreenPos({row.x, row.y + rowHeight}); ImGui::Dummy({0, 0});
                    ImGui::PopID();
                    if (remove) { Emit("tailorUnequipArmorItem", item); state.items.erase(state.items.begin() + static_cast<std::ptrdiff_t>(i)); }
                    else ++i;
                }
                EndList();
                BeginActions();
                if (Action("Cancel", sideContent)) Navigate(Page::Library, false);
                ImGui::EndChild();
                ImGui::PopStyleColor(2);
            }
			 void Discovered()
            {
                if (Header("DISCOVERED SETS", true, "", Icon::Grid)) Navigate(Page::Main, false);
                const float content = ImGui::GetWindowWidth() - 40 * scale, listBottom = ImGui::GetWindowHeight() - 102 * scale;
                ImGui::SetCursorPosX(20 * scale);
                if (Small("Rescan", Tone::Neutral)) Emit("tailorRescanDiscovered");
                // Sex filter and search share the row, like the Library page.
                const float fields = content - 32 * scale, sexWidth = fields * 0.24f;
                ImGui::SetCursorPosX(20 * scale); ImGui::SetNextItemWidth(sexWidth);
                if (BeginSelect("##discoveredSex", state.librarySex ? SexLabel(*state.librarySex) : "All Sexes", false, state.librarySex ? SexInk(*state.librarySex) : 0)) {
                    if (SelectItem("All Sexes", !state.librarySex)) state.librarySex.reset();
                    for (const int sex : {-1, 1, 0}) if (SelectItem(SexLabel(sex), state.librarySex == sex)) state.librarySex = sex;
                    EndSelect();
                }
                ImGui::SameLine(0, 16 * scale); ImGui::SetNextItemWidth(fields - sexWidth);
                TextInput("##discoveredSearch", "Search discovered sets...", state.search.data(), state.search.size());
                ImGui::SetCursorPosX(20 * scale);
                BeginList("discoveredPage", {content, std::max(100 * scale, listBottom - ImGui::GetCursorPosY())});
                auto discovered = Rows(Get(model, "tailorSetDiscoveredOutfits"));
                SortByName(discovered);
                int dshown = 0;
                const int dTargetSex = TargetSex(Get(model, "tailorSetTarget"));
                for (const auto& d : discovered) {
                    if (!Matches(Text(d, "name"), state.search.data())) continue;
                    const int dsex = Number(d, "sex", -1);
                    if (state.librarySex && dsex != *state.librarySex) continue;
                    const int did = Number(d, "id");
                    ImGui::PushID(did);
                    ImVec2 drow; float drowWidth; const float drowHeight = 56 * scale;
                    if (Row("##discovered", drowHeight, state.previewId == static_cast<std::uint32_t>(did), drow, drowWidth, true, dshown++ % 2)) {
                        if (!OutfitFits(static_cast<OutfitSex>(dsex), dTargetSex)) {
                            state.message = "Outfit can't be previewed by the currently selected NPC due to gender";
                            state.messageDanger = false;
                        } else {
                            // BUG 1 FIX: regular outfit rows start the create/edit
                            // preview session (tailorBeginCreateOutfit) before
                            // previewing; without it LoadCreateOutfitItems
                            // silently does nothing on a fresh session.
                            if (!state.outfitPreview) Emit("tailorBeginCreateOutfit");
                            state.outfitPreview = true; state.previewId = did;
                            Emit("tailorPreviewDiscovered", {{"outfitId", did}});
                        }
                    }
                    auto* ddraw = ImGui::GetWindowDrawList();
                    ddraw->AddText(fonts.medium, Px(14), {drow.x + 16 * scale, drow.y + 8 * scale}, Palette::Text, Text(d, "name").c_str());
                    ImVec2 dchip{drow.x + 16 * scale, drow.y + drowHeight - 24 * scale};
                    SexTag(ddraw, dchip, dsex);
                    const auto dcount = std::to_string(Number(d, "itemCount")) + " pcs";
                    const float dcountW = fonts.body->CalcTextSizeA(Px(12), FLT_MAX, 0, dcount.c_str()).x;
                    ddraw->AddText(fonts.body, Px(12), {drow.x + drowWidth - dcountW - 116 * scale, drow.y + (drowHeight - Px(12)) / 2}, Palette::Muted, dcount.c_str());
                    ImGui::SetCursorScreenPos({drow.x + drowWidth - 104 * scale, drow.y + (drowHeight - 30 * scale) / 2});
                    if (Small("Save", Tone::Amber)) Emit("tailorSaveDiscovered", {{"outfitId", did}});
                    ImGui::SetCursorScreenPos({drow.x, drow.y + drowHeight}); ImGui::Dummy({0, 0});
                    ImGui::PopID();
                }
                if (!dshown) { ImGui::SetCursorPos({18 * scale, 18 * scale}); Label(discovered.empty() ? "No discovered sets yet. They are built automatically when a save loads." : "No discovered set matches that search.", Palette::Muted, 13); }
                EndList();
            }
            void Help(const char* text)
            {
                ImGui::PushID(text);
                Press("?", {30 * scale, 30 * scale}, Tone::Amber, true, Icon::None, 13, true);
                if (ImGui::IsItemHovered() || ImGui::IsItemFocused()) {
                    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {16 * scale, 13 * scale});
                    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 8 * scale);
                    ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(245, 158, 11, 107));
                    ImGui::BeginTooltip(); ImGui::PushTextWrapPos(340 * scale); Label(text, Palette::Text, 13); ImGui::PopTextWrapPos(); ImGui::EndTooltip();
                    ImGui::PopStyleColor(); ImGui::PopStyleVar(2);
                }
                ImGui::PopID();
            }
            const char* PageHelp()
            {
                switch (state.page) {
                case Page::Categories: return "Create as many custom categories as you like. Select a category to rename or delete it.";
                case Page::Blacklist: return state.wigs ? "Blacklisted plugins are hidden from the Add Wigs screen. Blacklist hides a plugin's wigs; Unblacklist shows them again." : "Blacklisted plugins are hidden from the Create Outfit armor browser. Blacklist hides a plugin's armor; Unblacklist shows it again.";
                case Page::Situations: return state.wigs ? "Assign wigs to situations. The NPC automatically changes wig when the situation changes. Dress picks a wig category." : "Assign outfits to adventuring, town, home, sleep, swimming, or warm. Home is the player's house; NPCs are Home in any house. Swimming restores the previous outfit when the NPC leaves the water. Warm is worn outdoors in cloudy, rainy or snowy weather, and in snowy regions. Dress picks a category to cycle from. Only outfits that fit the NPC's sex are offered or picked at random.";
                case Page::Export: return "Select outfits to share. Each outfit's sex travels with it. Only built-in categories travel with an outfit; custom categories stay in your library. Recipients need the armor mods used by these outfits.";
                case Page::Import: return "Choose a file to import its outfits. A duplicate has the same name and armor items. Missing armor or unsupported categories cause that outfit to be skipped. Files from older versions carry no sex; their outfits import as Unisex. Existing outfits and NPC assignments are preserved.";
                case Page::AddWigs: return "Click a row to preview a wig on the target. Select a category with the pills, then click Add. Use Set All to batch-assign categories.";
                case Page::HairColor: return "Pick a preset swatch to apply its color immediately. Saved custom colors appear alongside the presets. Reset to Default restores the original hair color.";
                case Page::CustomColors: return "Pick a color from the wheel or type RGB values, then Add to save it. Saved colors appear on the main hair color screen alongside the defaults.";
                case Page::Library: return state.wigs ? "Click a wig to preview it on the target NPC. Use Move to to recategorize, or remove it from the library." : "Click a row to preview an outfit on the target NPC, or Edit to modify it. Female and Male outfits preview only on NPCs of that sex. Set outfits to... in the header tags every outfit the filters show. Leaving restores the NPC's outfit. Categories organize outfits; Blacklist hides plugins.";
				case Page::Discovered: return "Auto-detected outfit sets from your installed armor mods. Click a row to preview it on the target NPC. Save copies a set into your outfits as a regular editable outfit. Rescan rebuilds the list from the current load order.";
                case Page::Create: return "Name your outfit and select one or more categories. Choose who it is for: Unisex fits every NPC; Female and Male fit only NPCs of that sex. Search mods to find armor. Click an item to preview it on the NPC; Add puts it in the outfit. Remove pieces with the X button. The saved outfit is shared across its categories.";
                case Page::Cycle: return "Use arrow keys or A/D to cycle. Enter confirms. Escape cancels and reverts. Reset removes the assignment entirely.";
                default: return "";
                }
            }
            void SecondaryHeader(const char* title, Page back)
            {
                if (Header(title, true)) Navigate(back, state.wigs);
            }
            void Categories()
            {
                SecondaryHeader("OUTFIT CATEGORIES", Page::Library);
                const auto& data = Get(model, "tailorSetAllCategories");
                const auto& editable = Rows(Get(data, "categories"));
                if (state.category >= 0 && std::none_of(editable.begin(), editable.end(), [&](const auto& c) { return Number(c, "id") == state.category; })) { state.category = -1; ForgetName(); }
                const float content = ImGui::GetWindowWidth() - 40 * scale;
                ImGui::SetNextItemWidth(content);
                TextInput("##newCategory", "New category name...", state.newCategoryName.data(), state.newCategoryName.size());
                // A taken name keeps Create off and stays typed, with the reason in the gap under the field.
                const bool newTaken = CategoryNameTaken(model, state.newCategoryName.data());
                if (newTaken) {
                    const auto box = ImGui::GetItemRectMin(), end = ImGui::GetItemRectMax();
                    const auto message = CategoryTakenMessage(state.newCategoryName.data());
                    const ImVec4 clip{box.x, end.y, box.x + content - 116 * scale, end.y + 18 * scale};
                    ImGui::GetWindowDrawList()->AddText(fonts.body, Px(12), {box.x, end.y + 3 * scale}, Palette::DangerText, message.c_str(), nullptr, 0, &clip);
                }
                ImGui::SetCursorPos({20 * scale, 146 * scale - Px(11) / 2});
                Eyebrow(std::to_string(editable.size()) + (editable.size() == 1 ? " CUSTOM CATEGORY" : " CUSTOM CATEGORIES"), "", false, Palette::Muted);
                ImGui::SetCursorPos({ImGui::GetWindowWidth() - 116 * scale, 130 * scale});
                if (Press("Create", {96 * scale, 34 * scale}, Tone::Primary, !LibraryReadOnly() && !BlankName(state.newCategoryName.data()) && !newTaken)) { Emit("tailorAddCategory", {{"name", state.newCategoryName.data()}}); state.newCategoryName.fill(0); }
                // The list takes every pixel between the form above and the editor pinned below.
                const float editorTop = ImGui::GetWindowHeight() - 86 * scale - 84 * scale;
                ImGui::SetCursorPos({20 * scale, 180 * scale});
                BeginList("categoryRows", {content, std::max(100 * scale, editorTop - 16 * scale - 180 * scale)});
                auto band = [&](const char* title, const char* meta) {
                    const auto at = ImGui::GetCursorScreenPos();
                    ImGui::GetWindowDrawList()->AddRectFilled(at, {at.x + ImGui::GetContentRegionAvail().x, at.y + 32 * scale}, IM_COL32(10, 8, 7, 150));
                    ImGui::SetCursorScreenPos({at.x + 16 * scale, at.y + (32 * scale - Px(11)) / 2}); Eyebrow(title, meta);
                    ImGui::SetCursorScreenPos({at.x, at.y + 32 * scale}); ImGui::Dummy({0, 0});
                };
                auto count = [&](ImVec2 row, float rowWidth, float rowHeight, int outfits, ImU32 color) {
                    const auto text = std::to_string(outfits) + (outfits == 1 ? " outfit" : " outfits");
                    ImGui::GetWindowDrawList()->AddText(fonts.body, Px(12), {row.x + rowWidth - 16 * scale - fonts.body->CalcTextSizeA(Px(12), FLT_MAX, 0, text.c_str()).x, row.y + (rowHeight - Px(12)) / 2}, color, text.c_str());
                };
                band("SITUATION POOLS", "read-only");
                for (const auto& category : Rows(Get(data, "situationPools"))) {
                    const auto row = ImGui::GetCursorScreenPos();
                    const float rowWidth = ImGui::GetContentRegionAvail().x, rowHeight = 36 * scale;
                    ImGui::Dummy({rowWidth, rowHeight});
                    ImGui::GetWindowDrawList()->AddText(fonts.body, Px(14), {row.x + 16 * scale, row.y + (rowHeight - Px(14)) / 2}, Palette::Muted, Text(category, "name").c_str());
                    count(row, rowWidth, rowHeight, Number(category, "outfitCount"), Palette::Ghost);
                }
                band("CUSTOM CATEGORIES", "");
                auto customCategories = Rows(Get(data, "categories"));
                SortByName(customCategories);
                for (const auto& category : customCategories) {
                    const int id = Number(category, "id");
                    ImGui::PushID(id);
                    ImVec2 row; float rowWidth; const float rowHeight = 44 * scale;
                    if (Row("##customCategory", rowHeight, state.category == id, row, rowWidth)) { state.category = id; SetName(Text(category, "name")); }
                    ImGui::GetWindowDrawList()->AddText(fonts.medium, Px(15), {row.x + 16 * scale, row.y + (rowHeight - Px(15)) / 2}, state.category == id ? Palette::AmberLight : Palette::Text, Text(category, "name").c_str());
                    count(row, rowWidth, rowHeight, Number(category, "outfitCount"), Palette::Muted);
                    ImGui::PopID();
                }
                if (customCategories.empty()) { ImGui::SetCursorPosX(16 * scale); ImGui::Dummy({0, 10 * scale}); ImGui::SetCursorPosX(16 * scale); Label("No custom categories yet. Name one above, then Create.", Palette::Muted, 13); }
                EndList();
                ImGui::SetCursorPos({20 * scale, editorTop});
                Eyebrow("EDIT CATEGORY", state.category < 0 ? "select a category above" : "");
                ImGui::BeginDisabled(state.category < 0);
                ImGui::SetCursorPos({20 * scale, editorTop + 26 * scale}); ImGui::SetNextItemWidth(content);
                TextInput("##categoryName", "Category name", state.name.data(), state.name.size());
                const bool renameTaken = state.category >= 0 && CategoryNameTaken(model, EffectiveName(), state.category);
                if (renameTaken) {
                    const auto box = ImGui::GetItemRectMin(), end = ImGui::GetItemRectMax();
                    const auto message = CategoryTakenMessage(EffectiveName());
                    const ImVec4 clip{box.x, end.y, box.x + content, end.y + 18 * scale};
                    ImGui::GetWindowDrawList()->AddText(fonts.body, Px(12), {box.x, end.y + 3 * scale}, Palette::DangerText, message.c_str(), nullptr, 0, &clip);
                }
                const float bar = BeginActions(), half = (bar - 10 * scale) / 2;
                if (Action("Save Name", half, Tone::Primary, !LibraryReadOnly() && !BlankName(state.name.data()) && !renameTaken)) Emit("tailorRenameCategory", {{"categoryId", state.category}, {"name", EffectiveName()}});
                ImGui::SameLine(); if (Action("Delete Category", half, Tone::Danger, !LibraryReadOnly())) Confirm("Delete Category", "Delete this category? Outfits remain in the library.", "tailorDeleteCategory", {{"categoryId", state.category}});
                ImGui::EndDisabled();
            }
            void Blacklist()
            {
                SecondaryHeader("BLACKLIST PLUGINS", Page::Library);
                const float content = ImGui::GetWindowWidth() - 40 * scale;
                ImGui::SetNextItemWidth(content);
                TextInput("##search", "Search plugins...", state.search.data(), state.search.size());
                ImGui::SetCursorPosX(20 * scale);
                BeginList("plugins", {content, std::max(100 * scale, ImGui::GetWindowHeight() - 102 * scale - ImGui::GetCursorPosY())});
                int shown = 0;
                for (const auto& plugin : Rows(Get(model, state.wigs ? "wiggySetBlacklistData" : "tailorSetBlacklistData"))) {
                    const auto name = Text(plugin, "name");
                    if (!Matches(name, state.search.data())) continue;
                    ImGui::PushID(name.c_str());
                    const bool blocked = Flag(plugin, "blacklisted");
                    const auto row = ImGui::GetCursorScreenPos();
                    const float rowWidth = ImGui::GetContentRegionAvail().x, rowHeight = 48 * scale;
                    auto* draw = ImGui::GetWindowDrawList();
                    if (shown++ % 2) draw->AddRectFilled(row, {row.x + rowWidth, row.y + rowHeight}, IM_COL32(244, 235, 216, 5));
                    draw->AddLine({row.x, row.y + rowHeight - scale}, {row.x + rowWidth, row.y + rowHeight - scale}, Palette::Line1);
                    draw->AddText(fonts.medium, Px(14), {row.x + 16 * scale, row.y + (rowHeight - Px(14)) / 2}, blocked ? Palette::Ghost : Palette::Text, name.c_str());
                    const float nameEnd = row.x + 28 * scale + fonts.medium->CalcTextSizeA(Px(14), FLT_MAX, 0, name.c_str()).x;
                    const int pieces = Number(plugin, state.wigs ? "wigCount" : "armorCount");
                    const auto meta = blocked ? std::string("hidden") : std::to_string(pieces) + (state.wigs ? " wigs" : " armors");
                    draw->AddText(fonts.body, Px(12), {nameEnd, row.y + (rowHeight - Px(12)) / 2}, blocked ? Palette::DangerText : Palette::Ghost, meta.c_str());
                    ImGui::SetCursorScreenPos({row.x + rowWidth - 126 * scale, row.y + 9 * scale});
                    if (Press(blocked ? "Unblacklist" : "Blacklist", {110 * scale, 30 * scale}, blocked ? Tone::Neutral : Tone::Danger, true, Icon::None, 13)) Emit(state.wigs ? (blocked ? "wiggyUnblacklistPlugin" : "wiggyBlacklistPlugin") : (blocked ? "tailorUnblacklistPlugin" : "tailorBlacklistPlugin"), {{"plugin", name}});
                    ImGui::SetCursorScreenPos({row.x, row.y + rowHeight}); ImGui::Dummy({0, 0});
                    ImGui::PopID();
                }
                if (!shown) { ImGui::SetCursorPos({18 * scale, 18 * scale}); Label("No plugin matches that search.", Palette::Muted, 13); }
                EndList();
                const float bar = BeginActions();
                if (Action("Clear Blacklist", bar, Tone::Danger)) Confirm("Clear Blacklist", "Show all plugins in the item browser?", state.wigs ? "wiggyClearBlacklist" : "tailorClearBlacklist");
            }
            // Small stroke glyph for a situation card's tile.
            void SituationGlyph(ImDrawList* draw, int situation, ImVec2 c, ImU32 color) const
            {
                const float stroke = 1.5f * scale;
                auto p = [&](float x, float y) { return ImVec2(c.x + x * scale, c.y + y * scale); };
                auto line = [&](float x, float y, float xx, float yy) { draw->AddLine(p(x, y), p(xx, yy), color, stroke); };
                switch (situation) {
                case 0: line(-7, 7, 7, -7); line(7, 7, -7, -7); line(-7, 2, -2, 7); line(7, 2, 2, 7); break; // crossed blades
                case 1: draw->AddRect(p(-8, -2), p(-2, 8), color, 0, 0, stroke); draw->AddRect(p(-2, -8), p(4, 8), color, 0, 0, stroke); draw->AddRect(p(4, 1), p(9, 8), color, 0, 0, stroke); break; // rooftops
                case 2: line(-8, 0, 0, -8); line(0, -8, 8, 0); draw->AddRect(p(-6, 0), p(6, 8), color, 0, 0, stroke); break; // hearth
                case 3: draw->PathArcTo(c, 7 * scale, 0.9f, 5.4f, 20); draw->PathStroke(color, 0, stroke); draw->PathArcTo(p(3.5f, -2.5f), 5.5f * scale, 1.5f, 4.2f, 14); draw->PathStroke(color, 0, stroke); break; // crescent
                case 4: for (float y : {-4.0f, 3.0f}) { const ImVec2 wave[] = {p(-8, y + 2), p(-4, y - 2), p(0, y + 2), p(4, y - 2), p(8, y + 2)}; draw->AddPolyline(wave, 5, color, 0, stroke); } break; // water
                default: line(0, -8, 0, 8); line(-6.9f, -4, 6.9f, 4); line(-6.9f, 4, 6.9f, -4); line(-2.5f, -8, 0, -5.5f); line(2.5f, -8, 0, -5.5f); line(-2.5f, 8, 0, 5.5f); line(2.5f, 8, 0, 5.5f); break; // snowflake
                }
            }
            void Situations()
            {
                SecondaryHeader(state.wigs ? "SITUATIONAL WIGS" : "SITUATIONAL OUTFITS", Page::Main);
                // Outfits / Wigs segmented switch beside the help mark.
                const auto origin = ImGui::GetWindowPos();
                const float segmentLeft = ImGui::GetWindowWidth() - 206 * scale;
                ImGui::GetWindowDrawList()->AddRectFilled({origin.x + segmentLeft - 3 * scale, origin.y + 15 * scale}, {origin.x + segmentLeft + 143 * scale, origin.y + 49 * scale}, IM_COL32(244, 235, 216, 8), 9 * scale);
                ImGui::GetWindowDrawList()->AddRect({origin.x + segmentLeft - 3 * scale, origin.y + 15 * scale}, {origin.x + segmentLeft + 143 * scale, origin.y + 49 * scale}, Palette::Border, 9 * scale);
                ImGui::SetCursorPos({segmentLeft, 18 * scale});
                if (Press("Outfits", {68 * scale, 28 * scale}, state.wigs ? Tone::Neutral : Tone::Amber, true, Icon::None, 12)) Navigate(Page::Situations, false);
                ImGui::SameLine(0, 4 * scale);
                if (Press("Wigs", {68 * scale, 28 * scale}, state.wigs ? Tone::Amber : Tone::Neutral, true, Icon::None, 12)) Navigate(Page::Situations, true);
                const auto& data = Get(model, state.wigs ? "wiggySetWigSituationData" : "tailorSetSituationData");
                static constexpr const char* keys[] = {"adventuring", "town", "home", "sleep", "swimming", "warm"};
                static constexpr const char* names[] = {"Adventuring", "Town", "Home", "Sleep", "Swimming", "Warm"};
                const float content = ImGui::GetWindowWidth() - 40 * scale;
                ImGui::SetCursorPos({20 * scale, 80 * scale});
                BeginList("situationScroll", {content, std::max(100 * scale, ImGui::GetWindowHeight() - 102 * scale - 80 * scale)}, false);
                for (int i = 0; i < (state.wigs ? 4 : 6); ++i) {
                    const std::string key = keys[i];
                    ImGui::PushID(i);
                    const bool armor = i == 0 && !state.wigs;
                    const auto name = Text(data, (key + "Name").c_str());
                    bool random = Flag(data, (key + "Random").c_str());
                    const int pool = Number(data, (key + "CatCount").c_str());
                    const auto armorType = Text(data, "adventuringArmorType", "any");
                    // A saved outfit tagged for the other sex is skipped in game; the card says so.
                    const bool unfit = !state.wigs && !random && !name.empty() && data.contains(key + "Fits") && !Flag(data, (key + "Fits").c_str());
                    const bool mismatch = armor && !unfit && !name.empty() && armorType != "any" && data.contains("adventuringAssignedCompatible") && !Flag(data, "adventuringAssignedCompatible");
                    const float cardHeight = (armor ? (mismatch || unfit ? 158 : 132) : (unfit ? 102 : 76)) * scale, cardWidth = ImGui::GetContentRegionAvail().x - (ImGui::GetScrollMaxY() > 0 ? 6 * scale : 0);
                    const auto card = ImGui::GetCursorScreenPos();
                    auto* draw = ImGui::GetWindowDrawList();
                    draw->AddRectFilled(card, {card.x + cardWidth, card.y + cardHeight}, IM_COL32(10, 8, 7, 105), 10 * scale);
                    draw->AddRect(card, {card.x + cardWidth, card.y + cardHeight}, Palette::Border, 10 * scale);
                    draw->AddRectFilled({card.x + 16 * scale, card.y + 18 * scale}, {card.x + 56 * scale, card.y + 58 * scale}, IM_COL32(244, 235, 216, 10), 8 * scale);
                    draw->AddRect({card.x + 16 * scale, card.y + 18 * scale}, {card.x + 56 * scale, card.y + 58 * scale}, Palette::Line3, 8 * scale);
                    SituationGlyph(draw, i, {card.x + 36 * scale, card.y + 38 * scale}, Palette::CopperLight);
                    draw->AddText(fonts.bold, Px(16), {card.x + 68 * scale, card.y + 16 * scale}, Palette::Text, names[i]);
                    const auto assigned = random ? "Random from pool (" + std::to_string(pool) + (pool == 1 ? " outfit)" : " outfits)") : (name.empty() ? std::string("\xE2\x80\x94 Not assigned \xE2\x80\x94") : name);
                    draw->AddText(fonts.body, Px(13), {card.x + 68 * scale, card.y + 41 * scale}, random || name.empty() ? Palette::Muted : Palette::Amber, assigned.c_str());
                    if (const int rating = Number(data, (key + "AR").c_str()); rating > 0 && !random && !name.empty())
                        Rating(draw, {card.x + 90 * scale + fonts.body->CalcTextSizeA(Px(13), FLT_MAX, 0, assigned.c_str()).x, card.y + 41 * scale + Px(13) / 2}, rating);
                    // Controls ride the card's right edge: Random, Dress, clear.
                    float right = card.x + cardWidth - 16 * scale;
                    ImGui::SetCursorScreenPos({right - 34 * scale, card.y + 21 * scale});
                    if (Press("##clear", {34 * scale, 34 * scale}, Tone::Danger, true, Icon::Close, 12)) Emit(state.wigs ? "wiggyClearWigSituation" : "tailorClearSituation", {{"situation", i + 1}});
                    right -= 42 * scale;
                    const bool compatible = !armor || !data.contains("adventuringHasDressOptions") || Flag(data, "adventuringHasDressOptions");
                    ImGui::SetCursorScreenPos({right - 68 * scale, card.y + 21 * scale});
                    if (Press("Dress", {68 * scale, 34 * scale}, Tone::Primary, !random && compatible, Icon::None, 13)) state.pickerSituation = i + 1;
                    right -= 76 * scale;
                    if (!state.wigs) {
                        const bool usable = random || pool > 0;
                        const ImVec2 pill{right - 102 * scale, card.y + 21 * scale};
                        ImGui::SetCursorScreenPos(pill);
                        ImGui::BeginDisabled(!usable);
                        if (Press("##random", {102 * scale, 34 * scale}, random ? Tone::Amber : Tone::Neutral, true, Icon::None, 12, true)) { random = !random; Emit("tailorToggleSituationRandom", {{"situation", i + 1}, {"random", random}}); }
                        // A greyed-out pill says how to fill its pool: the count above only takes
                        // what the target can wear.
                        if (!usable && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {16 * scale, 13 * scale});
                            ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 8 * scale);
                            ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(245, 158, 11, 107));
                            ImGui::BeginTooltip(); ImGui::PushTextWrapPos(340 * scale);
                            Label("Add outfits to the " + std::string(names[i]) + " category in Create or Edit to pick one at random. Only Unisex outfits and outfits for this character's sex count.", Palette::Text, 13);
                            ImGui::PopTextWrapPos(); ImGui::EndTooltip();
                            ImGui::PopStyleColor(); ImGui::PopStyleVar(2);
                        }
                        const int fade = usable ? 255 : 115;
                        Check(draw, {pill.x + 13 * scale, pill.y + 8 * scale}, random);
                        Tracked(draw, {pill.x + 39 * scale, pill.y + (34 * scale - Px(10)) / 2}, "RANDOM", fonts.bold, 10, random ? IM_COL32(251, 191, 36, fade) : IM_COL32(184, 162, 133, fade), 1.4f);
                        ImGui::EndDisabled();
                    }
                    if (armor) {
                        draw->AddLine({card.x + 16 * scale, card.y + 76 * scale}, {card.x + cardWidth - 16 * scale, card.y + 76 * scale}, Palette::Line1);
                        Tracked(draw, {card.x + 16 * scale, card.y + 104 * scale - Px(10) / 2}, "ARMOR TYPE", fonts.bold, 10, Palette::Muted, 1.8f);
                        static constexpr const char* types[] = {"any", "heavy", "light", "clothing"};
                        static constexpr const char* typeNames[] = {"Any Armor", "Heavy Armor", "Light Armor", "Clothing"};
                        int current = 0;
                        for (int type = 1; type < 4; ++type) if (armorType == types[type]) current = type;
                        ImGui::SetCursorScreenPos({card.x + 110 * scale, card.y + 86 * scale});
                        ImGui::SetNextItemWidth(168 * scale);
                        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {14 * scale, (36 * scale - Px(14)) / 2});
                        if (BeginSelect("##armor", typeNames[current])) {
                            for (int type = 0; type < 4; ++type) if (SelectItem(typeNames[type], type == current)) Emit("tailorSetAdventuringArmorType", {{"armorType", types[type]}});
                            EndSelect();
                        }
                        ImGui::PopStyleVar();
                        const int matching = Number(data, "adventuringCatCount");
                        const auto matches = Flag(data, "adventuringTypeFallback")
                            ? std::string("No ") + typeNames[current] + " outfits in Adventuring, so all " + std::to_string(matching) + " are used"
                            : std::to_string(matching) + (matching == 1 ? " matching outfit in Adventuring" : " matching outfits in Adventuring");
                        draw->AddText(fonts.body, Px(13), {card.x + cardWidth - 16 * scale - fonts.body->CalcTextSizeA(Px(13), FLT_MAX, 0, matches.c_str()).x, card.y + 104 * scale - Px(13) / 2}, Palette::Muted, matches.c_str());
                        if (mismatch) draw->AddText(fonts.body, Px(12), {card.x + 16 * scale, card.y + 134 * scale}, Palette::CopperLight, "The saved outfit is not in Adventuring with this armor type; the regular outfit is the fallback.");
                    }
                    if (unfit) {
                        const auto note = std::string("The saved outfit is tagged ") + (TargetSex(Get(model, "tailorSetTarget")) == 1 ? "Male" : "Female") + " and doesn't fit this NPC; the fallback outfit is used.";
                        if (!armor) draw->AddLine({card.x + 16 * scale, card.y + 76 * scale}, {card.x + cardWidth - 16 * scale, card.y + 76 * scale}, Palette::Line1);
                        draw->AddText(fonts.body, Px(12), {card.x + 16 * scale, armor ? card.y + 134 * scale : card.y + 76 * scale + (26 * scale - Px(12)) / 2}, Palette::CopperLight, note.c_str());
                    }
                    ImGui::SetCursorScreenPos({card.x, card.y + cardHeight + 10 * scale}); ImGui::Dummy({0, 0});
                    if (state.pickerSituation == i + 1) {
                        // Dress picker: category chips flow under the card they belong to.
                        ImGui::SetCursorPosX(16 * scale); Eyebrow("DRESS FROM");
                        ImGui::Dummy({0, 8 * scale});
                        float x = 16 * scale;
                        const float startY = ImGui::GetCursorPosY(); float y = startY;
                        auto flow = [&](const std::string& label, bool enabled, Tone tone) {
                            const float width = fonts.medium->CalcTextSizeA(Px(13), FLT_MAX, 0, label.c_str()).x + 28 * scale;
                            if (x + width > cardWidth - 16 * scale && x > 16 * scale) { x = 16 * scale; y += 38 * scale; }
                            ImGui::SetCursorPos({x, y}); x += width + 8 * scale;
                            return Small(label.c_str(), tone, enabled);
                        };
                        for (const auto& category : Rows(Get(model, state.wigs ? "wiggySetCategories" : "tailorSetCategories"))) {
                            int count = state.wigs ? Number(category, "count") : Number(category, "fitCount", Number(category, "outfitCount"));
                            if (!state.wigs && i == 0 && Get(data, "adventuringCategoryCounts").is_object()) count = Number(Get(data, "adventuringCategoryCounts"), std::to_string(Number(category, "id")).c_str());
                            const auto catName = Text(category, "displayName", Text(category, "name").c_str());
                            ImGui::PushID(Number(category, state.wigs ? "index" : "id"));
                            if (flow(catName + " (" + std::to_string(count) + ")", count > 0, Tone::Amber)) StartCycle(category, i + 1);
                            ImGui::PopID();
                        }
                        if (flow("Cancel selection", true, Tone::Neutral)) state.pickerSituation = 0;
                        ImGui::SetCursorPos({0, y + 48 * scale}); ImGui::Dummy({0, 0});
                    }
                    ImGui::PopID();
                }
                EndList();
                BeginActions();
                const char* clearLabel = state.wigs ? "Clear All Wig Situations" : "Clear All Situations";
                const float clearWidth = fonts.bold->CalcTextSizeA(Px(15), FLT_MAX, 0, clearLabel).x + 40 * scale;
                ImGui::SetCursorPosX(ImGui::GetWindowWidth() - 20 * scale - clearWidth);
                if (Action(clearLabel, clearWidth, Tone::Danger)) Confirm("Clear All Situations", "Remove all situational assignments for this NPC?", state.wigs ? "wiggyClearAllWigSituations" : "tailorClearAllSituations");
            }
            void Transfer()
            {
                const bool exporting = state.page == Page::Export;
                SecondaryHeader(exporting ? "EXPORT OUTFITS" : "IMPORT OUTFITS", Page::Main);
                const auto& data = Get(model, "tailorSetTransferData");
                const float content = ImGui::GetWindowWidth() - 40 * scale, listBottom = ImGui::GetWindowHeight() - 102 * scale;
                ImGui::PushTextWrapPos(ImGui::GetWindowWidth() - 20 * scale);
                Label(exporting ? "Choose outfits to share. Your selections stay checked while you filter the list." : "Choose a JSON file to add its outfits to your library.", Palette::Muted, 13);
                const auto error = Text(data, "error");
                if (!error.empty()) { ImGui::SetCursorPosX(20 * scale); Label(error, Palette::DangerText, 13); }
                ImGui::PopTextWrapPos();
                auto transferOutfits = Rows(Get(data, "outfits"));
                SortByName(transferOutfits);
                const auto visible = [&](const Model& outfit) {
                    const auto& categories = Rows(Get(outfit, "categories"));
                    return Matches(Text(outfit, "name"), state.search.data()) &&
                        (!state.exportSex || SexOf(outfit) == *state.exportSex) &&
                        (state.exportArmorFilter.empty() || std::find(categories.begin(), categories.end(), Model(state.exportArmorFilter)) != categories.end()) &&
                        (state.exportCategoryFilter.empty() || std::find(categories.begin(), categories.end(), Model(state.exportCategoryFilter)) != categories.end());
                };
                ImGui::BeginDisabled(state.transferPending || state.transferLoading);
                if (exporting) {
                    static constexpr const char* armorKeys[] = {"", "heavy", "light", "clothing"};
                    static constexpr const char* armorLabels[] = {"All armor types", "Heavy Armor", "Light Armor", "Clothing"};
                    static constexpr const char* categoryKeys[] = {"", "adventuring", "town", "home", "sleep", "swimming", "warm"};
                    static constexpr const char* categoryLabels[] = {"All built-in categories", "Adventuring", "Town", "Home", "Sleep", "Swimming", "Warm"};
                    int armorIndex = 0, categoryIndex = 0;
                    for (int i = 1; i < 4; ++i) if (state.exportArmorFilter == armorKeys[i]) armorIndex = i;
                    for (int i = 1; i < 7; ++i) if (state.exportCategoryFilter == categoryKeys[i]) categoryIndex = i;
                    // Three filters share the row; Category is widest, for "All built-in categories".
                    const float fields = content - 24 * scale, armorWidth = fields * 0.3f, categoryWidth = fields * 0.4f;
                    const float categoryX = 32 * scale + armorWidth, sexX = categoryX + categoryWidth + 12 * scale, top = ImGui::GetCursorPosY() + 2 * scale;
                    ImGui::SetCursorPos({20 * scale, top}); Label("Armor type", Palette::Muted, 12);
                    ImGui::SetCursorPos({categoryX, top}); Label("Category", Palette::Muted, 12);
                    ImGui::SetCursorPos({sexX, top}); Label("Sex", Palette::Muted, 12);
                    ImGui::SetCursorPos({20 * scale, top + 22 * scale}); ImGui::SetNextItemWidth(armorWidth);
                    if (BeginSelect("##exportArmor", armorLabels[armorIndex])) {
                        for (int i = 0; i < 4; ++i) if (SelectItem(armorLabels[i], i == armorIndex)) state.exportArmorFilter = armorKeys[i];
                        EndSelect();
                    }
                    ImGui::SetCursorPos({categoryX, top + 22 * scale}); ImGui::SetNextItemWidth(categoryWidth);
                    if (BeginSelect("##exportCategory", categoryLabels[categoryIndex])) {
                        for (int i = 0; i < 7; ++i) if (SelectItem(categoryLabels[i], i == categoryIndex)) state.exportCategoryFilter = categoryKeys[i];
                        EndSelect();
                    }
                    ImGui::SetCursorPos({sexX, top + 22 * scale}); ImGui::SetNextItemWidth(fields - armorWidth - categoryWidth);
                    if (BeginSelect("##exportSex", state.exportSex ? SexLabel(*state.exportSex) : "All sexes", false, state.exportSex ? SexInk(*state.exportSex) : 0)) {
                        if (SelectItem("All sexes", !state.exportSex)) state.exportSex.reset();
                        for (const int sex : {-1, 1, 0}) if (SelectItem(SexLabel(sex), state.exportSex == sex)) state.exportSex = sex;
                        EndSelect();
                    }
                    ImGui::SetCursorPosX(20 * scale); ImGui::SetNextItemWidth(content);
                    TextInput("##search", "Search outfits by name...", state.search.data(), state.search.size());
                    ImGui::SetCursorPosX(20 * scale);
                    const float toolbar = ImGui::GetCursorPosY() + 2 * scale;
                    ImGui::SetCursorPosY(toolbar);
                    // "All" means everything the current filters list, matching the tally beside it.
                    if (Small("Select all")) for (const auto& outfit : transferOutfits) if (visible(outfit)) state.exportIds.insert(Number(outfit, "id"));
                    ImGui::SameLine(0, 8 * scale); if (Small("Clear selection", Tone::Neutral, !state.exportIds.empty())) state.exportIds.clear();
                    const auto shown = std::count_if(transferOutfits.begin(), transferOutfits.end(), visible);
                    const auto tally = std::to_string(state.exportIds.size()) + " selected  \xC2\xB7  " + std::to_string(shown) + " shown";
                    const auto origin = ImGui::GetWindowPos();
                    ImGui::GetWindowDrawList()->AddText(fonts.medium, Px(12), {origin.x + ImGui::GetWindowWidth() - 20 * scale - fonts.medium->CalcTextSizeA(Px(12), FLT_MAX, 0, tally.c_str()).x, origin.y + toolbar + (30 * scale - Px(12)) / 2}, Palette::Amber, tally.c_str());
                    ImGui::SetCursorPos({20 * scale, toolbar + 42 * scale});
                } else {
                    ImGui::SetCursorPos({20 * scale, 112 * scale});
                    if (Small("Refresh files", Tone::Neutral, true, Icon::RotateRight)) { state.transferLoading = true; Emit("tailorRequestTransferData"); }
                    ImGui::SameLine(0, 12 * scale); ImGui::SetCursorPosY(112 * scale + (30 * scale - Px(12)) / 2); Label("SKSE\\Plugins\\Tailor", Palette::Ghost, 12);
                    ImGui::SetCursorPos({20 * scale, 156 * scale});
                }
                BeginList("transfer", {content, std::max(100 * scale, listBottom - ImGui::GetCursorPosY())});
                if (state.transferLoading) { ImGui::SetCursorPos({18 * scale, 18 * scale}); Label("Reading your library...", Palette::Muted, 13); }
                if (exporting) {
                    int shown = 0;
                    for (const auto& outfit : transferOutfits) {
                        if (!visible(outfit)) continue;
                        const auto id = static_cast<std::uint32_t>(Number(outfit, "id"));
                        const bool selected = state.exportIds.contains(id);
                        ImGui::PushID(static_cast<int>(id));
                        ImVec2 row; float rowWidth; const float rowHeight = 66 * scale;
                        if (Row("##export", rowHeight, selected, row, rowWidth, false, shown++ % 2)) { if (selected) state.exportIds.erase(id); else state.exportIds.insert(id); }
                        auto* draw = ImGui::GetWindowDrawList();
                        Check(draw, {row.x + 16 * scale, row.y + 24 * scale}, selected, ImGui::IsItemHovered());
                        draw->AddText(fonts.medium, Px(15), {row.x + 46 * scale, row.y + 11 * scale}, Palette::Text, Text(outfit, "name").c_str());
                        ImVec2 chip{row.x + 46 * scale, row.y + 38 * scale};
                        SexTag(draw, chip, SexOf(outfit));
                        for (const auto& category : Rows(Get(outfit, "categories"))) if (category.is_string()) {
                            auto label = category.get<std::string>();
                            if (label == "heavy") label = "Heavy Armor";
                            else if (label == "light") label = "Light Armor";
                            else if (!label.empty()) label[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(label[0])));
                            Chip(draw, chip, label);
                        }
                        const auto items = std::to_string(Number(outfit, "itemCount")) + " items";
                        draw->AddText(fonts.body, Px(13), {row.x + rowWidth - 16 * scale - fonts.body->CalcTextSizeA(Px(13), FLT_MAX, 0, items.c_str()).x, row.y + (rowHeight - Px(13)) / 2}, Palette::Muted, items.c_str());
                        ImGui::PopID();
                    }
                    if (!shown && !state.transferLoading) { ImGui::SetCursorPos({18 * scale, 18 * scale}); Label("No outfit matches these filters.", Palette::Muted, 13); }
                } else {
                    if (Rows(Get(data, "files")).empty() && !state.transferLoading) {
                        ImGui::SetCursorPos({18 * scale, 18 * scale}); ImGui::PushTextWrapPos(content - 18 * scale);
                        Label("No import files found. Place an outfit export JSON in SKSE\\Plugins\\Tailor, then refresh.", Palette::Muted, 13);
                        ImGui::PopTextWrapPos();
                    }
                    int shown = 0;
                    // Saves are off: a file can't be imported, so its rows take no click and are drawn dimmed.
                    const bool readOnly = LibraryReadOnly();
                    ImGui::BeginDisabled(readOnly);
                    for (const auto& value : Rows(Get(data, "files"))) {
                        if (!value.is_string()) continue;
                        const auto file = value.get<std::string>();
                        ImGui::PushID(file.c_str());
                        ImVec2 row; float rowWidth; const float rowHeight = 50 * scale;
                        if (Row("##file", rowHeight, state.transferPending && state.selectedFile == file, row, rowWidth, false, shown++ % 2)) { state.selectedFile = file; Emit("tailorImportOutfits", {{"file", file}}); state.transferPending = true; }
                        const bool lit = !readOnly && (ImGui::IsItemHovered() || ImGui::IsItemFocused());
                        ImGui::GetWindowDrawList()->AddText(fonts.medium, Px(14), {row.x + 16 * scale, row.y + (rowHeight - Px(14)) / 2}, readOnly ? Palette::Muted : Palette::Text, file.c_str());
                        ImGui::GetWindowDrawList()->AddText(fonts.body, Px(12), {row.x + rowWidth - 86 * scale, row.y + (rowHeight - Px(12)) / 2}, lit ? Palette::Amber : Palette::Ghost, "Import");
                        DrawIcon(ImGui::GetWindowDrawList(), Icon::ChevronRight, {row.x + rowWidth - 22 * scale, row.y + rowHeight / 2}, 11 * scale, lit ? Palette::Amber : Palette::Ghost);
                        ImGui::PopID();
                    }
                    ImGui::EndDisabled();
                }
                EndList();
                BeginActions();
                if (exporting) {
                    ImGui::SetCursorPosX(ImGui::GetWindowWidth() - 180 * scale);
                    if (Action("Export", 160 * scale, Tone::Primary, !state.exportIds.empty())) {
                        state.pendingAction = "tailorExportOutfits"; state.pendingPayload = {{"outfitIds", state.exportIds}};
                        ForgetName(); state.transferError.clear(); state.exportSucceeded = false; state.promptRequested = true;
                    }
                } else {
                    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (46 * scale - Px(12)) / 2);
                    // Saves are off: the footer says why, since this page opens from the side menu without the
                    // Manage Outfits note. The label is the last thing drawn, so nothing moves.
                    if (const auto note = Text(Get(model, "tailorSetLibraryState"), "note"); LibraryReadOnly() && !note.empty()) Label(note, Palette::DangerText, 12);
                    else Label("Only built-in categories are imported. Tailor's own data files are excluded.", Palette::Ghost, 12);
                }
                ImGui::EndDisabled();
            }
            void AddWigs()
            {
                SecondaryHeader("ADD WIGS FROM MODS", Page::Library);
                const auto& mods = Rows(Get(model, "wiggySetModWigs"));
                const auto& categories = Rows(Get(model, "wiggySetCategories"));
                const auto sex = Text(Get(model, "tailorSetTarget"), "sex");
                const auto sameSex = [&](const Model& category) { const int index = Number(category, "index"); return sex == "male" ? index >= 4 : index < 4; };
                const auto key = [](const Model& wig) { return Text(wig, "plugin") + "|" + std::to_string(Number(wig, "formId")); };
                const auto bare = [](std::string label) {
                    if (label.starts_with("Female - ")) label.erase(0, 9);
                    if (label.starts_with("Male - ")) label.erase(0, 7);
                    return label;
                };
                std::map<std::string, int> existing;
                for (const auto& category : categories) for (const auto& wig : Rows(Get(category, "wigs"))) existing[key(wig)] = Number(category, "index");
                if (state.category < 0) state.category = sex == "male" ? 4 : 0;
                const float content = ImGui::GetWindowWidth() - 40 * scale;
                ImGui::SetNextItemWidth(content * 0.58f);
                if (BeginSelect("##plugin", state.selectedPlugin.empty() ? "Choose a wig mod..." : state.selectedPlugin, state.selectedPlugin.empty())) {
                    SelectSearch("##modfilter", "Search mods...", state.pluginSearch.data(), state.pluginSearch.size());
                    SelectOptions(static_cast<int>(std::count_if(mods.begin(), mods.end(), [&](const Model& mod) { return Matches(Text(mod, "name"), state.pluginSearch.data()); })));
                    for (const auto& mod : mods) if (Matches(Text(mod, "name"), state.pluginSearch.data()) && SelectItem(Text(mod, "name"), Text(mod, "name") == state.selectedPlugin)) {
                        if (state.wigPreview) Emit("wiggyEndPreview");
                        state.wigPreview = false; state.selectedPlugin = Text(mod, "name"); state.wigRowCategories.clear(); state.search.fill(0);
                    }
                    EndSelect();
                }
                const Model* selected = nullptr;
                for (const auto& mod : mods) if (Text(mod, "name") == state.selectedPlugin) selected = &mod;
                ImGui::SameLine(0, 12 * scale); Search("Filter wigs...");
                const float toolbar = ImGui::GetCursorPosY() + 2 * scale;
                ImGui::SetCursorPos({20 * scale, toolbar + (36 * scale - Px(11)) / 2}); Eyebrow("WIG NAME");
                ImGui::SetCursorPos({ImGui::GetWindowWidth() - 240 * scale, toolbar}); ImGui::SetNextItemWidth(220 * scale);
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {14 * scale, (36 * scale - Px(14)) / 2});
                if (BeginSelect("##setAll", "Set all categories...", true)) {
                    for (const auto& category : categories) if (sameSex(category) && SelectItem(bare(Text(category, "name")), false)) {
                        state.category = Number(category, "index");
                        if (selected) for (const auto& wig : Rows(Get(*selected, "wigs"))) if (!existing.contains(key(wig))) state.wigRowCategories[key(wig)] = state.category;
                    }
                    EndSelect();
                }
                ImGui::PopStyleVar();
                ImGui::SetCursorPos({20 * scale, toolbar + 46 * scale});
                BeginList("modWigs", {content, std::max(100 * scale, ImGui::GetWindowHeight() - 102 * scale - ImGui::GetCursorPosY())});
                int unadded = 0, shown = 0;
                if (!selected) { ImGui::SetCursorPos({18 * scale, 18 * scale}); Label(mods.empty() ? "Scanning installed mods..." : "Choose a wig mod above to browse its wigs.", Palette::Muted, 13); }
                else for (const auto& wig : Rows(Get(*selected, "wigs"))) {
                    const auto id = key(wig);
                    const bool added = existing.contains(id);
                    if (!added) ++unadded;
                    if (!Matches(Text(wig, "name"), state.search.data())) continue;
                    const int rowCategory = added ? existing.at(id) : (state.wigRowCategories.contains(id) ? state.wigRowCategories.at(id) : state.category);
                    ImGui::PushID(id.c_str());
                    ImVec2 row; float rowWidth; const float rowHeight = 52 * scale;
                    ImGui::BeginDisabled(added);
                    if (Row("##wig", rowHeight, false, row, rowWidth, true, shown++ % 2)) { Emit("wiggyPreviewWig", wig); state.wigPreview = true; }
                    ImGui::EndDisabled();
                    ImGui::GetWindowDrawList()->AddText(fonts.medium, Px(14), {row.x + 16 * scale, row.y + (rowHeight - Px(14)) / 2}, added ? Palette::Ghost : Palette::Text, Text(wig, "name").c_str());
                    // Category pills, then the row action, measured from the right edge.
                    float pills = 0;
                    for (const auto& category : categories) if (sameSex(category)) pills += fonts.medium->CalcTextSizeA(Px(12), FLT_MAX, 0, bare(Text(category, "name")).c_str()).x + 28 * scale + 4 * scale;
                    ImGui::SetCursorScreenPos({row.x + rowWidth - 112 * scale - pills, row.y + 12 * scale});
                    for (const auto& category : categories) if (sameSex(category)) {
                        const int index = Number(category, "index");
                        ImGui::PushID(index);
                        if (Press(bare(Text(category, "name")).c_str(), {0, 28 * scale}, rowCategory == index ? Tone::Amber : Tone::Neutral, !added, Icon::None, 12, true)) state.wigRowCategories[id] = index;
                        ImGui::SameLine(0, 4 * scale);
                        ImGui::PopID();
                    }
                    ImGui::SetCursorScreenPos({row.x + rowWidth - 100 * scale, row.y + 11 * scale});
                    if (Press(added ? "Remove" : "Add", {84 * scale, 30 * scale}, added ? Tone::Danger : Tone::Amber, true, added ? Icon::None : Icon::Plus, 13)) {
                        auto payload = wig; payload["category"] = rowCategory;
                        Emit(added ? "wiggyRemoveWig" : "wiggyAddWig", payload);
                    }
                    ImGui::SetCursorScreenPos({row.x, row.y + rowHeight}); ImGui::Dummy({0, 0});
                    ImGui::PopID();
                }
                EndList();
                BeginActions();
                const float barTop = ImGui::GetCursorPosY();
                ImGui::SetCursorPosY(barTop + (46 * scale - Px(13)) / 2);
                Label(!selected ? "" : unadded ? std::to_string(unadded) + (unadded == 1 ? " wig ready to add" : " wigs ready to add") : "Every wig in this mod is already in your library.", Palette::Muted, 13);
                ImGui::SetCursorPos({ImGui::GetWindowWidth() - 290 * scale, barTop});
                if (Action("Cancel", 120 * scale)) Navigate(Page::Library, true);
                ImGui::SameLine();
                if (Action("Add All", 140 * scale, Tone::Primary, selected && unadded > 0)) {
                    // Every wig in one action, so the library is saved and published once (D8).
                    Model wigs = Model::array();
                    for (const auto& wig : Rows(Get(*selected, "wigs"))) {
                        const auto id = key(wig);
                        if (existing.contains(id)) continue;
                        auto row = wig; row["category"] = state.wigRowCategories.contains(id) ? state.wigRowCategories.at(id) : state.category;
                        wigs.push_back(std::move(row));
                    }
                    Emit("wiggyAddWig", {{"wigs", std::move(wigs)}});
                }
            }
            void HairColor();
            void Shell();
            std::string ExportNameError(const std::string& name)
            {
                if (name.empty()) return "Enter a filename before exporting.";
                if (name.size() > 120) return "Use a shorter filename.";
                if (name.find('.') != std::string::npos) return "Enter a name without dots or a file extension. Tailor adds .json.";
                if (name.find_first_of("<>:\"/\\|?*") != std::string::npos || std::any_of(name.begin(), name.end(), [](unsigned char ch) { return ch < 32 || ch == 127; })) return "Enter a filename without paths or special characters.";
                std::string lower = name;
                std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
                for (const char* reserved : {"assignments", "library", "outfits", "blacklist", "con", "prn", "aux", "nul", "conin$", "conout$"}) if (lower == reserved) return "That filename is reserved. Choose another name.";
                if (lower.size() == 4 && (lower.starts_with("com") || lower.starts_with("lpt")) && std::isdigit(static_cast<unsigned char>(lower[3]))) return "That filename is reserved by Windows.";
                for (const auto& value : Rows(Get(Get(model, "tailorSetTransferData"), "files"))) if (value.is_string()) {
                    std::string file = value.get<std::string>();
                    std::transform(file.begin(), file.end(), file.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
                    if (file == lower + ".json") return "That export already exists. Choose a different filename.";
                }
                return "";
            }
            // Forge modal: walnut body, 18px corners, parchment hairline, gold heat behind it.
            bool BeginModal(const char* id)
            {
                ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f, ImGui::GetIO().DisplaySize.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
                ImGui::SetNextWindowSize(ImVec2(480 * scale, 0), ImGuiCond_Appearing);
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {24 * scale, 22 * scale});
                ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 18 * scale);
                ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, scale);
                ImGui::PushStyleColor(ImGuiCol_PopupBg, IM_COL32(31, 25, 18, 255));
                ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(245, 158, 11, 70));
                const bool open = ImGui::BeginPopupModal(id, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar);
                if (!open) { ImGui::PopStyleColor(2); ImGui::PopStyleVar(3); }
                return open;
            }
            void EndModal() { ImGui::EndPopup(); ImGui::PopStyleColor(2); ImGui::PopStyleVar(3); }
            void Dialogs()
            {
                if (state.confirmRequested) { ImGui::OpenPopup("Confirm##tailor"); state.confirmRequested = false; }
                if (state.promptRequested) { ImGui::OpenPopup("Name##tailor"); state.promptRequested = false; }
                const float half = (480 * scale - 48 * scale - 10 * scale) / 2;
                if (BeginModal("Confirm##tailor")) {
                    if (BackPressed() && !state.controllerEditing) { backConsumed = true; ImGui::CloseCurrentPopup(); }
                    // Retagging loses nothing, so it confirms in gold under its own name.
                    const bool tagging = state.pendingAction == "tailorSetOutfitSex";
                    Eyebrow("CONFIRM", "", true, tagging ? Palette::Amber : Palette::DangerText);
                    ImGui::Dummy({0, 2 * scale});
                    Label(state.confirmTitle, Palette::Text, 20, fonts.bold);
                    Wrapped(state.confirmMessage, Palette::Muted);
                    ImGui::Dummy({0, 8 * scale});
                    const char* confirmLabel = tagging ? "Set Sex" : state.pendingAction.find("Delete") != std::string::npos ? "Delete" : state.pendingAction.find("Clear") != std::string::npos ? "Clear" : "Confirm";
                    if (Action(confirmLabel, half, tagging ? Tone::Primary : Tone::Danger)) {
                        if (state.pendingAction == "tailorDeleteOutfit" && state.outfitPreview) { Emit("tailorCancelCreateOutfit"); state.outfitPreview = false; }
                        Emit(state.pendingAction, state.pendingPayload);
                        if (state.pendingAction == "tailorDeleteCategory") { state.category = -1; ForgetName(); }
                        if (state.pendingAction == "wiggyDeleteCustomColor") state.selectedColor = -1;
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::SameLine(); if (Action("Cancel", half)) ImGui::CloseCurrentPopup();
                    EndModal();
                }
                if (BeginModal("Name##tailor")) {
                    const bool exporting = state.pendingAction == "tailorExportOutfits";
                    const bool busy = exporting && state.transferPending;
                    if (exporting && state.exportSucceeded) { state.exportSucceeded = false; ImGui::CloseCurrentPopup(); }
                    if (!busy && BackPressed() && !state.controllerEditing) { backConsumed = true; ImGui::CloseCurrentPopup(); }
                    Eyebrow(exporting ? "EXPORT" : "COPY OUTFIT");
                    ImGui::Dummy({0, 2 * scale});
                    Label(exporting ? "Name this export" : "Name the copy", Palette::Text, 20, fonts.bold);
                    if (exporting) Wrapped("Saved as a JSON file in SKSE\\Plugins\\Tailor.", Palette::Muted);
                    ImGui::Dummy({0, 4 * scale});
                    ImGui::BeginDisabled(busy);
                    ImGui::SetNextItemWidth(-1);
                    if (TextInput("##newname", exporting ? "File name, without .json" : nullptr, state.name.data(), state.name.size()) && exporting) state.transferError.clear();
                    if (exporting && !state.transferError.empty()) Wrapped(state.transferError, Palette::DangerText);
                    const bool copyTaken = !exporting && OutfitNameTaken(Rows(Get(model, "tailorSetOutfits")), EffectiveName());
                    if (copyTaken) Wrapped(OutfitTakenMessage(EffectiveName()), Palette::DangerText);
                    ImGui::Dummy({0, 8 * scale});
                    if (Action(exporting ? "Export" : "Save", half, Tone::Primary, (exporting ? state.name[0] != '\0' : !BlankName(state.name.data()) && !copyTaken))) {
                        std::string name = exporting ? std::string(state.name.data()) : EffectiveName();
                        const auto first = name.find_first_not_of(" \t\r\n"), last = name.find_last_not_of(" \t\r\n");
                        name = first == std::string::npos ? "" : name.substr(first, last - first + 1);
                        state.transferError = exporting ? ExportNameError(name) : "";
                        if (state.transferError.empty() && !name.empty()) {
                            auto payload = state.pendingPayload; payload["name"] = name; Emit(state.pendingAction, payload);
                            if (exporting) state.transferPending = true;
                            else ImGui::CloseCurrentPopup();
                        }
                    }
                    ImGui::SameLine(); if (Action("Cancel", half)) ImGui::CloseCurrentPopup();
                    ImGui::EndDisabled();
                    EndModal();
                }
            }
        };

        void Screen::HairColor()
        {
            const bool custom = state.page == Page::CustomColors;
            // Returning within the hair editor must retain its native editing lease.
            if (Header(custom ? "CUSTOM COLORS" : "HAIR COLOR", true)) { if (custom) state.page = Page::HairColor; else Navigate(Page::Main, true); }
            const auto& data = Get(model, "wiggySetHairColor");
            auto rgbPayload = [&] { return Model{{"r", static_cast<int>(std::round(state.rgb[0] * 255))}, {"g", static_cast<int>(std::round(state.rgb[1] * 255))}, {"b", static_cast<int>(std::round(state.rgb[2] * 255))}}; };
            if (custom) {
                const float diameter = std::min(260 * scale, std::max(190 * scale, ImGui::GetWindowHeight() - 450 * scale));
                const float unit = diameter / 260;
                const float x = (ImGui::GetWindowWidth() - diameter - 86 * scale) / 2;
                ImGui::SetCursorPos(ImVec2(x, 112 * scale));
                const auto start = ImGui::GetCursorScreenPos();
                const ImVec2 center(start.x + diameter / 2, start.y + diameter / 2);
                float hue, saturation, value;
                ImGui::ColorConvertRGBtoHSV(state.rgb[0], state.rgb[1], state.rgb[2], hue, saturation, value);
                // Hue is undefined for gray, and saturation is undefined for black.
                // Retain the picker coordinates until RGB becomes chromatic again.
                if (saturation == 0) hue = state.colorHue;
                if (value == 0) saturation = state.colorSaturation;
                ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(0, 0, 0, 0));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(0, 0, 0, 0));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(0, 0, 0, 0));
                ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(0, 0, 0, 0));
                ImGui::Button("##hueWheel", ImVec2(diameter, diameter));
                if (ImGui::IsItemActivated()) {
                    const auto mouse = ImGui::GetIO().MousePos;
                    const float dx = mouse.x - center.x, dy = mouse.y - center.y;
                    state.colorDrag = std::hypot(dx, dy) >= 96 * unit ? 1 : (std::abs(dx) <= 62 * unit && std::abs(dy) <= 62 * unit ? 2 : 0);
                }
                if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                    const auto mouse = ImGui::GetIO().MousePos;
                    if (state.colorDrag == 1) {
                        hue = std::atan2(mouse.y - center.y, mouse.x - center.x) / (2 * std::numbers::pi_v<float>);
                        if (hue < 0) hue += 1;
                    } else if (state.colorDrag == 2) {
                        saturation = std::clamp((mouse.x - center.x + 62 * unit) / (124 * unit), 0.0f, 1.0f);
                        value = 1 - std::clamp((mouse.y - center.y + 62 * unit) / (124 * unit), 0.0f, 1.0f);
                    }
                    ImGui::ColorConvertHSVtoRGB(hue, saturation, value, state.rgb[0], state.rgb[1], state.rgb[2]);
                }
                state.colorHue = hue; state.colorSaturation = saturation;
                ImGui::PopStyleColor(4);
                auto* draw = ImGui::GetWindowDrawList();
                auto hsvColor = [](float h, float sat, float val) { float r, g, b; ImGui::ColorConvertHSVtoRGB(h, sat, val, r, g, b); return ImGui::ColorConvertFloat4ToU32(ImVec4(r, g, b, 1)); };
                for (int sector = 0; sector < 180; ++sector) {
                    const float angle = sector * 2 * std::numbers::pi_v<float> / 180;
                    draw->PathArcTo(center, 112 * unit, angle, angle + 2 * std::numbers::pi_v<float> / 180 + 0.004f, 2);
                    draw->PathStroke(hsvColor(sector / 180.0f, 1, 1), 0, 24 * unit);
                }
                const ImVec2 squareMin(center.x - 62 * unit, center.y - 62 * unit), squareMax(center.x + 62 * unit, center.y + 62 * unit);
                draw->AddRectFilledMultiColor(squareMin, squareMax, IM_COL32_WHITE, hsvColor(hue, 1, 1), hsvColor(hue, 1, 1), IM_COL32_WHITE);
                draw->AddRectFilledMultiColor(squareMin, squareMax, IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0), IM_COL32_BLACK, IM_COL32_BLACK);
                const float angle = hue * 2 * std::numbers::pi_v<float>;
                draw->AddCircle(ImVec2(center.x + std::cos(angle) * 112 * unit, center.y + std::sin(angle) * 112 * unit), 7 * unit, IM_COL32_WHITE, 20, 2 * scale);
                draw->AddCircle(ImVec2(squareMin.x + saturation * 124 * unit, squareMin.y + (1 - value) * 124 * unit), 5 * unit, IM_COL32_WHITE, 20, 2 * scale);
                ImGui::SetCursorPos(ImVec2(x + diameter + 38 * scale, 122 * scale));
                ImGui::ColorButton("Selected RGB", ImVec4(state.rgb[0], state.rgb[1], state.rgb[2], 1), ImGuiColorEditFlags_NoDragDrop, ImVec2(46 * scale, diameter - 20 * scale));
                ImGui::SetCursorPos(ImVec2((ImGui::GetWindowWidth() - 330 * scale) / 2, 112 * scale + diameter + 28 * scale));
                for (int channel = 0; channel < 3; ++channel) {
                    if (channel) ImGui::SameLine();
                    Label(channel == 0 ? "R" : channel == 1 ? "G" : "B", Palette::Copper, 13);
                    ImGui::SameLine(); ImGui::SetNextItemWidth(70 * scale); ImGui::PushID(channel);
                    int component = static_cast<int>(std::round(state.rgb[channel] * 255));
                    if (ImGui::SliderInt("##rgb", &component, 0, 255, "%d", ImGuiSliderFlags_AlwaysClamp)) state.rgb[channel] = std::clamp(component, 0, 255) / 255.0f;
                    ImGui::PopID();
                }
                ImGui::SetCursorPosX(20 * scale);
                if (Primary("+ Add to Custom Colors", -1 / scale)) Emit("wiggyAddCustomColor", rgbPayload());
                const auto& colors = Rows(Get(data, "customColors"));
                ImGui::Dummy({0, 2 * scale}); Eyebrow("SAVED COLORS", std::to_string(colors.size()) + (colors.size() == 1 ? " color" : " colors"));
                Child("savedColors", ImVec2(0, std::max(80 * scale, ImGui::GetContentRegionAvail().y - 92 * scale)), ImGuiChildFlags_Borders);
                int index = 0;
                const int columns = std::max(1, static_cast<int>(ImGui::GetContentRegionAvail().x / (42 * scale)));
                for (const auto& color : colors) {
                    ImGui::PushID(index);
                    if (index % columns) ImGui::SameLine(0, 7 * scale);
                    const ImVec4 rgb(Number(color, "r") / 255.0f, Number(color, "g") / 255.0f, Number(color, "b") / 255.0f, 1);
                    if (ImGui::ColorButton("##saved", rgb, 0, ImVec2(32 * scale, 32 * scale))) {
                        state.selectedColor = index;
                        state.rgb[0] = rgb.x; state.rgb[1] = rgb.y; state.rgb[2] = rgb.z;
                    }
                    if (state.selectedColor == index) ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), Palette::Amber, 4 * scale, 0, 2 * scale);
                    ++index; ImGui::PopID();
                }
                ImGui::EndChild();
                ImGui::SetCursorPosY(ImGui::GetWindowHeight() - 76 * scale);
                const bool selected = state.selectedColor >= 0 && state.selectedColor < static_cast<int>(colors.size());
                if (Button("Delete Selected", (ImGui::GetContentRegionAvail().x / scale - 10) / 2, selected, true)) Confirm("Delete Color", "Remove this color from your library?", "wiggyDeleteCustomColor", colors[state.selectedColor]);
            } else {
                const std::string current = Flag(data, "isDefault") ? "Current: Default" : "Current RGB: " + std::to_string(Number(data, "r")) + ", " + std::to_string(Number(data, "g")) + ", " + std::to_string(Number(data, "b"));
                ImGui::SetCursorPosY(102 * scale);
                ImGui::SetCursorPosX((ImGui::GetWindowWidth() - ImGui::CalcTextSize(current.c_str()).x) / 2); Label(current, Palette::Muted, 16);
                ImGui::SetCursorPos(ImVec2(20 * scale, 146 * scale)); Eyebrow("PRESETS");
                static const Model presets = Model::parse(HairPresets);
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8 * scale, 8 * scale));
                Child("presets", ImVec2(0, std::max(100 * scale, ImGui::GetContentRegionAvail().y - 138 * scale)), ImGuiChildFlags_Borders);
                const int columns = std::max(1, static_cast<int>((ImGui::GetContentRegionAvail().x + 4 * scale) / (32 * scale)));
                int index = 0, column = 0;
                // One section per family: a ruled heading with its count, then its swatches.
                auto section = [&](std::string title, std::size_t count) {
                    if (index) ImGui::Dummy({0, 6 * scale});
                    std::transform(title.begin(), title.end(), title.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
                    Eyebrow(title, std::to_string(count));
                    ImGui::Dummy({0, 2 * scale});
                    column = 0;
                };
                auto swatch = [&](const Model& preset) {
                    ImGui::PushID(index++);
                    if (column++ % columns) ImGui::SameLine(0, 4 * scale);
                    const ImVec4 color(Number(preset, "r") / 255.0f, Number(preset, "g") / 255.0f, Number(preset, "b") / 255.0f, 1);
                    if (ImGui::ColorButton("##preset", color, ImGuiColorEditFlags_NoTooltip, ImVec2(28 * scale, 28 * scale))) Emit("wiggyConfirmHairColor", {{"r", Number(preset, "r")}, {"g", Number(preset, "g")}, {"b", Number(preset, "b")}});
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", Text(preset, "name", "Custom color").c_str());
                    ImGui::PopID();
                };
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4 * scale, 4 * scale));
                std::string family;
                for (const auto& preset : presets) {
                    if (const auto next = Text(preset, "family"); next != family) {
                        family = next;
                        section(family, static_cast<std::size_t>(std::count_if(presets.begin(), presets.end(), [&](const Model& other) { return Text(other, "family") == family; })));
                    }
                    swatch(preset);
                }
                if (const auto& saved = Rows(Get(data, "customColors")); !saved.empty()) {
                    section("Saved", saved.size());
                    for (const auto& color : saved) swatch(color);
                }
                ImGui::PopStyleVar(); ImGui::EndChild(); ImGui::PopStyleVar();
                ImGui::SetCursorPosY(ImGui::GetWindowHeight() - 146 * scale);
                if (Button("Custom Colors   >", -1 / scale)) state.page = Page::CustomColors;
                ImGui::SetCursorPosY(ImGui::GetWindowHeight() - 76 * scale);
                if (Button("Reset to Default", (ImGui::GetContentRegionAvail().x / scale - 10) / 2, true, true)) Emit("wiggyResetHairColor", Model::object());
            }
            ImGui::SameLine(); if (Button("Back", -1 / scale)) { if (custom) state.page = Page::HairColor; else Navigate(Page::Main, true); }
        }

        void Screen::Shell()
        {
            auto& io = ImGui::GetIO();
            const float width = io.DisplaySize.x, height = io.DisplaySize.y;
            const float rail = 56 * scale, footer = 58 * scale;
            const auto legend = Legend();
            const float legendHeight = Controller() ? DrawControllerLegend(legend, fonts, scale, width) : 0;
            const float requestedWork = state.page == Page::Create && !state.wigs ? 1380 * scale : 840 * scale;
            const float work = std::min(requestedWork, std::max(320 * scale, width - 2 * rail - 220 * scale));
            const float contentHeight = std::max(1.0f, height - footer - legendHeight);
            const float stageX = state.wigs ? rail : rail + work;
            const float stageWidth = std::max(1.0f, width - 2 * rail - work);
            const bool active = Flag(Get(model, "tailorSetPreviewState"), "active");
            const char* rotationHint = "Hold arrows or drag the preview to rotate";
            const float hintWrapWidth = std::max(1.0f, stageWidth - 16 * scale);
            ImGui::PushFont(fonts.body, 12 * scale * FontMetricScale);
            const auto hintSize = ImGui::CalcTextSize(rotationHint, nullptr, false, hintWrapWidth);
            ImGui::PopFont();
            const float previewFooter = Controller() ? 102 * scale : std::max(78 * scale, 62 * scale + hintSize.y);
            ImGui::PushFont(fonts.medium, 14 * scale * FontMetricScale);
            const float resetWidth = ImGui::CalcTextSize("Reset to Front").x + 2 * ImGui::GetStyle().FramePadding.x;
            ImGui::PopFont();
            const float arrowWidth = std::clamp((stageWidth - resetWidth - 2 * ImGui::GetStyle().ItemSpacing.x - 16 * scale) / 2, 24 * scale, 50 * scale);
            const float controlsWidth = 2 * arrowWidth + resetWidth + 2 * ImGui::GetStyle().ItemSpacing.x;
            result.viewport = {stageX / width, 0, stageWidth / width, std::max(1.0f, contentHeight - (active ? previewFooter : 0)) / height};
            result.hairMode = state.wigs;
            const ImGuiWindowFlags fixed = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | (interactive ? 0 : ImGuiWindowFlags_NoInputs);
            auto window = [&](const char* id, ImVec2 position, ImVec2 size, ImGuiWindowFlags extra = 0) {
                ImGui::SetNextWindowPos(position); ImGui::SetNextWindowSize(size);
                if (state.controllerRotating && std::string_view(id) != "Tailor Live Preview") extra |= ImGuiWindowFlags_NoNavInputs | ImGuiWindowFlags_NoNavFocus;
                ImGui::Begin(id, nullptr, fixed | extra);
            };
            auto railButton = [&](const char* tip, Page page, bool wig, int icon) {
                const bool selected = state.wigs == wig && state.page == page;
                ImGui::PushStyleColor(ImGuiCol_Button, selected ? IM_COL32(245, 158, 11, 24) : IM_COL32(0, 0, 0, 0));
                ImGui::PushStyleColor(ImGuiCol_Border, selected ? IM_COL32(245, 158, 11, 95) : IM_COL32(0, 0, 0, 0));
                ImGui::PushID(tip);
                const bool pressed = ImGui::Button("##rail", ImVec2(42 * scale, 42 * scale));
                const auto pos = ImGui::GetItemRectMin();
                auto* draw = ImGui::GetWindowDrawList();
                const ImU32 color = selected ? Palette::Amber : Palette::Muted;
                auto p = [&](float x, float y) { return ImVec2(pos.x + (11 + x) * scale, pos.y + (11 + y) * scale); };
                auto line = [&](float x, float y, float xx, float yy) { draw->AddLine(p(x, y), p(xx, yy), color, 1.7f * scale); };
                if (icon == 1) {
                    for (int y = 0; y < 2; ++y) for (int x = 0; x < 2; ++x) draw->AddRect(p(x * 11.0f, y * 11.0f), p(x * 11.0f + 7, y * 11.0f + 7), color, scale, 0, 1.6f * scale);
                } else if (icon == 2) {
                    for (float y : {1.0f, 7.0f, 13.0f}) { line(0, y + 4, 10, y + 9); line(10, y + 9, 20, y + 4); }
                    line(0, 5, 10, 0); line(10, 0, 20, 5);
                } else if (icon == 3 || icon == 4) {
                    const float y = icon == 3 ? 1.0f : 13.0f;
                    line(10, 1, 10, 14); line(6, icon == 3 ? 5.0f : 9.0f, 10, y); line(14, icon == 3 ? 5.0f : 9.0f, 10, y);
                    line(1, 13, 1, 20); line(1, 20, 19, 20); line(19, 20, 19, 13);
                } else if (icon == 5) {
                    draw->AddCircle(p(10, 9), 8 * scale, color, 20, 1.6f * scale); line(5, 15, 5, 20); line(5, 20, 15, 20); line(15, 20, 15, 15);
                } else if (icon == 6) {
                    draw->AddCircle(p(10, 10), 9 * scale, color, 24, 1.6f * scale); draw->AddCircle(p(10, 10), 3 * scale, color, 16, 1.6f * scale);
                    line(10, 1, 10, 6); line(10, 14, 10, 19); line(1, 10, 6, 10); line(14, 10, 19, 10);
                } else {
                    const ImVec2 points[] = {p(6, 1), p(1, 3), p(0, 8), p(4, 11), p(6, 9), p(7, 20), p(14, 20), p(15, 9), p(17, 11), p(20, 8), p(19, 3), p(14, 1), p(10, 5)};
                    draw->AddPolyline(points, 13, color, ImDrawFlags_Closed, 1.6f * scale);
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
                ImGui::PopID(); ImGui::PopStyleColor(2);
                if (pressed) Navigate(page, wig);
            };
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(7 * scale, 16 * scale));
            ImGui::PushStyleColor(ImGuiCol_WindowBg, Palette::ObsidianLight);
            window("Tailor Outfit Rail", ImVec2(0, 0), ImVec2(rail, contentHeight));
            railButton("Dress", Page::Main, false, 0);
            railButton("Manage Outfits", Page::Library, false, 1);
            railButton("Outfit Situations", Page::Situations, false, 2);
            railButton("Export Outfits", Page::Export, false, 3);
            railButton("Import Outfits", Page::Import, false, 4);
			railButton("Discovered Sets", Page::Discovered, false, 6);
            ImGui::End();
            window("Tailor Wig Rail", ImVec2(width - rail, 0), ImVec2(rail, contentHeight));
            railButton("Wigs", Page::Main, true, 5);
            railButton("Manage Wigs", Page::Library, true, 1);
            railButton("Hair Color", Page::HairColor, true, 6);
            railButton("Wig Situations", Page::Situations, true, 2);
            ImGui::End(); ImGui::PopStyleColor(); ImGui::PopStyleVar();

            window("Tailor Work", ImVec2(state.wigs ? width - rail - work : rail, 0), ImVec2(work, contentHeight), state.page == Page::Settings ? ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse : 0);
            const auto workOrigin = ImGui::GetWindowPos();
            DrawPageGlow(ImGui::GetWindowDrawList(), workOrigin, ImVec2(work, contentHeight));
            ImGui::GetWindowDrawList()->AddLine(workOrigin, {workOrigin.x, workOrigin.y + contentHeight}, Palette::Border);
            ImGui::GetWindowDrawList()->AddLine({workOrigin.x + work - 1, workOrigin.y}, {workOrigin.x + work - 1, workOrigin.y + contentHeight}, Palette::Border);
            switch (state.page) {
            case Page::Main: Main(); break;
            case Page::Cycle: Cycle(); break;
            case Page::Library: Library(); break;
			case Page::Discovered: Discovered(); break;
            case Page::Create: Create(); break;
            case Page::Categories: Categories(); break;
            case Page::Blacklist: Blacklist(); break;
            case Page::Situations: Situations(); break;
            case Page::Export: case Page::Import: Transfer(); break;
            case Page::AddWigs: AddWigs(); break;
            case Page::HairColor: case Page::CustomColors: HairColor(); break;
            case Page::Settings: {
                const auto& published = Get(model, "tailorSetSettings");
                const SettingsSwitches switches{Flag(published, "disableFavorite"), Flag(published, "hideWeapons"),
                    Flag(published, "hideHelmets")};
                std::optional<SettingsChange> change;
                if (DrawSettingsScreen(fonts, scale, switches, change)) Navigate(state.settingsReturn, state.settingsReturnWigs);
                // The page focuses its Back button on entry; the work window's default focus must not reset that.
                controllerFocusChosen = true;
                if (change) Emit("tailorSetSetting", {{"name", change->name}, {"on", change->on}});
                break;
            }
            }
            Dialogs();
            ImGui::End();

            window("Tailor Live Preview", ImVec2(stageX, 0), ImVec2(stageWidth, contentHeight), ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            if (active) {
                auto* draw = ImGui::GetWindowDrawList();
                draw->AddRectFilledMultiColor(ImVec2(stageX, contentHeight - previewFooter), ImVec2(stageX + stageWidth, contentHeight), IM_COL32(38, 28, 17, 255), IM_COL32(38, 28, 17, 255), Palette::Floor, Palette::Floor);
                ImGui::SetCursorPos(ImVec2(0, 0));
                ImGui::InvisibleButton("Orbit preview", ImVec2(stageWidth, std::max(1.0f, contentHeight - previewFooter - 22 * scale)));
                if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) state.yaw += io.MouseDelta.x / stageWidth * 2 * std::numbers::pi_v<float>;
                float controlsY = contentHeight - previewFooter + 4 * scale;
                if (Controller()) {
                    ImGui::SetCursorPos(ImVec2(std::max(0.0f, (stageWidth - 200 * scale) / 2), controlsY));
                    if (Button("Rotate preview", std::min(200.0f, stageWidth / scale - 16), !Flag(model, "_controllerCursor"))) {
                        if (state.controllerRotating) state.controllerRotating = false; else EnterRotation();
                    }
                    controlsY += 48 * scale;
                }
                ImGui::SetCursorPos(ImVec2(std::max(0.0f, (stageWidth - controlsWidth) / 2), controlsY));
                auto rotateArrow = [&](const char* label, float direction) {
                    const bool pressed = Press(label, {arrowWidth, 40 * scale}, Tone::Neutral, true, direction < 0 ? Icon::RotateLeft : Icon::RotateRight, 15, false, true);
                    if (pressed && Controller() && !Flag(model, "_controllerCursor") && ImGui::GetCurrentContext()->NavInputSource == ImGuiInputSource_Gamepad) {
                        if (!state.controllerRotating) EnterRotation(false);
                        state.yaw += direction * std::numbers::pi_v<float> / 12;
                    }
                    if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) state.yaw += direction * std::min(io.DeltaTime, 0.05f) * std::numbers::pi_v<float> / 3;
                };
                rotateArrow("<##rotate", -1);
                ImGui::SameLine(); if (Button("Reset to Front")) state.yaw = 0;
                ImGui::SameLine(); rotateArrow(">##rotate", 1);
                if (!Controller()) {
                    ImGui::SetCursorPos(ImVec2(std::max(0.0f, (stageWidth - hintSize.x) / 2), contentHeight - hintSize.y - 8 * scale));
                    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + hintWrapWidth);
                    Label(rotationHint, Palette::Muted, 12);
                    ImGui::PopTextWrapPos();
                }
            } else {
                ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(stageX, 0), ImVec2(stageX + stageWidth, contentHeight), Palette::Obsidian);
                ImGui::SetCursorPos(ImVec2(35 * scale, contentHeight / 2 - 24 * scale));
                const auto& stageTarget = Get(model, "tailorSetTarget");
                const auto notice = Text(stageTarget, "notice");
                Eyebrow(Text(stageTarget, "name").empty() ? "THE COURT IS EMPTY" : "LIVE PREVIEW");
                ImGui::SetCursorPosX(35 * scale);
                Wrapped(Text(stageTarget, "name").empty() ? (notice.empty() ? "Look at an NPC, then reopen this menu." : notice) : Text(Get(model, "tailorSetPreviewState"), "message", "Preparing live NPC preview..."), Palette::Muted);
            }
            ImGui::End();

            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(22 * scale, 10 * scale));
            window("Tailor Counter", ImVec2(0, contentHeight), ImVec2(width, footer));
            auto* counter = ImGui::GetWindowDrawList();
            counter->AddRectFilled({0, contentHeight}, {width, contentHeight + footer}, Palette::ObsidianLight);
            counter->AddLine({0, contentHeight}, {width, contentHeight}, Palette::Border);
            const float counterMiddle = contentHeight + footer / 2;
            if (!Controller()) Tracked(counter, {22 * scale, counterMiddle - Px(10) / 2}, "SHIFT  +  Z   CLOSE", fonts.medium, 10, Palette::Muted, 1.6f);
            // Nameplate: the client's name in ceremonial caps, parchment running to gold leaf.
            std::string name = Text(Get(model, "tailorSetTarget"), "name", "Tailor");
            if (name.empty()) name = "Tailor";
            std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
            static constexpr ImU32 leaf[] = {IM_COL32(244, 235, 216, 255), IM_COL32(217, 202, 167, 255), IM_COL32(184, 115, 51, 255), IM_COL32(251, 191, 36, 255)};
            const float nameWidth = TrackedWidth(name, fonts.heading, 17, 1.4f);
            Tracked(counter, {(width - nameWidth) * 0.5f, contentHeight + 8 * scale}, name, fonts.heading, 17, Palette::Copper, 1.4f, leaf, 4);
            const auto outfit = Text(Get(model, "tailorSetTarget"), "currentOutfit");
            const auto wig = Text(Get(model, "wiggySetTarget"), "currentWig");
            const std::string wearingValue = outfit.empty() ? (Flag(Get(model, "tailorSetTarget"), "isPlayer") ? "Own Gear" : "Default Outfit") : outfit, hairValue = wig.empty() ? "Default Hair" : wig;
            const float labelWidth = TrackedWidth("WEARING", fonts.bold, 9, 1.6f), hairLabelWidth = TrackedWidth("HAIR", fonts.bold, 9, 1.6f);
            const float wearingWidth = fonts.medium->CalcTextSizeA(Px(12), FLT_MAX, 0, wearingValue.c_str()).x, hairWidth = fonts.medium->CalcTextSizeA(Px(12), FLT_MAX, 0, hairValue.c_str()).x;
            float metaX = (width - (labelWidth + wearingWidth + hairLabelWidth + hairWidth + 12 * scale + 22 * scale)) * 0.5f;
            const float metaY = contentHeight + 35 * scale;
            Tracked(counter, {metaX, metaY + (Px(12) - Px(9)) / 2}, "WEARING", fonts.bold, 9, Palette::Muted, 1.6f); metaX += labelWidth + 6 * scale;
            counter->AddText(fonts.medium, Px(12), {metaX, metaY}, Palette::Text, wearingValue.c_str()); metaX += wearingWidth + 16 * scale;
            Tracked(counter, {metaX, metaY + (Px(12) - Px(9)) / 2}, "HAIR", fonts.bold, 9, Palette::Muted, 1.6f); metaX += hairLabelWidth + 6 * scale;
            counter->AddText(fonts.medium, Px(12), {metaX, metaY}, Palette::Text, hairValue.c_str());
            // Between the NPC Tailor opened on and the player. It waits while a page
            // has work in progress, which switching would cancel. Sized to its label, but
            // a long name must not run into the centered nameplate and meta line at
            // 1280x720: the button stops 12px short of them and the name is cut to fit.
            if (const auto switchTo = Text(Get(model, "tailorSetTarget"), "switchTo"); !switchTo.empty()) {
                const float room = width - 337 * scale - std::max((width + nameWidth) * 0.5f, metaX + hairWidth) - 12 * scale;
                const std::string label = FitLabel(fonts.medium, Px(13), "Switch to ", switchTo, room - 32 * scale);
                const float switchWidth = fonts.medium->CalcTextSizeA(Px(13), FLT_MAX, 0, label.c_str()).x + 32 * scale;
                const bool busy = state.page == Page::Cycle || state.outfitPreview || state.wigPreview || state.hairEditor;
                ImGui::SetCursorPos(ImVec2(width - 337 * scale - switchWidth, 10 * scale));
                if (Press((label + "###switchTarget").c_str(), {switchWidth, 38 * scale}, Tone::Neutral, !busy, Icon::None, 13)) Emit("tailorSwitchTarget");
            }
            ImGui::SetCursorPos(ImVec2(width - 325 * scale, 10 * scale));
            if (Press("Settings", {125 * scale, 38 * scale}, Tone::Amber, state.page != Page::Cycle, Icon::Gear, 13)) Navigate(Page::Settings, false);
            Tracked(counter, {width - 190 * scale, counterMiddle - Px(10) / 2}, "V3.0.1", fonts.medium, 10, Palette::Ghost, 1.4f);
            ImGui::SetCursorPos(ImVec2(width - 22 * scale - 100 * scale, 10 * scale));
            if (Press("CLOSE", {100 * scale, 38 * scale}, Tone::Neutral, true, Icon::Close, 11)) { Leave(); Emit("tailorClose"); }
            ImGui::End(); ImGui::PopStyleVar();
            if (Controller()) {
                window("Tailor Controller Legend", ImVec2(0, contentHeight + footer), ImVec2(width, legendHeight), ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
                auto* draw = ImGui::GetWindowDrawList();
                draw->AddRectFilled(ImVec2(0, contentHeight + footer), ImVec2(width, height), IM_COL32(14, 11, 8, 255));
                draw->AddLine(ImVec2(0, contentHeight + footer), ImVec2(width, contentHeight + footer), IM_COL32(244, 235, 216, 46));
                DrawControllerLegend(legend, fonts, scale, width, draw, contentHeight + footer);
                ImGui::End();
            }

            // Toast: one pill above the counter on the foreground layer, so neither panel
            // can cover it. It reads, then leaves on its own; longer notes stay longer.
            if (state.message != state.toastShown) { state.toastShown = state.message; state.toastAge = 0; }
            if (!state.message.empty()) {
                state.toastAge += io.DeltaTime;
                const float life = std::clamp(2.2f + state.message.size() * 0.04f, 2.2f, 9.0f);
                if (state.toastAge >= life) { state.message.clear(); state.toastShown.clear(); state.messageDanger = false; }
            }
            if (!state.message.empty()) {
                const float fade = std::clamp(std::min(state.toastAge / 0.2f, (std::clamp(2.2f + state.message.size() * 0.04f, 2.2f, 9.0f) - state.toastAge) / 0.22f), 0.0f, 1.0f);
                const auto alpha = [&](ImU32 color) { return (color & ~IM_COL32_A_MASK) | (static_cast<ImU32>(((color >> IM_COL32_A_SHIFT) & 0xFF) * fade) << IM_COL32_A_SHIFT); };
                const float wrap = std::min(620 * scale, width - 2 * rail - 80 * scale);
                const auto textSize = fonts.medium->CalcTextSizeA(Px(13), FLT_MAX, wrap, state.message.c_str());
                const bool single = textSize.y <= Px(13) * 1.5f;
                const ImVec2 size{textSize.x + 50 * scale, textSize.y + 22 * scale};
                const ImVec2 min{(width - size.x) * 0.5f, contentHeight - 24 * scale - size.y + (1 - fade) * 8 * scale}, max{min.x + size.x, min.y + size.y};
                const float rounding = single ? size.y / 2 : 12 * scale;
                const ImU32 accent = state.messageDanger ? Palette::Danger : Palette::Amber;
                auto* top = ImGui::GetForegroundDrawList();
                for (int ring = 3; ring >= 1; --ring) top->AddRect({min.x - ring * 2 * scale, min.y - ring * 2 * scale}, {max.x + ring * 2 * scale, max.y + ring * 2 * scale}, alpha((accent & ~IM_COL32_A_MASK) | (14u << IM_COL32_A_SHIFT)), rounding + ring * 2 * scale, 0, 2 * scale);
                top->AddRectFilled(min, max, alpha(IM_COL32(31, 25, 18, 245)), rounding);
                top->AddRect(min, max, alpha((accent & ~IM_COL32_A_MASK) | (96u << IM_COL32_A_SHIFT)), rounding);
                top->AddCircleFilled({min.x + 20 * scale, min.y + 11 * scale + Px(13) / 2}, 3 * scale, alpha(accent), 12);
                top->AddText(fonts.medium, Px(13), {min.x + 34 * scale, min.y + 11 * scale}, alpha(Palette::Text), state.message.c_str(), nullptr, wrap);
            }
            if (!backConsumed && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId) && BackPressed()) {
                if (state.page == Page::Main) { Leave(); Emit("tailorClose"); }
                else if (state.page == Page::Cycle) { const auto page = state.situation ? Page::Situations : Page::Main; Navigate(page, state.wigs); }
                else if (state.page == Page::Settings) Navigate(state.settingsReturn, state.settingsReturnWigs);
                else if (state.page == Page::CustomColors) state.page = Page::HairColor;
                else if (state.page == Page::Categories || state.page == Page::Blacklist || state.page == Page::AddWigs) Navigate(Page::Library, state.wigs);
                else Navigate(Page::Main, state.wigs);
            }
            state.yaw = std::remainder(state.yaw, 2 * std::numbers::pi_v<float>);
            result.yaw = state.yaw;
            RememberControllerScope();
        }
    }

    FrameResult DrawTailor(const Model& model, ScreenState& state, const Fonts& fonts, bool interactive)
    {
        auto& io = ImGui::GetIO();
        if (io.DisplaySize.x <= 0 || io.DisplaySize.y <= 0) return {};
        const auto& value = Get(model, "tailorSetPreviewOpenGeneration");
        const auto generation = value.is_number_integer() ? value.get<std::uint64_t>() : 0;
        if (state.generation != generation) {
            state = ScreenState{};
            // Dear ImGui's context outlives each open, and a dialog open when Tailor closed would stay on its popup
            // stack: Dialogs() would draw it again, empty, on this open. The new session closes what the old one left.
            if (ImGui::GetCurrentContext()->OpenPopupStack.Size > 0) ImGui::ClosePopupToLevel(0, false);
            state.generation = generation;
            // Opening receives a complete model snapshot. Old result callbacks are
            // not replayable state and must not navigate or mutate the new session.
            const auto& versions = Get(model, "_versions");
            if (versions.is_object()) for (const auto& [topic, version] : versions.items()) {
                if (version.is_number_integer()) state.consumed[topic] = version.get<std::uint64_t>();
            }
            const auto& color = Get(model, "wiggySetHairColor");
            if (color.contains("r") && !Flag(color, "isDefault")) {
                state.rgb[0] = Number(color, "r") / 255.0f;
                state.rgb[1] = Number(color, "g") / 255.0f;
                state.rgb[2] = Number(color, "b") / 255.0f;
            }
        }
        Screen screen{model, state, fonts, {}, Scale(io.DisplaySize.y), interactive};
        ApplyTheme(screen.scale);
        ImGui::PushFont(fonts.body, 14 * screen.scale * FontMetricScale);
        ImGui::BeginDisabled(!interactive);
        screen.Events();
        screen.ControllerActions();
        screen.Shell();
        ImGui::EndDisabled();
        ImGui::PopFont();
        if (!interactive) screen.result.actions.clear();
        return screen.result;
    }
}
