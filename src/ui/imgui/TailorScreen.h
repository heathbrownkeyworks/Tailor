#pragma once
#include "Theme.h"
#include "UiState.h"
#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <set>

namespace Tailor::ImGuiUI
{
    enum class Page { Main, Cycle, Library, Discovered, Create, Categories, Blacklist, Situations, Export, Import, HairColor, CustomColors, AddWigs, Settings };
    struct ScreenState
    {
        Page page = Page::Main;
        Page settingsReturn = Page::Main;
        bool settingsReturnWigs = false;
        bool wigs = false;
        std::uint64_t generation = 0;
        std::map<std::string, std::uint64_t> consumed;
        std::array<char, 256> search{}, name{}, pluginSearch{}, newCategoryName{};
        // A name too long for the name box: as the box was given it (cut between characters) and in full. Both are
        // empty unless the name had to be cut (TailorScreen.cpp, SetName).
        std::string nameCut, nameFull;
        std::string selectedPlugin, categoryName, selectedFile, message, slotFilter;
        std::string exportArmorFilter, exportCategoryFilter, transferError;
        // The library as the user left it for one of its sub-pages (editor, categories,
        // blacklist, add wigs), restored when that sub-page returns. A visit from
        // anywhere else starts clean.
        std::array<char, 256> librarySearch{};
        std::string libraryCategoryName;
        int libraryCategory = -1;
        bool libraryStashed = false, libraryWigs = false;
        // The outfit last sent to the editor: kept selected, and revealed once on
        // return and once more when the refreshed list arrives after a save.
        std::uint32_t libraryFocusId = 0;
        bool libraryReveal = false, libraryRevealOnRefresh = false;
        // Toast lifetime: the message being shown, how long it has been up, and its tone.
        std::string toastShown;
        float toastAge = 0;
        bool messageDanger = false;
        std::map<std::string, int> wigRowCategories;
        int selectedColor = -1, colorDrag = 0;
        int category = -1, situation = 0, pickerSituation = 0;
        std::uint32_t editId = 0, previewId = 0, deleteId = 0;
        // Create/Edit's Sex choice: -1 Unisex, 0 Male, 1 Female.
        int outfitSex = -1;
        // Sex filters on Manage Outfits and Export; empty lists every sex.
        std::optional<int> librarySex, exportSex;
        std::set<std::uint32_t> categoryIds, exportIds;
        Model items = Model::array();
        // Create/Edit: the armor piece a row click put on the NPC without adding it; null when none.
        Model createPreview;
        bool outfitPreview = false, wigPreview = false, hairEditor = false;
        bool exportSucceeded = false;
        bool defaultHairPending = false, transferPending = false, transferLoading = false;
        bool confirmRequested = false, promptRequested = false;
        bool controllerRotating = false;
        // Retained across NewFrame: ImGui may already have consumed Back and
        // cleared the editor/popup before the screen receives this frame.
        bool controllerEditing = false, controllerTextEditing = false, controllerPopupOpen = false;
        Page controllerPage = Page::Main;
        bool controllerWigs = false;
        bool controllerFocusInitialized = false;
        ImGuiID controllerReturnWindow = 0, controllerReturnItem = 0;
        ImVec4 controllerReturnRect{};
        std::string confirmTitle, confirmMessage, pendingAction;
        Model pendingPayload = Model::object();
        float colorHue = 0, colorSaturation = 0;
        float rgb[3] = {0.5f, 0.5f, 0.5f};
        float yaw = 0;
    };
    FrameResult DrawTailor(const Model& model, ScreenState& state, const Fonts& fonts, bool interactive = true);
}
