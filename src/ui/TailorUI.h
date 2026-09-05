#pragma once

#include "MeridianUIAPI/ViewAPI.h"
#include "outfit/OutfitManager.h"
#include "preview/TailorPreviewSession.h"
#include "ui/GameMenuVisibility.h"

#include <atomic>
#include <filesystem>
#include <set>

class TailorUI
{
public:
    static TailorUI& GetSingleton();

    void Initialize();
    void Toggle();
    void Open();   // idempotent — no-op if already open
    void Close();  // idempotent — no-op if already closed
    void CloseForLifecycle(Tailor::Preview::EndReason reason);
    bool IsOpen() const;
    bool HasFocus() const;
    void SendPreviewState();

    // --- Outfit C++ → JS ---
    void SendTargetUpdate();
    void SendCategories();
    void SendCycleState();
    void SendOutfits();
    void SendCategoryOutfits(int categoryId);
    void SendArmorPlugins();
    void SendArmorForPlugin(const std::string& plugin);
    void SendOutfitData(int outfitId);
    void SendAllCategories();
    void SendSituationData();
    void SendOutfitUsage(int outfitId);
    void RestoreCorrectOutfit(RE::Actor* actor);

    // --- Wig C++ → JS ---
    void SendWigTargetUpdate();
    void SendWigCategories();
    void SendWigCycleState();
    void SendModWigs();
    void SendWigBlacklistData();
    void SendHairColorState();
    void SendWigSituationData();

    // Outfit blacklist
    void LoadBlacklist();
    void SaveBlacklist() const;
    void BlacklistPlugin(const std::string& pluginName);
    void UnblacklistPlugin(const std::string& pluginName);
    void ClearBlacklist();
    bool IsBlacklisted(const std::string& pluginName) const;
    void SendBlacklistData();

    // Wig blacklist
    void LoadWigBlacklist();
    void SaveWigBlacklist() const;
    void WigBlacklistPlugin(const std::string& pluginName);
    void WigUnblacklistPlugin(const std::string& pluginName);
    void ClearWigBlacklist();
    bool IsWigBlacklisted(const std::string& pluginName) const;

private:
    TailorUI() = default;

    std::filesystem::path GetBlacklistPath() const;
    std::filesystem::path GetWigBlacklistPath() const;

    Meridian::UI::View::ViewHandle _view = Meridian::UI::View::INVALID_VIEW_HANDLE;
    // Atomic: the exported IsTailorOpen() lets other plugins read this from
    // their own thread, while writes happen on the game thread.
    std::atomic<bool> _isOpen{false};
    // Close-animation bookkeeping: bumped on every Open() so a deferred Hide()
    // scheduled by an earlier Close() can detect a reopen and cancel itself.
    std::atomic<std::uint32_t> _hideGeneration{0};
    std::atomic<std::uint64_t> _previewOpenGeneration{0};
    Tailor::GameMenuVisibility _gameMenus;
    std::set<std::string> _blacklist;     // outfit blacklist
    std::set<std::string> _wigBlacklist;  // wig blacklist
};
