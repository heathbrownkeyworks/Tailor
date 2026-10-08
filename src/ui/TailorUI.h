#pragma once

#include "outfit/OutfitManager.h"
#include "preview/TailorPreviewSession.h"
#include <functional>
#include <unordered_map>

#include <atomic>
#include <filesystem>
#include <optional>
#include <set>

class TailorUI
{
public:
    static TailorUI& GetSingleton();

    void Initialize();
    void OnNativeMenuShown();
    void Dispatch(std::string name, std::string data, std::uint64_t openGeneration);
    std::uint64_t OpenGeneration() const { return _previewOpenGeneration.load(); }
    void Toggle();
    void Open();   // idempotent — no-op if already open
    void Close();  // idempotent — no-op if already closed
    void CloseForLifecycle(Tailor::Preview::EndReason reason);
    // Between the NPC the session opened on and the player, from the footer button.
    void SwitchTarget();
    bool IsOpen() const;
    bool HasFocus() const;
    void SendPreviewState();
    void ShowEquipmentWarning(RE::Actor* actor, std::string_view message);
    // A red toast: a change to outfits or categories that was refused, or couldn't be saved.
    void ShowLibraryProblem(std::string_view message);
    // The HUD message when an error closes Tailor: an action that threw (QueueOpenAction) or a frame whose drawing threw
    // (ImGuiHost::DrawFrame).
    static constexpr const char* kClosedAfterError = "Tailor closed after an error. See Tailor.log.";

    // --- Outfit C++ → JS ---
    void SendTargetUpdate();
    void SendCategories();
    void SendCycleState();
    void SendOutfits();
	void SendDiscoveredOutfits();
    void SendCategoryOutfits(int categoryId);
    void SendArmorPlugins();
    void SendArmorForPlugin(const std::string& plugin);
    void SendOutfitData(int outfitId);
    void SendAllCategories();
    void SendSituationData();
    void SendOutfitUsage(int outfitId);
    void SendTransferData();

    // --- Wig C++ → JS ---
    void SendWigTargetUpdate();
    void SendWigCategories();
    void SendWigCycleState();
    void SendDefaultHairResult(bool success);
    void SendModWigs();
    void SendWigBlacklistData();
    void SendHairColorState();
    void SendWigSituationData();

    void SendSettings();

    // Whether outfits and categories can be changed this session, for the screen (tailorSetLibraryState).
    void SendLibraryState();

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
    void RegisterAction(std::string name, std::function<void(const char*)> callback);
    // Ends cycling, Create/Edit and wig browsing on the current target, as Close does.
    void CancelTargetWork(Tailor::Preview::EndReason reason);
    void Publish(std::string topic, nlohmann::json data = nullptr);
    std::unordered_map<std::string, std::function<void(const char*)>> _actions;
    bool _initialized = false;

    std::filesystem::path GetBlacklistPath() const;
    std::filesystem::path GetWigBlacklistPath() const;

    // Atomic: the exported IsTailorOpen() lets other plugins read this from
    // their own thread, while writes happen on the game thread.
    std::atomic<bool> _isOpen{false};
    std::atomic<std::uint64_t> _previewOpenGeneration{0};
    std::optional<float> _originalTimeScale;
    std::set<std::string> _blacklist;     // outfit blacklist
    std::set<std::string> _wigBlacklist;  // wig blacklist
    // A blacklist file Tailor couldn't fully read is never written over; edits still work for the session.
    bool _blacklistSaveAllowed = false;
    bool _wigBlacklistSaveAllowed = false;
};
