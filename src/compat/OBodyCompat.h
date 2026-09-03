#pragma once

#include "compat/OBodyAPI.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace SKSE
{
    class LoadInterface;
}

class OBodyCompat
{
public:
    static OBodyCompat& GetSingleton();

    void DetectInstalled(const SKSE::LoadInterface* loadInterface);
    void RequestAPI();

    void OnOBodyReady();
    void OnOBodyNoLongerReady();
    void OnPreLoadGame();
    void OnNewGame();
    void OnActorPresetEvent(
        RE::Actor* actor,
        std::string_view presetName,
        OBody::API::IPluginInterfaceVersionIndependent* responsibleInterface,
        bool unassigned);

    bool IsInstalled() const { return _installed.load(); }
    bool HasAPI() const { return _api != nullptr; }
    bool IsReady() const { return _apiReady.load(); }

    // Automatic outfit changes must not race OBody's load/initialization path.
    // Returns false while the caller should retain and retry the outfit change.
    // This method never asks OBody to generate or randomly assign a preset.
    bool PrepareActorForAutomaticOutfitChange(RE::Actor* actor);

    // Returns true when OBody is installed and Tailor should not send any morph
    // refresh. OBody observes the armor equip events and owns its morph state.
    bool TryRefreshMorphs(RE::Actor* actor);

private:
    OBodyCompat() = default;

    std::atomic_bool _installed{false};
    std::atomic_bool _apiRequested{false};
    std::atomic_bool _apiReady{false};
    std::atomic_bool _passiveModeLogged{false};

    OBody::API::IPluginInterface* _api = nullptr;

    std::mutex _stateMutex;
    std::chrono::steady_clock::time_point _readyAt{};
    std::unordered_map<RE::FormID, std::string> _capturedPresets;
    std::unordered_map<RE::FormID, std::chrono::steady_clock::time_point> _legacyWaitStarted;
    std::unordered_set<RE::FormID> _preparedActors;
    std::unordered_set<RE::FormID> _intentionallyUnassignedActors;
    std::unordered_set<RE::FormID> _waitingLogged;
};
