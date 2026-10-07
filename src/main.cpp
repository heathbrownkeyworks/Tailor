#include "pch.h"

#include "Settings.h"
#include "PreferenceStore.h"
#include "TailorAPI.h"
#include "api/ModApi.h"
#include "api/PapyrusApi.h"
#include "compat/OBodyCompat.h"
#include "compat/SmoothCamCompat.h"
#include "outfit/OutfitAssignments.h"
#include "outfit/OutfitManager.h"
#include "outfit/OutfitLibrary.h"
#include "outfit/OutfitStore.h"
#include "player/PlayerCoSave.h"
#include "preview/TailorPreviewSession.h"
#include "wig/CustomColorLibrary.h"
#include "wig/WigManager.h"
#include "wig/WigAssignments.h"
#include "wig/WigLibrary.h"
#include "events/CellHandler.h"
#include "events/PowerHandler.h"
#include "events/SituationHandler.h"
#include "ui/TailorUI.h"
#include "ui/imgui/ImGuiHost.h"
#include "ui/imgui/InputDispatchHook.h"
#include "ui/imgui/PapyrusInputGuard.h"
#include "keyhandler/keyhandler.h"


namespace
{
    std::atomic<std::uint64_t> sGameLoadGeneration{1};
}

static void OnDataLoaded()
{
    auto& settings = Settings::GetSingleton();
    settings.Load();
    PreferenceStore::GetSingleton().Load();

    // Outfit systems
    OutfitManager::GetSingleton().Initialize();
    OutfitStore::GetSingleton().Load();
    OutfitLibrary::GetSingleton().Load();
    // The mod API may change the library from now on.
    Tailor::Api::MarkLibraryLoaded();
    OutfitAssignments::GetSingleton().Load();
    OutfitManager::GetSingleton().CaptureDefaultOutfitsAtDataLoad();

    // Wig systems
    WigManager::GetSingleton().Initialize();
    WigLibrary::GetSingleton().Load();
    WigAssignments::GetSingleton().Load();
    WigAssignments::GetSingleton().LoadSituations();
    WigAssignments::GetSingleton().InferAssignedWigs();
    CustomColorLibrary::GetSingleton().Load();

    CellHandler::Register();
    Tailor::Preview::TailorPreviewSession::GetSingleton().RegisterEvents();
    PowerHandler::Register();
    SituationHandler::GetSingleton()->Initialize();
    SituationHandler::Register();
    TailorUI::GetSingleton().Initialize();

    // Register configurable hotkey via KeyHandler
    KeyHandler::RegisterSink();
    auto* kh = KeyHandler::GetSingleton();

    uint32_t modKey = settings.GetModifierKey();
    uint32_t actKey = settings.GetActivateKey();

    // Poll modifier key state instead of tracking press/release events.
    // Event-based tracking is unreliable: the console, other mods, or Windows
    // can consume the modifier UP event, leaving the flag permanently stuck.
    // GetAsyncKeyState checks the physical key state directly.
    static bool useModifier = (modKey != 0);
    static uint32_t modifierVK = 0;

    if (useModifier) {
        modifierVK = MapVirtualKeyA(modKey, MAPVK_VSC_TO_VK);
        if (modifierVK == 0) {
            logger::warn("Settings: Could not map modifier scan code 0x{:02X} to VK, modifier disabled", modKey);
            useModifier = false;
        } else {
            logger::info("Settings: Modifier scan code 0x{:02X} mapped to VK 0x{:02X}", modKey, modifierVK);
        }
    }

    Tailor::ImGuiUI::ConfigureToggleHotkey(actKey, []() {
        // Another plugin (menu manager, hotkey framework) can turn this off at
        // runtime via the exported SetTailorHotkeyEnabled and drive the UI with
        // OpenTailor/CloseTailor instead.
        if (!TailorAPI::IsHotkeyEnabled() || Tailor::ImGuiUI::ImGuiHost::GetSingleton().WantsTextInput()) {
            return false;
        }
        if (!TailorUI::GetSingleton().IsOpen() && !Tailor::ImGuiUI::ImGuiHost::GetSingleton().CanOpen()) return false;
        return !useModifier || (GetAsyncKeyState(modifierVK) & 0x8000) != 0;
    });
    (void)kh->Register(actKey, KeyEventType::KEY_DOWN, []() {
        if (Tailor::ImGuiUI::IsToggleHotkeyActive()) {
            logger::info("Tailor activation: keyboard toggle");
            SKSE::GetTaskInterface()->AddTask([]() { TailorUI::GetSingleton().Toggle(); });
        }
    });

    logger::info("Tailor systems initialized");
}

static void OnPostLoadGame()
{
    const auto generation = sGameLoadGeneration.load();
    SKSE::GetTaskInterface()->AddTask([generation]() {
        if (generation != sGameLoadGeneration.load()) return;
        WigManager::GetSingleton().SetRecoveryEnabled(true);
        PowerHandler::GrantTailorPower();
        OutfitManager::GetSingleton().ReApplyAllAssignments();  // outfits first
        WigManager::GetSingleton().ReEquipAllAssignments();     // then wigs (order matters)
        OutfitManager::GetSingleton().ReconcilePlayerAfterLoad();  // then the player
        WigManager::GetSingleton().ReconcilePlayerWig();           // and the player's wig
        SituationHandler::GetSingleton()->StartSleepMonitoring();
    });
}

static void OnPreLoadGame()
{
    sGameLoadGeneration.fetch_add(1);
    WigManager::GetSingleton().SetRecoveryEnabled(false);
    TailorUI::GetSingleton().CloseForLifecycle(Tailor::Preview::EndReason::PreLoadGame);
    WigManager::GetSingleton().ClearHeadwearForGameLoad();
    OBodyCompat::GetSingleton().OnPreLoadGame();
    CellHandler::InvalidatePendingOutfitTasks();
    SituationHandler::GetSingleton()->ResetForGameLoad();
    OutfitManager::GetSingleton().PrepareForGameLoad();
}

static void SKSEMessageHandler(SKSE::MessagingInterface::Message* message)
{
    switch (message->type) {
    case SKSE::MessagingInterface::kPostLoad:
        SmoothCamCompat::GetSingleton().RegisterInterfaceListener();
        break;
    case SKSE::MessagingInterface::kPostPostLoad:
        OBodyCompat::GetSingleton().RequestAPI();
        SmoothCamCompat::GetSingleton().RequestAPI();
        // After every plugin has loaded, so this guard runs ahead of hooks they
        // placed on the same call.
        (void)Tailor::ImGuiUI::InstallInputDispatchHook();
        break;
    case SKSE::MessagingInterface::kDataLoaded:
        // A failure while Tailor loads is logged and never thrown into the game. OnDataLoaded's body is left as it is:
        // its order is pinned, and LifecycleRegressionTests slices it at four spaces.
        try {
            OnDataLoaded();
        } catch (const std::exception& e) {
            logger::critical("Tailor: loading stopped early, so Tailor is only partly loaded this session: {}", e.what());
        } catch (...) {
            logger::critical("Tailor: loading stopped early on an unknown error, so Tailor is only partly loaded this session");
        }
        // The VM and SKSE's natives exist now, and no script is running yet.
        (void)Tailor::ImGuiUI::InstallPapyrusInputGuard();
        break;
    case SKSE::MessagingInterface::kPreLoadGame:
        OnPreLoadGame();
        break;
    case SKSE::MessagingInterface::kPostLoadGame:
        OnPostLoadGame();
        break;
    case SKSE::MessagingInterface::kNewGame: {
        const auto generation = sGameLoadGeneration.fetch_add(1) + 1;
        WigManager::GetSingleton().SetRecoveryEnabled(false);
        TailorUI::GetSingleton().CloseForLifecycle(Tailor::Preview::EndReason::NewGame);
        WigManager::GetSingleton().ClearHeadwearForGameLoad();
        CellHandler::InvalidatePendingOutfitTasks();
        SituationHandler::GetSingleton()->ResetForGameLoad();
        OutfitManager::GetSingleton().PrepareForGameLoad();
        // Run after this message dispatch so OBody can finish its own new-game
        // transition first. OBody may remain ready and emit no new callback.
        SKSE::GetTaskInterface()->AddTask([generation]() {
            if (generation != sGameLoadGeneration.load()) return;
            WigManager::GetSingleton().SetRecoveryEnabled(true);
            OBodyCompat::GetSingleton().OnNewGame();
            PowerHandler::GrantTailorPower();
            OutfitManager::GetSingleton().ReApplyAllAssignments();
            WigManager::GetSingleton().ReEquipAllAssignments();
            OutfitManager::GetSingleton().ReconcilePlayerAfterLoad();
            WigManager::GetSingleton().ReconcilePlayerWig();
            SituationHandler::GetSingleton()->StartSleepMonitoring();
        });
        break;
    }
    }
}

extern "C" DLLEXPORT bool SKSEAPI SKSEPlugin_Load(const SKSE::LoadInterface* a_skse)
{
    REL::Module::reset();

    auto* g_messaging = reinterpret_cast<SKSE::MessagingInterface*>(
        a_skse->QueryInterface(SKSE::LoadInterface::kMessaging)
    );

    if (!g_messaging) {
        logger::critical("Failed to load messaging interface! Plugin will not load.");
        return false;
    }

    logger::info("{} v{}"sv, Plugin::NAME, Plugin::VERSION.string());

    SKSE::Init(a_skse);
    Tailor::Player::RegisterCoSave();
    Tailor::Api::RegisterPapyrus();
    // One allocation for both 5-byte call hooks (SmoothCam camera requests, input
    // dispatch). A second AllocTrampoline could release the first hook's stub.
    SKSE::AllocTrampoline(28);
    Tailor::Preview::TailorPreviewSession::InstallHooks();
    OBodyCompat::GetSingleton().DetectInstalled(a_skse);
    SmoothCamCompat::GetSingleton().DetectInstalled(a_skse);

    g_messaging->RegisterListener("SKSE", SKSEMessageHandler);

    return true;
}
