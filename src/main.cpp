#include "pch.h"

#include "MeridianUIAPI/ViewDllLoader.h"
#include "Settings.h"
#include "TailorAPI.h"
#include "compat/OBodyCompat.h"
#include "compat/SmoothCamCompat.h"
#include "outfit/OutfitAssignments.h"
#include "outfit/OutfitManager.h"
#include "outfit/OutfitLibrary.h"
#include "outfit/OutfitStore.h"
#include "preview/TailorPreviewSession.h"
#include "wig/CustomColorLibrary.h"
#include "wig/WigManager.h"
#include "wig/WigAssignments.h"
#include "wig/WigLibrary.h"
#include "events/CellHandler.h"
#include "events/PowerHandler.h"
#include "events/SituationHandler.h"
#include "ui/TailorUI.h"
#include "keyhandler/keyhandler.h"

Meridian::UI::View::IViewAPI* g_MeridianView = nullptr;

namespace
{
    std::atomic<std::uint64_t> sGameLoadGeneration{1};
}

static void OnInputLoaded()
{
    if (g_MeridianView) {
        return;
    }

    Meridian::UI::Settings meridianSettings{};
    g_MeridianView = Meridian::UI::View::Query(&meridianSettings, "Tailor");

    if (g_MeridianView) {
        logger::info("Tailor: Meridian.View/1 acquired during kInputLoaded");
    } else {
        logger::error("Tailor: Meridian.View/1 unavailable — browser UI disabled; core systems continue.");
    }
}

static void OnDataLoaded()
{
    auto& settings = Settings::GetSingleton();
    settings.Load();

    // Outfit systems
    OutfitManager::GetSingleton().Initialize();
    OutfitStore::GetSingleton().Load();
    OutfitLibrary::GetSingleton().Load();
    OutfitAssignments::GetSingleton().Load();

    // Wig systems
    WigManager::GetSingleton().Initialize();
    WigLibrary::GetSingleton().Load();
    WigAssignments::GetSingleton().Load();
    WigAssignments::GetSingleton().LoadSituations();
    CustomColorLibrary::GetSingleton().Load();

    CellHandler::Register();
    PowerHandler::Register();
    SituationHandler::GetSingleton()->Initialize();
    SituationHandler::Register();
    if (g_MeridianView) {
        TailorUI::GetSingleton().Initialize();
    } else {
        logger::warn("Tailor: skipping browser UI initialization (no Meridian)");
    }

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

    (void)kh->Register(actKey, KeyEventType::KEY_DOWN, []() {
        // Another plugin (menu manager, hotkey framework) can turn this off at
        // runtime via the exported SetTailorHotkeyEnabled and drive the UI with
        // OpenTailor/CloseTailor instead.
        if (!TailorAPI::IsHotkeyEnabled()) {
            return;
        }
        if (!useModifier || (GetAsyncKeyState(modifierVK) & 0x8000)) {
            TailorUI::GetSingleton().Toggle();
        }
    });

    logger::info("Tailor systems initialized");
}

static void OnPostLoadGame()
{
    const auto generation = sGameLoadGeneration.load();
    SKSE::GetTaskInterface()->AddTask([generation]() {
        if (generation != sGameLoadGeneration.load()) return;
        PowerHandler::GrantTailorPower();
        OutfitManager::GetSingleton().ReApplyAllAssignments();  // outfits first
        WigManager::GetSingleton().ReEquipAllAssignments();     // then wigs (order matters)
    });
}

static void OnPreLoadGame()
{
    sGameLoadGeneration.fetch_add(1);
    TailorUI::GetSingleton().CloseForLifecycle(Tailor::Preview::EndReason::PreLoadGame);
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
        break;
    case SKSE::MessagingInterface::kInputLoaded:
        OnInputLoaded();
        break;
    case SKSE::MessagingInterface::kDataLoaded:
        OnDataLoaded();
        break;
    case SKSE::MessagingInterface::kPreLoadGame:
        OnPreLoadGame();
        break;
    case SKSE::MessagingInterface::kPostLoadGame:
        OnPostLoadGame();
        break;
    case SKSE::MessagingInterface::kNewGame: {
        const auto generation = sGameLoadGeneration.fetch_add(1) + 1;
        TailorUI::GetSingleton().CloseForLifecycle(Tailor::Preview::EndReason::NewGame);
        CellHandler::InvalidatePendingOutfitTasks();
        SituationHandler::GetSingleton()->ResetForGameLoad();
        OutfitManager::GetSingleton().PrepareForGameLoad();
        // Run after this message dispatch so OBody can finish its own new-game
        // transition first. OBody may remain ready and emit no new callback.
        SKSE::GetTaskInterface()->AddTask([generation]() {
            if (generation != sGameLoadGeneration.load()) return;
            OBodyCompat::GetSingleton().OnNewGame();
            PowerHandler::GrantTailorPower();
            OutfitManager::GetSingleton().ReApplyAllAssignments();
            WigManager::GetSingleton().ReEquipAllAssignments();
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
    Tailor::Preview::TailorPreviewSession::InstallHooks();
    OBodyCompat::GetSingleton().DetectInstalled(a_skse);
    SmoothCamCompat::GetSingleton().DetectInstalled(a_skse);

    g_messaging->RegisterListener("SKSE", SKSEMessageHandler);

    return true;
}
