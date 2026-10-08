#include "ui/TailorUI.h"
#include "ui/imgui/ImGuiHost.h"
#include "Settings.h"

#include "events/PowerHandler.h"
#include "events/SituationHandler.h"
#include "PreferenceStore.h"
#include "outfit/Children.h"
#include "outfit/OutfitAssignments.h"
#include "outfit/OutfitLibrary.h"
#include "outfit/OutfitStore.h"
#include "outfit/DiscoveredOutfits.h"
#include "outfit/OutfitTransfer.h"
#include "persistence/JsonFile.h"
#include "player/PlayerTarget.h"
#include "player/PlayerWardrobe.h"
#include "preview/TailorPreviewSession.h"
#include "wig/CustomColorLibrary.h"
#include "wig/WigAssignments.h"
#include "wig/WigCategory.h"
#include "wig/WigLibrary.h"
#include "wig/WigManager.h"

#include <algorithm>
#include <limits>
#include <stdexcept>


namespace
{
    // Native Dispatch and lifecycle events share the game task queue. Recheck at
    // the final mutation too: a close/load can run after Dispatch but before a
    // callback's queued operation is drained.
    // An action that throws would end the game, and one that stops part-way can leave the screen waiting for an answer
    // (an import or export, Default Hair) with Back blocked. So Tailor logs it, closes, and says so on the HUD.
    void QueueOpenAction(std::function<void()> action)
    {
        const auto generation = TailorUI::GetSingleton().OpenGeneration();
        SKSE::GetTaskInterface()->AddTask([generation, action = std::move(action)]() {
            auto& ui = TailorUI::GetSingleton();
            try {
                if (ui.IsOpen() && generation && generation == ui.OpenGeneration()) action();
            } catch (const std::exception& e) {
                logger::error("TailorUI: an action failed, so Tailor is closing: {}", e.what());
                ui.CloseForLifecycle(Tailor::Preview::EndReason::FocusLost);
                RE::SendHUDMessage::ShowHUDMessage(TailorUI::kClosedAfterError);
            } catch (...) {
                logger::error("TailorUI: an action failed on an unknown error, so Tailor is closing");
                ui.CloseForLifecycle(Tailor::Preview::EndReason::FocusLost);
                RE::SendHUDMessage::ShowHUDMessage(TailorUI::kClosedAfterError);
            }
        });
    }

    // Outfits and categories change only while both their files can be saved. A file Tailor couldn't fully read at
    // startup has its saves off for the session, and a change made then would be lost at the
    // next start, or leave the two files disagreeing. Each handler asks once, first, before it changes anything.
    bool LibraryWritable()
    {
        return OutfitStore::GetSingleton().SaveAllowed() && OutfitLibrary::GetSingleton().SaveAllowed();
    }

    constexpr std::string_view kLibraryReadOnly =
        "Outfits and categories can't be changed: Tailor couldn't fully read outfits.json or library.json. See Tailor.log.";
    constexpr std::string_view kAssignmentsReadOnly =
        "Outfits can't be deleted: Tailor couldn't fully read assignments.json. See Tailor.log.";
    // The store refused to make it (no id is left), and its log says why.
    constexpr std::string_view kOutfitNotCreated = "Tailor couldn't create the outfit. See Tailor.log.";
    constexpr std::string_view kCategoryNotCreated = "Tailor couldn't create the category. See Tailor.log.";

    // A save that failed. The change holds until the game closes, unless the handler takes it back.
    std::string SaveFailed(std::string_view file)
    {
        return std::format("Tailor couldn't save {}. See Tailor.log.", file);
    }

    // A refused editor save. The screen has already left the editor, so the preview ends here, or the NPC would
    // stay in the pieces they were trying on.
    void RefuseEditorSave()
    {
        TailorUI::GetSingleton().ShowLibraryProblem(kLibraryReadOnly);
        OutfitManager::GetSingleton().EndCreateOutfit();
    }

    // One wig for wiggyAddWig. It is added when it is a wig the game has loaded, in a category the library has.
    // Anything else (a missing field, an unknown category, or a plugin name that matches no loaded plugin, which is
    // what a name with bytes that weren't valid UTF-8 becomes on its way from the screen) is skipped and logged, and
    // returns false. A wig already in the library returns true: AddWig leaves it where it is.
    bool AddWigRow(const nlohmann::json& row)
    {
        if (!row.is_object() || !row.contains("category") || !row.at("category").is_number_integer() ||
            !row.contains("formId") || !row.at("formId").is_number_integer() ||
            !row.contains("plugin") || !row.at("plugin").is_string() || !row.contains("name") || !row.at("name").is_string()) {
            logger::warn("wiggyAddWig: a wig is missing its category, formId, plugin or name; skipped");
            return false;
        }
        const auto category = row.at("category").get<std::int64_t>();
        if (category < 0 || category >= static_cast<std::int64_t>(kCategoryCount)) {
            logger::warn("wiggyAddWig: invalid category {}; skipped", category);
            return false;
        }
        WigEntry entry;
        entry.formId = row.at("formId").get<RE::FormID>();
        entry.plugin = row.at("plugin").get<std::string>();
        entry.name = row.at("name").get<std::string>();
        if (!entry.Resolve()) {
            logger::warn("wiggyAddWig: {:06X} in '{}' is not an armor the game has loaded; skipped", entry.formId, entry.plugin);
            return false;
        }
        WigLibrary::GetSingleton().AddWig(static_cast<WigCategory>(category), entry);
        return true;
    }

    std::vector<int> ReadOutfitCategoryIds(const nlohmann::json& data, int outfitId = 0)
    {
        auto& library = OutfitLibrary::GetSingleton();
        std::vector<int> ids;
        if (data.contains("categoryIds")) {
            const auto& selected = data.at("categoryIds");
            if (!selected.is_array()) throw std::invalid_argument("categoryIds must be an array");
            for (const auto& id : selected) {
                if (!id.is_number_integer() || id <= 0 || id > (std::numeric_limits<int>::max)()) {
                    throw std::invalid_argument("Invalid outfit category ID");
                }
                ids.push_back(id.get<int>());
            }
        } else {
            // Older views send only one category. Preserve memberships they cannot display.
            for (const auto& cat : library.GetCategories()) {
                if (std::find(cat.outfitIds.begin(), cat.outfitIds.end(), outfitId) != cat.outfitIds.end()) {
                    ids.push_back(cat.id);
                }
            }
            const int categoryId = data.value("categoryId", 0);
            if (categoryId > 0) ids.push_back(categoryId);
        }
        if (ids.empty()) throw std::invalid_argument("Select at least one outfit category");
        for (int id : ids) {
            if (!library.GetCategoryById(id)) throw std::invalid_argument("Outfit category no longer exists");
        }
        return ids;
    }

    // The sex a save or edit asks for. A request without one keeps `current`;
    // anything but -1, 0 or 1 is refused.
    OutfitSex ReadRequestedSex(const nlohmann::json& data, OutfitSex current)
    {
        if (!data.contains("sex")) return current;
        const auto sex = ReadOutfitSex(data.at("sex"));
        if (!sex) throw std::invalid_argument("Invalid outfit sex");
        return *sex;
    }

    int CalcOutfitArmorRating(const CustomOutfit& outfit)
    {
        int total = 0;
        for (auto& item : outfit.items) {
            if (auto* armor = item.Resolve()) {
                total += armor->armorRating;
            }
        }
        return total / 100;  // armorRating is CK value * 100
    }

    nlohmann::json GetArmorEnchantData(RE::TESObjectARMO* armor)
    {
        nlohmann::json result;
        result["armorRating"] = armor ? static_cast<int>(armor->armorRating / 100) : 0;
        result["enchanted"] = false;
        result["enchantments"] = nlohmann::json::array();

        if (!armor || !armor->formEnchanting) return result;

        result["enchanted"] = true;

        auto* enchantment = armor->formEnchanting;

        for (auto* effect : enchantment->effects) {
            if (!effect || !effect->baseEffect) continue;
            std::string effectName = SanitizeUtf8(effect->baseEffect->GetFullName());
            float mag = effect->effectItem.magnitude;
            if (mag > 0.0f) {
                result["enchantments"].push_back(
                    std::format("{} +{}", effectName, static_cast<int>(mag)));
            } else {
                result["enchantments"].push_back(effectName);
            }
        }

        return result;
    }

    std::string GetArmorSlotName(RE::TESObjectARMO* armor)
    {
        if (!armor) return "Unknown";
        using Slot = RE::BGSBipedObjectForm::BipedObjectSlot;
        if (armor->HasPartOf(Slot::kBody))     return "Body";
        if (armor->HasPartOf(Slot::kHead))     return "Head";
        if (armor->HasPartOf(Slot::kHair))     return "Hair";
        if (armor->HasPartOf(Slot::kHands))    return "Hands";
        if (armor->HasPartOf(Slot::kForearms)) return "Forearms";
        if (armor->HasPartOf(Slot::kFeet))     return "Feet";
        if (armor->HasPartOf(Slot::kCalves))   return "Calves";
        if (armor->HasPartOf(Slot::kShield))   return "Shield";
        if (armor->HasPartOf(Slot::kAmulet))   return "Amulet";
        if (armor->HasPartOf(Slot::kRing))     return "Ring";
        if (armor->HasPartOf(Slot::kCirclet))  return "Circlet";
        if (armor->HasPartOf(Slot::kEars))     return "Ears";
        if (armor->HasPartOf(Slot::kTail))     return "Tail";
        if (armor->HasPartOf(Slot::kLongHair)) return "Long Hair";
        return "Other";
    }

    void RestorePlayerRunMode()
    {
        auto* pc = RE::PlayerControls::GetSingleton();
        if (!pc) {
            logger::warn("TailorUI: PlayerControls unavailable; could not restore run mode");
            return;
        }

        // Restore the player's *preferred* run/walk mode (not a hardcoded run):
        // bAlwaysRunByDefault. Falls back to run if the setting can't be read.
        bool runByDefault = true;
        if (auto* setting = RE::GetINISetting("bAlwaysRunByDefault:Controls")) {
            runByDefault = setting->GetBool();
        }

        // The focus menu tears down asynchronously over the next few frames
        // through the UI message queue, and the engine re-derives data.running
        // so a synchronous write here gets clobbered (player ends up walking).
        // A single AddTask won't help: from inside our task-dispatched close path
        // it drains the same frame. Defer past teardown with short real-time
        // delays that bracket it; last writer wins. (Verified in-game 2026-06-03.)
        std::thread([runByDefault]() {
            auto apply = [runByDefault]() {
                SKSE::GetTaskInterface()->AddTask([runByDefault]() {
                    if (auto* pc = RE::PlayerControls::GetSingleton()) {
                        pc->data.running = runByDefault;
                    }
                });
            };
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            apply();
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            apply();
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
            apply();
        }).detach();

        logger::info("TailorUI: scheduled player run-mode restore (target={})", runByDefault);
    }

}

TailorUI& TailorUI::GetSingleton()
{
    static TailorUI singleton;
    return singleton;
}

void TailorUI::RegisterAction(std::string name, std::function<void(const char*)> callback)
{
    _actions.emplace(std::move(name), std::move(callback));
}

void TailorUI::Publish(std::string topic, nlohmann::json data)
{
    Tailor::ImGuiUI::ImGuiHost::GetSingleton().Publish(std::move(topic), std::move(data));
}

void TailorUI::Dispatch(std::string name, std::string data, std::uint64_t openGeneration)
{
    SKSE::GetTaskInterface()->AddTask([this, name = std::move(name), data = std::move(data), openGeneration]() {
        if (!IsOpen() || !openGeneration || openGeneration != OpenGeneration()) {
            logger::warn("TailorUI: dropped '{}' for a closed or reopened menu", name);
            return;
        }
        if (const auto action = _actions.find(name); action != _actions.end()) {
            action->second(data.c_str());
        } else {
            logger::warn("TailorUI: no handler for '{}'", name);
        }
    });
}

void TailorUI::Initialize()
{
    if (_initialized) return;
    // ================================================================
    // OUTFIT native actions (33)
    // ================================================================

    // 1. tailorSelectCategory — start cycling in a category
    RegisterAction("tailorSelectCategory", [](const char* arg) {
        QueueOpenAction([d = std::string(arg)]() {
            try {
                auto json = nlohmann::json::parse(d);
                int id = json.value("id", 0);
                const int situation = json.value("situation", 0);
                if (situation < 0 || situation > kLastOutfitSituation) return;
                auto& ui = TailorUI::GetSingleton();
                if (OutfitManager::GetSingleton().StartCycle(id, static_cast<OutfitSituation>(situation))) {
                    ui.SendCycleState();
                } else {
                    ui.SendSituationData();
                    ui.Publish("tailorCycleUnavailable");
                }
            } catch (const nlohmann::json::exception& e) {
                logger::error("tailorSelectCategory: {}", e.what());
            }
        });
    });

    // 2. tailorCycleNext
    RegisterAction("tailorCycleNext", [](const char*) {
        QueueOpenAction([]() {
            OutfitManager::GetSingleton().CycleNext();
            TailorUI::GetSingleton().SendCycleState();
        });
    });

    // 3. tailorCyclePrev
    RegisterAction("tailorCyclePrev", [](const char*) {
        QueueOpenAction([]() {
            OutfitManager::GetSingleton().CyclePrev();
            TailorUI::GetSingleton().SendCycleState();
        });
    });

    // 3b. tailorCycleToIndex — jump to a specific index in the cycle list
    RegisterAction("tailorCycleToIndex", [](const char* arg) {
        QueueOpenAction([d = std::string(arg)]() {
            try {
                auto json = nlohmann::json::parse(d);
                int index = json.value("index", -1);
                OutfitManager::GetSingleton().CycleToIndex(index);
                TailorUI::GetSingleton().SendCycleState();
            } catch (const nlohmann::json::exception& e) {
                logger::error("tailorCycleToIndex: {}", e.what());
            }
        });
    });

    // 4. tailorConfirmCycle
    RegisterAction("tailorConfirmCycle", [](const char*) {
        QueueOpenAction([]() {
            OutfitManager::GetSingleton().ConfirmCycle();
            TailorUI::GetSingleton().SendTargetUpdate();
        });
    });

    // 5. tailorCancelCycle
    RegisterAction("tailorCancelCycle", [](const char*) {
        QueueOpenAction([]() {
            OutfitManager::GetSingleton().CancelCycle();
        });
    });

    // 6. tailorResetOutfit
    RegisterAction("tailorResetOutfit", [](const char*) {
        QueueOpenAction([]() {
            auto& mgr = OutfitManager::GetSingleton();
            auto* target = mgr.GetTarget();
            if (target) {
                mgr.ResetOutfit(target);
                TailorUI::GetSingleton().SendTargetUpdate();
            }
        });
    });

    RegisterAction("tailorRequestTransferData", [](const char*) {
        QueueOpenAction([]() {
            auto& ui = TailorUI::GetSingleton();
            if (ui.IsOpen()) ui.SendTransferData();
        });
    });
    RegisterAction("tailorExportOutfits", [](const char* arg) {
        QueueOpenAction([payload = std::string(arg)]() {
            auto& ui = TailorUI::GetSingleton();
            if (!ui.IsOpen()) return;
            nlohmann::json result;
            try {
                const auto json = nlohmann::json::parse(payload);
                std::vector<int> ids;
                if (!json.at("outfitIds").is_array()) throw std::invalid_argument("Invalid outfit selection.");
                for (const auto& id : json.at("outfitIds")) {
                    if (!id.is_number_integer() || id <= 0 || id > (std::numeric_limits<int>::max)()) throw std::invalid_argument("Invalid outfit selection.");
                    ids.push_back(id.get<int>());
                }
                result = OutfitTransfer::Export(json.at("name").get<std::string>(), ids);
            } catch (const std::exception& error) {
                logger::warn("Outfit export failed: {}", error.what());
                result = {{"ok", false}, {"operation", "export"}, {"error", error.what()}};
            }
            ui.Publish("tailorTransferResult", result);
            ui.SendTransferData();
        });
    });
    RegisterAction("tailorImportOutfits", [](const char* arg) {
        QueueOpenAction([payload = std::string(arg)]() {
            auto& ui = TailorUI::GetSingleton();
            if (!ui.IsOpen()) return;
            nlohmann::json result;
            try {
                // Refused like every change to outfits or categories, through the result the screen waits for.
                if (!LibraryWritable()) throw std::runtime_error(std::string(kLibraryReadOnly));
                const auto json = nlohmann::json::parse(payload);
                result = OutfitTransfer::Import(json.at("file").get<std::string>());
                ui.SendOutfits();
                ui.SendCategories();
                ui.SendSituationData();
            } catch (const std::exception& error) {
                logger::warn("Outfit import failed: {}", error.what());
                result = {{"ok", false}, {"operation", "import"}, {"error", error.what()}};
            }
            ui.Publish("tailorTransferResult", result);
            ui.SendTransferData();
        });
    });

    // 7. tailorRequestOutfits — send all custom outfits
    RegisterAction("tailorRequestOutfits", [](const char*) {
        QueueOpenAction([]() {
            TailorUI::GetSingleton().SendOutfits();
        });
    });
	
	// Discovered outfits (Fitting Room auto-discovery port) — send the
    // read-only discovered sets, re-run the pipeline, preview one on the
    // target NPC through the same temporary session as tailorPreviewOutfit.
    RegisterAction("tailorRequestDiscovered", [](const char*) {
        QueueOpenAction([]() {
            TailorUI::GetSingleton().SendDiscoveredOutfits();
        });
    });

    RegisterAction("tailorRescanDiscovered", [](const char*) {
        QueueOpenAction([]() {
            Tailor::Discovered::DiscoveredOutfits::GetSingleton().Regenerate();
            TailorUI::GetSingleton().SendDiscoveredOutfits();
        });
    });

    RegisterAction("tailorPreviewDiscovered", [](const char* arg) {
        QueueOpenAction([d = std::string(arg)]() {
            if (!TailorUI::GetSingleton().IsOpen()) return;
            try {
                const auto json = nlohmann::json::parse(d);
                const int outfitId = json.value("outfitId", 0);
                auto outfit = Tailor::Discovered::DiscoveredOutfits::GetSingleton().GetById(outfitId);
                auto& mgr = OutfitManager::GetSingleton();
                auto* target = mgr.GetTarget();
                // The screen refuses these with its toast; never dress the target in one anyway.
                if (outfit && target && !OutfitFits(outfit->sex, OutfitManager::GetNpcSex(target))) {
                    logger::info("tailorPreviewDiscovered: '{}' does not fit {}", outfit->name, target->GetDisplayFullName());
                    return;
                }
                if (outfit && target) {
                    mgr.LoadCreateOutfitItems(target, outfit->items);
                }
            } catch (const nlohmann::json::exception& e) {
                logger::error("tailorPreviewDiscovered: {}", e.what());
            }
        });
    });

    // 8. tailorAddOutfitToCategory
    RegisterAction("tailorAddOutfitToCategory", [](const char* arg) {
        QueueOpenAction([d = std::string(arg)]() {
            if (!LibraryWritable()) {
                TailorUI::GetSingleton().ShowLibraryProblem(kLibraryReadOnly);
                return;
            }
            try {
                auto json = nlohmann::json::parse(d);
                int categoryId = json.value("categoryId", 0);
                int outfitId = json.value("outfitId", 0);

                if (categoryId > 0 && outfitId > 0) {
                    auto& lib = OutfitLibrary::GetSingleton();
                    lib.AddOutfitToCategory(categoryId, outfitId);
                    if (!lib.Save()) TailorUI::GetSingleton().ShowLibraryProblem(SaveFailed("library.json"));
                    TailorUI::GetSingleton().SendCategoryOutfits(categoryId);
                    TailorUI::GetSingleton().SendCategories();
                }
            } catch (const nlohmann::json::exception& e) {
                logger::error("tailorAddOutfitToCategory: {}", e.what());
            }
        });
    });

    // 9. tailorRemoveOutfitFromCategory
    RegisterAction("tailorRemoveOutfitFromCategory", [](const char* arg) {
        QueueOpenAction([d = std::string(arg)]() {
            if (!LibraryWritable()) {
                TailorUI::GetSingleton().ShowLibraryProblem(kLibraryReadOnly);
                return;
            }
            try {
                auto json = nlohmann::json::parse(d);
                int categoryId = json.value("categoryId", 0);
                int outfitId = json.value("outfitId", 0);

                if (categoryId > 0 && outfitId > 0) {
                    auto& lib = OutfitLibrary::GetSingleton();
                    lib.RemoveOutfitFromCategory(categoryId, outfitId);
                    if (!lib.Save()) TailorUI::GetSingleton().ShowLibraryProblem(SaveFailed("library.json"));
                    TailorUI::GetSingleton().SendCategoryOutfits(categoryId);
                    TailorUI::GetSingleton().SendCategories();
                }
            } catch (const nlohmann::json::exception& e) {
                logger::error("tailorRemoveOutfitFromCategory: {}", e.what());
            }
        });
    });

    // 10. tailorRequestCategoryOutfits
    RegisterAction("tailorRequestCategoryOutfits", [](const char* arg) {
        QueueOpenAction([d = std::string(arg)]() {
            try {
                auto json = nlohmann::json::parse(d);
                int id = json.value("id", 0);
                if (id > 0) {
                    TailorUI::GetSingleton().SendCategoryOutfits(id);
                }
            } catch (const nlohmann::json::exception& e) {
                logger::error("tailorRequestCategoryOutfits: {}", e.what());
            }
        });
    });

    // 11. tailorDeleteOutfit
    RegisterAction("tailorDeleteOutfit", [](const char* arg) {
        QueueOpenAction([d = std::string(arg)]() {
            if (!LibraryWritable()) {
                TailorUI::GetSingleton().ShowLibraryProblem(kLibraryReadOnly);
                return;
            }
            // Deleting also releases everyone assigned the outfit, which changes assignments.json.
            if (!OutfitAssignments::GetSingleton().SaveAllowed()) {
                TailorUI::GetSingleton().ShowLibraryProblem(kAssignmentsReadOnly);
                return;
            }
            try {
                auto json = nlohmann::json::parse(d);
                int outfitId = json.value("outfitId", 0);

                if (outfitId > 0) {
                    auto& store = OutfitStore::GetSingleton();
                    auto& lib = OutfitLibrary::GetSingleton();

                    lib.RemoveOutfitFromAllCategories(outfitId);
                    const bool librarySaved = lib.Save();
                    store.DeleteOutfit(outfitId);
                    const bool outfitsSaved = store.Save();
                    // The outfit is gone for this session either way; the player is told a file couldn't be saved.
                    if (!librarySaved || !outfitsSaved) {
                        TailorUI::GetSingleton().ShowLibraryProblem(SaveFailed(!outfitsSaved && !librarySaved ? "outfits.json and library.json" :
                            outfitsSaved ? "library.json" : "outfits.json"));
                    }

                    // After the store forgets it, so nobody is redressed in it again.
                    OutfitManager::GetSingleton().ReleaseDeletedOutfit(outfitId);

                    TailorUI::GetSingleton().SendOutfits();
                    TailorUI::GetSingleton().SendCategories();
                    TailorUI::GetSingleton().SendTargetUpdate();
                    TailorUI::GetSingleton().SendSituationData();
                }
            } catch (const nlohmann::json::exception& e) {
                logger::error("tailorDeleteOutfit: {}", e.what());
            }
        });
    });

    // 11b. tailorCheckOutfitUsage
    RegisterAction("tailorCheckOutfitUsage", [](const char* arg) {
        QueueOpenAction([d = std::string(arg)]() {
            try {
                auto json = nlohmann::json::parse(d);
                int outfitId = json.value("outfitId", 0);
                if (outfitId > 0) {
                    TailorUI::GetSingleton().SendOutfitUsage(outfitId);
                }
            } catch (const nlohmann::json::exception& e) {
                logger::error("tailorCheckOutfitUsage: {}", e.what());
            }
        });
    });

    // 12. tailorRequestArmorPlugins
    RegisterAction("tailorRequestArmorPlugins", [](const char*) {
        QueueOpenAction([]() {
            TailorUI::GetSingleton().SendArmorPlugins();
        });
    });

    // 13. tailorRequestArmorForPlugin
    RegisterAction("tailorRequestArmorForPlugin", [](const char* arg) {
        QueueOpenAction([d = std::string(arg)]() {
            try {
                auto json = nlohmann::json::parse(d);
                auto plugin = json.value("plugin", std::string{});
                if (!plugin.empty()) {
                    TailorUI::GetSingleton().SendArmorForPlugin(plugin);
                }
            } catch (const nlohmann::json::exception& e) {
                logger::error("tailorRequestArmorForPlugin: {}", e.what());
            }
        });
    });

    // 14. tailorEquipArmorItem
    RegisterAction("tailorEquipArmorItem", [](const char* arg) {
        QueueOpenAction([d = std::string(arg)]() {
            try {
                auto json = nlohmann::json::parse(d);
                ArmorItem item;
                item.formId = json.value("formId", static_cast<RE::FormID>(0));
                item.plugin = json.value("plugin", std::string{});
                item.name = json.value("name", std::string{});

                auto& mgr = OutfitManager::GetSingleton();
                auto* target = mgr.GetTarget();
                if (target && item.formId != 0) {
                    mgr.AddItemToCreateOutfit(target, item);
                }
            } catch (const nlohmann::json::exception& e) {
                logger::error("tailorEquipArmorItem: {}", e.what());
            }
        });
    });

    // 15. tailorUnequipArmorItem
    RegisterAction("tailorUnequipArmorItem", [](const char* arg) {
        QueueOpenAction([d = std::string(arg)]() {
            try {
                auto json = nlohmann::json::parse(d);
                ArmorItem item;
                item.formId = json.value("formId", static_cast<RE::FormID>(0));
                item.plugin = json.value("plugin", std::string{});
                item.name = json.value("name", std::string{});

                auto& mgr = OutfitManager::GetSingleton();
                auto* target = mgr.GetTarget();
                if (target && item.formId != 0) {
                    mgr.RemoveItemFromCreateOutfit(target, item);
                }
            } catch (const nlohmann::json::exception& e) {
                logger::error("tailorUnequipArmorItem: {}", e.what());
            }
        });
    });

    // 16. tailorBeginCreateOutfit
    RegisterAction("tailorBeginCreateOutfit", [](const char*) {
        QueueOpenAction([]() {
            auto& mgr = OutfitManager::GetSingleton();
            auto* target = mgr.GetTarget();
            if (target) {
                mgr.BeginCreateOutfit(target);
            }
        });
    });

    // 17. tailorSaveOutfit
    RegisterAction("tailorSaveOutfit", [](const char* arg) {
        QueueOpenAction([d = std::string(arg)]() {
            if (!LibraryWritable()) {
                RefuseEditorSave();
                return;
            }
            try {
                auto json = nlohmann::json::parse(d);
                auto name = json.value("name", std::string{});
                const auto categoryIds = ReadOutfitCategoryIds(json);
                const auto sex = ReadRequestedSex(json, OutfitSex::Unisex);

                std::vector<ArmorItem> items;
                if (json.contains("items") && json["items"].is_array()) {
                    for (auto& ij : json["items"]) {
                        ArmorItem item;
                        item.formId = ij.value("formId", static_cast<RE::FormID>(0));
                        item.plugin = ij.value("plugin", std::string{});
                        item.name = ij.value("name", std::string{});
                        items.push_back(std::move(item));
                    }
                }

                if (!name.empty() && !items.empty()) {
                    auto& store = OutfitStore::GetSingleton();
                    // A taken name is refused with a message; nothing is saved or changed.
                    if (store.NameTaken(name)) {
                        TailorUI::GetSingleton().Publish("toast", {{"message", std::format("An outfit named '{}' already exists", name)}, {"kind", "danger"}});
                        return;
                    }
                    int outfitId = store.AddOutfit(name, items, sex);
                    // Refused by the store (no id is left): say so, and end the editor the screen has already left.
                    if (outfitId == 0) {
                        TailorUI::GetSingleton().ShowLibraryProblem(kOutfitNotCreated);
                        OutfitManager::GetSingleton().EndCreateOutfit();
                        return;
                    }
                    // An outfit that couldn't be saved is taken back, so its id never reaches an assignment or the
                    // co-save. The editor still closes: the screen has already left it.
                    if (!store.Save()) {
                        store.DiscardNewOutfit(outfitId);
                        TailorUI::GetSingleton().ShowLibraryProblem(SaveFailed("outfits.json"));
                        OutfitManager::GetSingleton().EndCreateOutfit();
                        TailorUI::GetSingleton().SendOutfits();
                        return;
                    }

                    auto& lib = OutfitLibrary::GetSingleton();
                    lib.SetOutfitCategories(outfitId, categoryIds);
                    // The outfit itself is saved; categories that couldn't be saved hold for this session.
                    if (!lib.Save()) TailorUI::GetSingleton().ShowLibraryProblem(SaveFailed("library.json"));

                    OutfitManager::GetSingleton().EndCreateOutfit();

                    TailorUI::GetSingleton().SendOutfits();
                    TailorUI::GetSingleton().SendCategories();
                    logger::info("Saved custom outfit '{}' (id={}) with {} items",
                        name, outfitId, items.size());
                }
            } catch (const std::exception& e) {
                logger::error("tailorSaveOutfit: {}", e.what());
            }
        });
    });

    // 18. tailorCancelCreateOutfit
    RegisterAction("tailorCancelCreateOutfit", [](const char*) {
        QueueOpenAction([]() {
            OutfitManager::GetSingleton().EndCreateOutfit();
        });
    });

    // 19. tailorClose
    RegisterAction("tailorClose", [](const char*) {
        QueueOpenAction([]() {
            TailorUI::GetSingleton().Close();
        });
    });

    // 20. tailorRequestOutfitData
    RegisterAction("tailorRequestOutfitData", [](const char* arg) {
        QueueOpenAction([d = std::string(arg)]() {
            try {
                auto json = nlohmann::json::parse(d);
                int outfitId = json.value("outfitId", 0);
                if (outfitId > 0) {
                    TailorUI::GetSingleton().SendOutfitData(outfitId);

                    auto* outfit = OutfitStore::GetSingleton().GetOutfitById(outfitId);
                    auto& mgr = OutfitManager::GetSingleton();
                    auto* target = mgr.GetTarget();
                    if (outfit && target) {
                        mgr.LoadCreateOutfitItems(target, outfit->items);
                    }
                }
            } catch (const nlohmann::json::exception& e) {
                logger::error("tailorRequestOutfitData: {}", e.what());
            }
        });
    });

    // Manage Outfits row preview uses the same temporary session and cleanup as
    // the editor, without populating editor fields or saving an assignment.
    RegisterAction("tailorPreviewOutfit", [](const char* arg) {
        QueueOpenAction([d = std::string(arg)]() {
            if (!TailorUI::GetSingleton().IsOpen()) return;
            try {
                const auto json = nlohmann::json::parse(d);
                const int outfitId = json.value("outfitId", 0);
                auto* outfit = OutfitStore::GetSingleton().GetOutfitById(outfitId);
                auto& mgr = OutfitManager::GetSingleton();
                auto* target = mgr.GetTarget();
                // The screen refuses these with its toast; never dress the target in one anyway.
                if (outfit && target && !OutfitFits(outfit->sex, OutfitManager::GetNpcSex(target))) {
                    logger::info("tailorPreviewOutfit: '{}' does not fit {}", outfit->name, target->GetDisplayFullName());
                    return;
                }
                if (outfit && target) {
                    mgr.LoadCreateOutfitItems(target, outfit->items);
                }
            } catch (const nlohmann::json::exception& e) {
                logger::error("tailorPreviewOutfit: {}", e.what());
            }
        });
    });
	
	// Save a discovered set as a regular outfit. Name collisions are
    // resolved automatically: "Abyss" -> "Abyss (2)" -> "Abyss (3)" ...
    RegisterAction("tailorSaveDiscovered", [](const char* arg) {
        QueueOpenAction([d = std::string(arg)]() {
            if (!TailorUI::GetSingleton().IsOpen()) return;
            try {
                const auto json = nlohmann::json::parse(d);
                const int outfitId = json.value("outfitId", 0);
                auto discovered = Tailor::Discovered::DiscoveredOutfits::GetSingleton().GetById(outfitId);
                if (!discovered || discovered->items.empty()) return;
                auto& store = OutfitStore::GetSingleton();
                if (!store.SaveAllowed()) {
                    TailorUI::GetSingleton().Publish("toast", {{"message", "Outfits can't be saved right now (outfits.json didn't load cleanly)"}, {"kind", "danger"}});
                    return;
                }
                std::string name = discovered->name;
                if (store.NameTaken(name)) {
                    int n = 2;
                    while (store.NameTaken(name + " (" + std::to_string(n) + ")")) ++n;
                    name += " (" + std::to_string(n) + ")";
                }
                const int newId = store.AddOutfit(name, discovered->items, discovered->sex);
                if (newId == 0) {
                    TailorUI::GetSingleton().ShowLibraryProblem(kOutfitNotCreated);
                    return;
                }
                if (!store.Save()) {
                    store.DiscardNewOutfit(newId);
                    TailorUI::GetSingleton().ShowLibraryProblem(SaveFailed("outfits.json"));
                    TailorUI::GetSingleton().SendOutfits();
                    return;
                }
                TailorUI::GetSingleton().SendOutfits();
                TailorUI::GetSingleton().Publish("toast", std::string("Saved '") + name + "' as a new outfit");
                logger::info("tailorSaveDiscovered: saved '{}' (id={}) with {} items",
                    name, newId, discovered->items.size());
            } catch (const std::exception& e) {
                logger::error("tailorSaveDiscovered: {}", e.what());
            }
        });
    });

    // 21. tailorUpdateOutfit
    RegisterAction("tailorUpdateOutfit", [](const char* arg) {
        QueueOpenAction([d = std::string(arg)]() {
            if (!LibraryWritable()) {
                RefuseEditorSave();
                return;
            }
            try {
                auto json = nlohmann::json::parse(d);
                int outfitId = json.value("outfitId", 0);
                auto name = json.value("name", std::string{});
                const auto categoryIds = ReadOutfitCategoryIds(json, outfitId);
                // A request without a sex keeps the one the outfit has.
                const auto* existing = OutfitStore::GetSingleton().GetOutfitById(outfitId);
                const auto previousSex = existing ? existing->sex : OutfitSex::Unisex;
                const auto sex = ReadRequestedSex(json, previousSex);

                std::vector<ArmorItem> items;
                if (json.contains("items") && json["items"].is_array()) {
                    for (auto& ij : json["items"]) {
                        ArmorItem item;
                        item.formId = ij.value("formId", static_cast<RE::FormID>(0));
                        item.plugin = ij.value("plugin", std::string{});
                        item.name = ij.value("name", std::string{});
                        items.push_back(std::move(item));
                    }
                }

                if (outfitId > 0 && !name.empty() && !items.empty()) {
                    auto& store = OutfitStore::GetSingleton();
                    if (store.NameTaken(name, outfitId)) {
                        TailorUI::GetSingleton().Publish("toast", {{"message", std::format("An outfit named '{}' already exists", name)}, {"kind", "danger"}});
                        return;
                    }
                    if (!store.UpdateOutfit(outfitId, name, items, sex)) return;
                    const bool outfitsSaved = store.Save();

                    auto& lib = OutfitLibrary::GetSingleton();
                    lib.SetOutfitCategories(outfitId, categoryIds);
                    const bool librarySaved = lib.Save();
                    // An edit that couldn't be saved holds for this session; nothing is taken back.
                    if (!outfitsSaved || !librarySaved) {
                        TailorUI::GetSingleton().ShowLibraryProblem(SaveFailed(!outfitsSaved && !librarySaved ? "outfits.json and library.json" :
                            outfitsSaved ? "library.json" : "outfits.json"));
                    }

                    // Before the editor closes: it redresses its own NPC, and the rest are redressed here.
                    if (sex != previousSex) OutfitManager::GetSingleton().RefitOutfits({outfitId});
                    OutfitManager::GetSingleton().EndCreateOutfit();
                    // The player may be wearing the outfit just edited, from any session.
                    if (Tailor::Player::PlayerWardrobe::GetSingleton().WornOutfitId() == outfitId) OutfitManager::GetSingleton().RedressPlayer();

                    TailorUI::GetSingleton().SendOutfits();
                    TailorUI::GetSingleton().SendCategories();
                    TailorUI::GetSingleton().SendSituationData();
                    TailorUI::GetSingleton().SendTargetUpdate();
                    logger::info("Updated outfit '{}' (id={}) with {} items", name, outfitId, items.size());
                }
            } catch (const std::exception& e) {
                logger::error("tailorUpdateOutfit: {}", e.what());
            }
        });
    });

    // 22. tailorRequestAllCategories
    RegisterAction("tailorRequestAllCategories", [](const char*) {
        QueueOpenAction([]() {
            TailorUI::GetSingleton().SendAllCategories();
        });
    });

    // 23. tailorAddCategory
    RegisterAction("tailorAddCategory", [](const char* arg) {
        QueueOpenAction([d = std::string(arg)]() {
            if (!LibraryWritable()) {
                TailorUI::GetSingleton().ShowLibraryProblem(kLibraryReadOnly);
                return;
            }
            try {
                auto json = nlohmann::json::parse(d);
                auto name = json.value("name", std::string{});

                if (name.empty()) return;

                auto& lib = OutfitLibrary::GetSingleton();
                if (lib.CategoryNameTaken(name)) {
                    TailorUI::GetSingleton().Publish("toast", {{"message", std::format("A category named '{}' already exists", name)}, {"kind", "danger"}});
                    return;
                }
                const int categoryId = lib.AddCategory(name);
                // Refused by the library (no id is left): say so, since the screen has already cleared the name.
                if (categoryId == 0) {
                    TailorUI::GetSingleton().ShowLibraryProblem(kCategoryNotCreated);
                    return;
                }
                // A new category that couldn't be saved is taken back, so its id is never in use unsaved.
                if (!lib.Save()) {
                    lib.DeleteCategory(categoryId);
                    TailorUI::GetSingleton().ShowLibraryProblem(SaveFailed("library.json"));
                }
                TailorUI::GetSingleton().SendAllCategories();
                TailorUI::GetSingleton().SendCategories();
            } catch (const nlohmann::json::exception& e) {
                logger::error("tailorAddCategory: {}", e.what());
            }
        });
    });

    // 24. tailorRenameCategory
    RegisterAction("tailorRenameCategory", [](const char* arg) {
        QueueOpenAction([d = std::string(arg)]() {
            if (!LibraryWritable()) {
                TailorUI::GetSingleton().ShowLibraryProblem(kLibraryReadOnly);
                return;
            }
            try {
                auto json = nlohmann::json::parse(d);
                int categoryId = json.value("categoryId", 0);
                auto name = json.value("name", std::string{});

                if (categoryId > 0 && !name.empty()) {
                    auto& lib = OutfitLibrary::GetSingleton();
                    if (lib.CategoryNameTaken(name, categoryId)) {
                        TailorUI::GetSingleton().Publish("toast", {{"message", std::format("A category named '{}' already exists", name)}, {"kind", "danger"}});
                        return;
                    }
                    if (!lib.RenameCategory(categoryId, name)) return;
                    if (!lib.Save()) TailorUI::GetSingleton().ShowLibraryProblem(SaveFailed("library.json"));
                    TailorUI::GetSingleton().SendAllCategories();
                    TailorUI::GetSingleton().SendCategories();
                }
            } catch (const nlohmann::json::exception& e) {
                logger::error("tailorRenameCategory: {}", e.what());
            }
        });
    });

    // 25. tailorDeleteCategory
    RegisterAction("tailorDeleteCategory", [](const char* arg) {
        QueueOpenAction([d = std::string(arg)]() {
            if (!LibraryWritable()) {
                TailorUI::GetSingleton().ShowLibraryProblem(kLibraryReadOnly);
                return;
            }
            try {
                auto json = nlohmann::json::parse(d);
                int categoryId = json.value("categoryId", 0);

                if (categoryId > 0) {
                    auto& lib = OutfitLibrary::GetSingleton();
                    lib.DeleteCategory(categoryId);
                    if (!lib.Save()) TailorUI::GetSingleton().ShowLibraryProblem(SaveFailed("library.json"));
                    TailorUI::GetSingleton().SendAllCategories();
                    TailorUI::GetSingleton().SendCategories();
                }
            } catch (const nlohmann::json::exception& e) {
                logger::error("tailorDeleteCategory: {}", e.what());
            }
        });
    });

    // 26. tailorRequestBlacklist
    RegisterAction("tailorRequestBlacklist", [](const char*) {
        QueueOpenAction([]() {
            TailorUI::GetSingleton().SendBlacklistData();
        });
    });

    // 27. tailorBlacklistPlugin
    RegisterAction("tailorBlacklistPlugin", [](const char* arg) {
        QueueOpenAction([d = std::string(arg)]() {
            try {
                auto json = nlohmann::json::parse(d);
                auto name = json.value("plugin", std::string{});
                if (!name.empty()) {
                    // Only a plugin the game has loaded: a name with bytes that weren't valid UTF-8 arrives with U+FFFD in
                    // their place and would match nothing. LookupModByName also finds a plugin that is in Data but not
                    // active, whose compile index is 0xFF.
                    auto* data = RE::TESDataHandler::GetSingleton();
                    const auto* file = data ? data->LookupModByName(name) : nullptr;
                    if (!file || file->compileIndex == 0xFF) {
                        logger::warn("tailorBlacklistPlugin: '{}' is not a plugin the game has loaded; not blacklisted", name);
                        return;
                    }
                    auto& ui = TailorUI::GetSingleton();
                    ui.BlacklistPlugin(name);
                    ui.SendBlacklistData();
                }
            } catch (const nlohmann::json::exception& e) {
                logger::error("tailorBlacklistPlugin: {}", e.what());
            }
        });
    });

    // 28. tailorUnblacklistPlugin
    RegisterAction("tailorUnblacklistPlugin", [](const char* arg) {
        QueueOpenAction([d = std::string(arg)]() {
            try {
                auto json = nlohmann::json::parse(d);
                auto name = json.value("plugin", std::string{});
                if (!name.empty()) {
                    auto& ui = TailorUI::GetSingleton();
                    ui.UnblacklistPlugin(name);
                    ui.SendBlacklistData();
                }
            } catch (const nlohmann::json::exception& e) {
                logger::error("tailorUnblacklistPlugin: {}", e.what());
            }
        });
    });

    // 29. tailorClearBlacklist
    RegisterAction("tailorClearBlacklist", [](const char*) {
        QueueOpenAction([]() {
            auto& ui = TailorUI::GetSingleton();
            ui.ClearBlacklist();
            ui.SendBlacklistData();
        });
    });

    // 30. tailorRequestSituations
    RegisterAction("tailorRequestSituations", [](const char*) {
        QueueOpenAction([]() {
            TailorUI::GetSingleton().SendSituationData();
        });
    });

    RegisterAction("tailorSetAdventuringArmorType", [](const char* arg) {
        QueueOpenAction([d = std::string(arg)]() {
            auto& ui = TailorUI::GetSingleton();
            if (!ui.IsOpen()) return;
            try {
                const auto json = nlohmann::json::parse(d);
                const auto value = json.value("armorType", std::string{});
                const auto type = ParseOutfitArmorType(value);
                if (value != OutfitArmorTypeName(type)) throw std::invalid_argument("Unknown armor type");
                if (auto* target = OutfitManager::GetSingleton().GetTarget()) {
                    auto& assignments = OutfitAssignments::GetSingleton();
                    assignments.SetAdventuringArmorType(target->GetFormID(), type);
                    assignments.Save();
                    if (assignments.HasAnySituation(target->GetFormID())) {
                        SituationHandler::GetSingleton()->ForceApplyForSituation(target);
                    }
                }
            } catch (const std::exception& e) {
                logger::error("tailorSetAdventuringArmorType: {}", e.what());
            }
            ui.SendSituationData();
            ui.SendTargetUpdate();
        });
    });

    // 31. tailorConfirmSituationCycle
    RegisterAction("tailorConfirmSituationCycle", [](const char* arg) {
        QueueOpenAction([d = std::string(arg)]() {
            try {
                auto json = nlohmann::json::parse(d);
                int sit = json.value("situation", 0);
                logger::info("tailorConfirmSituationCycle: situation={}", sit);
                if (sit >= 1 && sit <= kLastOutfitSituation) {
                    auto& mgr = OutfitManager::GetSingleton();
                    bool ok = mgr.ConfirmCycle(static_cast<OutfitSituation>(sit));
                    logger::info("tailorConfirmSituationCycle: ConfirmCycle={}", ok);
                    auto* target = mgr.GetTarget();
                    if (target) {
                        SituationHandler::GetSingleton()->ForceApplyForSituation(target);
                    } else {
                        logger::warn("tailorConfirmSituationCycle: no target after confirm!");
                    }
                    TailorUI::GetSingleton().SendSituationData();
                    TailorUI::GetSingleton().SendTargetUpdate();
                }
            } catch (const nlohmann::json::exception& e) {
                logger::error("tailorConfirmSituationCycle: {}", e.what());
            }
        });
    });

    // 32. tailorClearSituation
    RegisterAction("tailorClearSituation", [](const char* arg) {
        QueueOpenAction([d = std::string(arg)]() {
            try {
                auto json = nlohmann::json::parse(d);
                int sit = json.value("situation", 0);
                auto& mgr = OutfitManager::GetSingleton();
                auto* target = mgr.GetTarget();
                if (target && sit >= 1 && sit <= kLastOutfitSituation) {
                    auto& assignments = OutfitAssignments::GetSingleton();
                    const auto* saved = assignments.GetAssignment(target->GetFormID());
                    if (!saved || !saved->HasOutfits()) return;
                    auto remaining = *saved;
                    remaining.ClearSlot(static_cast<OutfitSituation>(sit));
                    if (remaining.outfitId <= 0 && !remaining.HasAnySituation()) {
                        // Restore before removing the last assignment and its original-outfit metadata.
                        if (!mgr.ResetOutfit(target)) return;
                    } else {
                        assignments.ClearSituation(target->GetFormID(), static_cast<OutfitSituation>(sit));
                        assignments.Save();
                    }
                    SituationHandler::GetSingleton()->ForceApplyForSituation(target);
                    TailorUI::GetSingleton().SendSituationData();
                    TailorUI::GetSingleton().SendTargetUpdate();
                }
            } catch (const nlohmann::json::exception& e) {
                logger::error("tailorClearSituation: {}", e.what());
            }
        });
    });

    // 33. tailorClearAllSituations
    RegisterAction("tailorClearAllSituations", [](const char*) {
        QueueOpenAction([]() {
            auto& mgr = OutfitManager::GetSingleton();
            auto* target = mgr.GetTarget();
            if (target) {
                auto& assignments = OutfitAssignments::GetSingleton();
                if (!assignments.HasAssignment(target->GetFormID())) return;
                if (assignments.GetOutfitId(target->GetFormID()) <= 0) {
                    if (!mgr.ResetOutfit(target)) return;
                } else {
                    assignments.ClearAllSituations(target->GetFormID());
                    assignments.Save();
                }
                SituationHandler::GetSingleton()->ForceApplyForSituation(target);
                TailorUI::GetSingleton().SendSituationData();
                TailorUI::GetSingleton().SendTargetUpdate();
            }
        });
    });

    // 34. tailorToggleSituationRandom
    RegisterAction("tailorToggleSituationRandom", [](const char* arg) {
        QueueOpenAction([d = std::string(arg)]() {
            try {
                auto json = nlohmann::json::parse(d);
                int sit = json.value("situation", 0);
                bool random = json.value("random", false);
                auto& mgr = OutfitManager::GetSingleton();
                auto* target = mgr.GetTarget();
                if (target && sit >= 1 && sit <= kLastOutfitSituation) {
                    auto& assignments = OutfitAssignments::GetSingleton();
                    const auto* saved = assignments.GetAssignment(target->GetFormID());
                    if ((!saved || !saved->HasOutfits()) && !random) return;
                    auto remaining = saved ? *saved : SituationalAssignment{};
                    remaining.SetRandomFlag(static_cast<OutfitSituation>(sit), random);
                    if (remaining.outfitId <= 0 && !remaining.HasAnySituation()) {
                        if (!mgr.ResetOutfit(target)) return;
                    } else {
                        assignments.SetSituationRandom(target->GetFormID(),
                            static_cast<OutfitSituation>(sit), random);
                        assignments.Save();
                    }
                    SituationHandler::GetSingleton()->ForceApplyForSituation(target);
                    TailorUI::GetSingleton().SendSituationData();
                    TailorUI::GetSingleton().SendTargetUpdate();
                }
            } catch (const nlohmann::json::exception& e) {
                logger::error("tailorToggleSituationRandom: {}", e.what());
            }
        });
    });

    // 35. tailorCopyOutfit
    RegisterAction("tailorCopyOutfit", [](const char* arg) {
        QueueOpenAction([d = std::string(arg)]() {
            if (!LibraryWritable()) {
                TailorUI::GetSingleton().ShowLibraryProblem(kLibraryReadOnly);
                return;
            }
            try {
                auto json = nlohmann::json::parse(d);
                int  outfitId = json.value("outfitId", 0);
                auto name = json.value("name", std::string{});

                if (outfitId <= 0 || name.empty()) {
                    return;
                }

                auto& store = OutfitStore::GetSingleton();
                const auto* source = store.GetOutfitById(outfitId);
                if (!source) {
                    logger::warn("tailorCopyOutfit: source outfit {} not found", outfitId);
                    return;
                }
                if (store.NameTaken(name)) {
                    TailorUI::GetSingleton().Publish("toast", {{"message", std::format("An outfit named '{}' already exists", name)}, {"kind", "danger"}});
                    return;
                }

                // Copy out before AddOutfit — it push_backs into the same vector `source`
                // points into, so the pointer can dangle the moment it reallocates.
                auto sourceName = source->name;
                auto items = source->items;
                const auto sex = source->sex;

                int newId = store.AddOutfit(name, items, sex);
                // Refused by the store (no id is left): say so.
                if (newId == 0) {
                    TailorUI::GetSingleton().ShowLibraryProblem(kOutfitNotCreated);
                    return;
                }
                // A copy that couldn't be saved is taken back, so its id never reaches an assignment or the co-save.
                if (!store.Save()) {
                    store.DiscardNewOutfit(newId);
                    TailorUI::GetSingleton().ShowLibraryProblem(SaveFailed("outfits.json"));
                    TailorUI::GetSingleton().SendOutfits();
                    return;
                }

                // Mirror the source's category membership so the copy lands beside it.
                auto& lib = OutfitLibrary::GetSingleton();
                std::vector<int> targetCategories;
                for (const auto& cat : lib.GetCategories()) {
                    for (int id : cat.outfitIds) {
                        if (id == outfitId) {
                            targetCategories.push_back(cat.id);
                            break;
                        }
                    }
                }
                for (int catId : targetCategories) {
                    lib.AddOutfitToCategory(catId, newId);
                }
                // The copy itself is saved; categories that couldn't be saved hold for this session.
                if (!targetCategories.empty() && !lib.Save()) {
                    TailorUI::GetSingleton().ShowLibraryProblem(SaveFailed("library.json"));
                }

                TailorUI::GetSingleton().SendOutfits();
                TailorUI::GetSingleton().SendCategories();
                logger::info("Copied outfit '{}' (id={}) to '{}' (id={}) with {} items",
                    sourceName, outfitId, name, newId, items.size());
            } catch (const nlohmann::json::exception& e) {
                logger::error("tailorCopyOutfit: {}", e.what());
            }
        });
    });

    // 36. tailorSetOutfitSex — tag every outfit Manage Outfits shows
    RegisterAction("tailorSetOutfitSex", [](const char* arg) {
        QueueOpenAction([d = std::string(arg)]() {
            if (!LibraryWritable()) {
                TailorUI::GetSingleton().ShowLibraryProblem(kLibraryReadOnly);
                return;
            }
            auto& ui = TailorUI::GetSingleton();
            try {
                const auto json = nlohmann::json::parse(d);
                const auto sex = ReadOutfitSex(json.at("sex"));
                if (!sex || !json.at("outfitIds").is_array()) throw std::invalid_argument("Invalid outfit sex request");
                std::vector<int> ids;
                for (const auto& id : json.at("outfitIds")) {
                    if (!id.is_number_integer() || id <= 0 || id > (std::numeric_limits<int>::max)()) throw std::invalid_argument("Invalid outfit selection");
                    ids.push_back(id.get<int>());
                }
                auto& store = OutfitStore::GetSingleton();
                const auto result = store.SetOutfitSex(ids, *sex);
                bool saved = true;
                // Only an outfit whose sex changed can change who may wear it.
                if (!result.changed.empty()) {
                    saved = store.Save();
                    OutfitManager::GetSingleton().RefitOutfits(result.changed);
                }
                // The success toast only for a change that was saved, or it would overwrite the failure's.
                if (saved) ui.Publish("toast", std::format("Set {} {} to {}", result.tagged, result.tagged == 1 ? "outfit" : "outfits", OutfitSexName(*sex)));
                else ui.ShowLibraryProblem(SaveFailed("outfits.json"));
            } catch (const std::exception& e) {
                logger::error("tailorSetOutfitSex: {}", e.what());
            }
            ui.SendOutfits();
            ui.SendCategories();
            ui.SendSituationData();
            ui.SendTargetUpdate();
        });
    });

    // ================================================================
    // WIG native actions (21)
    // ================================================================

    RegisterAction("wiggySelectCategory", [](const char* arg) {
        try {
            auto json = nlohmann::json::parse(arg);
            int catIdx = json.value("category", -1);
            if (catIdx < 0 || catIdx >= static_cast<int>(kCategoryCount)) {
                logger::warn("wiggySelectCategory: invalid category {}", catIdx);
                return;
            }

            auto category = static_cast<WigCategory>(catIdx);
            QueueOpenAction([category]() {
                auto& mgr = WigManager::GetSingleton();
                auto* target = mgr.GetTarget();
                if (!target) {
                    logger::warn("wiggySelectCategory: no target");
                    return;
                }
                const bool started = mgr.StartCycling(target, category);
                logger::info("wiggySelectCategory: category {} on {:08X} {} with {} wig(s)",
                    static_cast<int>(category), target->GetFormID(), started ? "started" : "did not start", mgr.GetCycleCount());
                TailorUI::GetSingleton().SendWigCycleState();
            });
        } catch (const nlohmann::json::exception& e) {
            logger::error("wiggySelectCategory: JSON parse error: {}", e.what());
        }
    });

    RegisterAction("wiggyCycleNext", [](const char*) {
        QueueOpenAction([]() {
            WigManager::GetSingleton().CycleNext();
            TailorUI::GetSingleton().SendWigCycleState();
        });
    });

    RegisterAction("wiggyCyclePrev", [](const char*) {
        QueueOpenAction([]() {
            WigManager::GetSingleton().CyclePrev();
            TailorUI::GetSingleton().SendWigCycleState();
        });
    });

    RegisterAction("wiggyCycleToIndex", [](const char* arg) {
        QueueOpenAction([d = std::string(arg)]() {
            try {
                auto json = nlohmann::json::parse(d);
                int index = json.value("index", -1);
                WigManager::GetSingleton().CycleToIndex(index);
                TailorUI::GetSingleton().SendWigCycleState();
            } catch (const nlohmann::json::exception& e) {
                logger::error("wiggyCycleToIndex: {}", e.what());
            }
        });
    });

    RegisterAction("wiggyConfirmCycle", [](const char*) {
        QueueOpenAction([]() {
            WigManager::GetSingleton().ConfirmCycle();
            TailorUI::GetSingleton().SendWigTargetUpdate();
        });
    });

    RegisterAction("wiggyCancelCycle", [](const char*) {
        QueueOpenAction([]() {
            WigManager::GetSingleton().CancelCycle();
        });
    });

    RegisterAction("wiggyResetWig", [](const char*) {
        QueueOpenAction([]() {
            auto& mgr = WigManager::GetSingleton();
            auto* target = mgr.GetTarget();
            const bool success = mgr.ResetToDefaultHair(target);
            auto& ui = TailorUI::GetSingleton();
            ui.SendWigTargetUpdate();
            ui.SendWigSituationData();
            ui.SendWigCycleState();
            ui.SendDefaultHairResult(success);
        });
    });

    RegisterAction("wiggyRequestMods", [](const char*) {
        QueueOpenAction([]() {
            TailorUI::GetSingleton().SendModWigs();
        });
    });

    RegisterAction("wiggyAddWig", [](const char* arg) {
        QueueOpenAction([d = std::string(arg)]() {
            try {
                const auto json = nlohmann::json::parse(d);
                // Add All sends every wig in one {"wigs": [...]}, so the library is saved, and the screen told, once; a
                // row's Add sends the wig itself. A wig AddWigRow skips is counted and logged; the rest still go in.
                const bool batch = json.is_object() && json.contains("wigs");
                const auto rows = batch ? json.at("wigs") : nlohmann::json::array({json});
                auto& library = WigLibrary::GetSingleton();
                const auto revision = library.Revision();
                std::size_t skipped = rows.is_array() ? 0 : 1;
                if (rows.is_array()) for (const auto& row : rows) if (!AddWigRow(row)) ++skipped;
                const auto added = library.Revision() - revision;
                if (added) library.Save();
                auto& ui = TailorUI::GetSingleton();
                // The original view's wording: "<name>" added to library, N wigs added to library. A
                // wig already in the library adds nothing and says nothing; a skipped one says so in red.
                const auto count = std::format("{} {} added to library", added, added == 1 ? "wig" : "wigs");
                if (skipped) ui.Publish("toast", {{"message", batch ? std::format("{}; {} couldn't be added. See Tailor.log.", count, skipped) : std::string("Tailor couldn't add that wig. See Tailor.log.")}, {"kind", "danger"}});
                else if (added) ui.Publish("toast", batch ? count : std::format("\"{}\" added to library", json.at("name").get<std::string>()));
                ui.SendWigCategories();
            } catch (const nlohmann::json::exception& e) {
                logger::error("wiggyAddWig: JSON parse error: {}", e.what());
            }
        });
    });

    RegisterAction("wiggyRemoveWig", [](const char* arg) {
        QueueOpenAction([d = std::string(arg)]() {
            try {
                auto json = nlohmann::json::parse(d);
                int catIdx = json.value("category", -1);
                if (catIdx < 0 || catIdx >= static_cast<int>(kCategoryCount)) {
                    logger::warn("wiggyRemoveWig: invalid category {}", catIdx);
                    return;
                }

                WigEntry entry;
                entry.formId = json.value("formId", static_cast<RE::FormID>(0));
                entry.plugin = json.value("plugin", std::string{});
                entry.name = json.value("name", std::string{});

                auto category = static_cast<WigCategory>(catIdx);
                WigLibrary::GetSingleton().RemoveWig(category, entry);
                WigLibrary::GetSingleton().Save();
                TailorUI::GetSingleton().SendWigCategories();
            } catch (const nlohmann::json::exception& e) {
                logger::error("wiggyRemoveWig: JSON parse error: {}", e.what());
            }
        });
    });

    RegisterAction("wiggyMoveWig", [](const char* arg) {
        QueueOpenAction([d = std::string(arg)]() {
            try {
                auto json = nlohmann::json::parse(d);
                int fromIdx = json.value("fromCategory", -1);
                int toIdx = json.value("toCategory", -1);
                if (fromIdx < 0 || fromIdx >= static_cast<int>(kCategoryCount) ||
                    toIdx < 0 || toIdx >= static_cast<int>(kCategoryCount) ||
                    fromIdx == toIdx) {
                    return;
                }

                WigEntry entry;
                entry.formId = json.value("formId", static_cast<RE::FormID>(0));
                entry.plugin = json.value("plugin", std::string{});
                entry.name = json.value("name", std::string{});

                auto& lib = WigLibrary::GetSingleton();
                auto from = static_cast<WigCategory>(fromIdx);
                auto to = static_cast<WigCategory>(toIdx);
                if (lib.RemoveWig(from, entry)) {
                    lib.AddWig(to, entry);
                    lib.Save();
                    logger::info("Moved wig '{}' from {} to {}", entry.name,
                        CategoryToString(from), CategoryToString(to));
                }
                TailorUI::GetSingleton().SendWigCategories();
            } catch (const nlohmann::json::exception& e) {
                logger::error("wiggyMoveWig: JSON parse error: {}", e.what());
            }
        });
    });

    RegisterAction("wiggyPreviewWig", [](const char* arg) {
        QueueOpenAction([d = std::string(arg)]() {
            try {
                auto json = nlohmann::json::parse(d);
                WigEntry entry;
                entry.formId = json.value("formId", static_cast<RE::FormID>(0));
                entry.plugin = json.value("plugin", std::string{});
                entry.name = json.value("name", std::string{});

                auto& mgr = WigManager::GetSingleton();
                auto* target = mgr.GetTarget();
                if (target) {
                    mgr.PreviewWig(target, entry);
                }
            } catch (const nlohmann::json::exception& e) {
                logger::error("wiggyPreviewWig: JSON parse error: {}", e.what());
            }
        });
    });

    RegisterAction("wiggyEndPreview", [](const char*) {
        QueueOpenAction([]() {
            auto& mgr = WigManager::GetSingleton();
            if (mgr.IsPreviewing()) {
                mgr.EndPreview();
            }
        });
    });

    // --- Wig blacklist listeners ---

    RegisterAction("wiggyRequestBlacklist", [](const char*) {
        QueueOpenAction([]() {
            TailorUI::GetSingleton().SendWigBlacklistData();
        });
    });

    RegisterAction("wiggyBlacklistPlugin", [](const char* arg) {
        QueueOpenAction([d = std::string(arg)]() {
            try {
                auto json = nlohmann::json::parse(d);
                auto name = json.value("plugin", std::string{});
                if (!name.empty()) {
                    // Only a plugin the game has loaded: a name with bytes that weren't valid UTF-8 arrives with U+FFFD in
                    // their place and would match nothing. LookupModByName also finds a plugin that is in Data but not
                    // active, whose compile index is 0xFF.
                    auto* data = RE::TESDataHandler::GetSingleton();
                    const auto* file = data ? data->LookupModByName(name) : nullptr;
                    if (!file || file->compileIndex == 0xFF) {
                        logger::warn("wiggyBlacklistPlugin: '{}' is not a plugin the game has loaded; not blacklisted", name);
                        return;
                    }
                    auto& ui = TailorUI::GetSingleton();
                    ui.WigBlacklistPlugin(name);
                    ui.SendWigBlacklistData();
                }
            } catch (const nlohmann::json::exception& e) {
                logger::error("wiggyBlacklistPlugin: JSON parse error: {}", e.what());
            }
        });
    });

    RegisterAction("wiggyUnblacklistPlugin", [](const char* arg) {
        QueueOpenAction([d = std::string(arg)]() {
            try {
                auto json = nlohmann::json::parse(d);
                auto name = json.value("plugin", std::string{});
                if (!name.empty()) {
                    auto& ui = TailorUI::GetSingleton();
                    ui.WigUnblacklistPlugin(name);
                    ui.SendWigBlacklistData();
                }
            } catch (const nlohmann::json::exception& e) {
                logger::error("wiggyUnblacklistPlugin: JSON parse error: {}", e.what());
            }
        });
    });

    RegisterAction("wiggyClearBlacklist", [](const char*) {
        QueueOpenAction([]() {
            auto& ui = TailorUI::GetSingleton();
            ui.ClearWigBlacklist();
            ui.SendWigBlacklistData();
        });
    });

    // --- Hair color listeners ---

    RegisterAction("wiggyOpenHairColor", [](const char*) {
        QueueOpenAction([]() {
            TailorUI::GetSingleton().SendHairColorState();
        });
    });

    RegisterAction("wiggyApplyHairColor", [](const char* arg) {
        QueueOpenAction([d = std::string(arg)]() {
            try {
                auto json = nlohmann::json::parse(d);
                auto r = static_cast<uint8_t>(json.value("r", 0));
                auto g = static_cast<uint8_t>(json.value("g", 0));
                auto b = static_cast<uint8_t>(json.value("b", 0));

                auto& mgr = WigManager::GetSingleton();
                auto* target = mgr.GetTarget();
                if (target) {
                    mgr.ApplyHairColor(target, r, g, b);
                }
            } catch (const nlohmann::json::exception& e) {
                logger::error("wiggyApplyHairColor: JSON parse error: {}", e.what());
            }
        });
    });

    RegisterAction("wiggyConfirmHairColor", [](const char* arg) {
        QueueOpenAction([d = std::string(arg)]() {
            try {
                auto json = nlohmann::json::parse(d);
                auto r = static_cast<int16_t>(json.value("r", 0));
                auto g = static_cast<int16_t>(json.value("g", 0));
                auto b = static_cast<int16_t>(json.value("b", 0));

                auto& mgr = WigManager::GetSingleton();
                auto* target = mgr.GetTarget();
                if (target) {
                    if (!mgr.ConfirmHairColor(target,
                        static_cast<uint8_t>(r),
                        static_cast<uint8_t>(g),
                        static_cast<uint8_t>(b))) {
                        TailorUI::GetSingleton().SendHairColorState();
                        return;
                    }
                    // Heal any neighbour still sharing a hair material bled by an older build.
                    mgr.RetintNearbyActors(target);
                    logger::info("Confirmed hair color ({}, {}, {}) for {}",
                        r, g, b, target->GetDisplayFullName());
                    TailorUI::GetSingleton().SendHairColorState();
                }
            } catch (const nlohmann::json::exception& e) {
                logger::error("wiggyConfirmHairColor: JSON parse error: {}", e.what());
            }
        });
    });

    RegisterAction("wiggyResetHairColor", [](const char*) {
        QueueOpenAction([]() {
            auto& mgr = WigManager::GetSingleton();
            auto* target = mgr.GetTarget();
            if (target) {
                // Clear the assignment FIRST — ResetHairColor retints, and ResolveHairTint
                // would otherwise resolve the very custom color we're undoing.
                WigAssignments::GetSingleton().ClearHairColor(target->GetFormID());
                WigAssignments::GetSingleton().Save();
                mgr.ResetHairColor(target);
                mgr.RetintNearbyActors(target);
                logger::info("Reset hair color for {}", target->GetDisplayFullName());
                TailorUI::GetSingleton().SendHairColorState();
            }
        });
    });

    RegisterAction("wiggyCloseHairColor", [](const char*) {
        // Purely a navigation event; the native screen handles panel switching.
    });

    // --- Custom color library listeners ---

    RegisterAction("wiggyAddCustomColor", [](const char* arg) {
        QueueOpenAction([d = std::string(arg)]() {
            try {
                auto json = nlohmann::json::parse(d);
                auto r = static_cast<uint8_t>(json.value("r", 0));
                auto g = static_cast<uint8_t>(json.value("g", 0));
                auto b = static_cast<uint8_t>(json.value("b", 0));

                auto& lib = CustomColorLibrary::GetSingleton();
                if (lib.AddColor(r, g, b)) {
                    lib.Save();
                    logger::info("CustomColorLibrary: added color ({}, {}, {})", r, g, b);
                }
                // Always refresh the UI (even on duplicate) so the screens stay in sync
                TailorUI::GetSingleton().SendHairColorState();
            } catch (const nlohmann::json::exception& e) {
                logger::error("wiggyAddCustomColor: JSON parse error: {}", e.what());
            }
        });
    });

    RegisterAction("wiggyDeleteCustomColor", [](const char* arg) {
        QueueOpenAction([d = std::string(arg)]() {
            try {
                auto json = nlohmann::json::parse(d);
                auto r = static_cast<uint8_t>(json.value("r", 0));
                auto g = static_cast<uint8_t>(json.value("g", 0));
                auto b = static_cast<uint8_t>(json.value("b", 0));

                auto& lib = CustomColorLibrary::GetSingleton();
                if (lib.RemoveColor(r, g, b)) {
                    lib.Save();
                    logger::info("CustomColorLibrary: removed color ({}, {}, {})", r, g, b);
                }
                TailorUI::GetSingleton().SendHairColorState();
            } catch (const nlohmann::json::exception& e) {
                logger::error("wiggyDeleteCustomColor: JSON parse error: {}", e.what());
            }
        });
    });

    // --- Wig situation listeners ---

    RegisterAction("wiggyRequestWigSituations", [](const char*) {
        QueueOpenAction([]() {
            TailorUI::GetSingleton().SendWigSituationData();
        });
    });

    RegisterAction("wiggyConfirmSituationCycle", [](const char* arg) {
        QueueOpenAction([d = std::string(arg)]() {
            try {
                auto json = nlohmann::json::parse(d);
                int sit = json.value("situation", 0);
                if (sit >= 1 && sit <= 4) {
                    auto& mgr = WigManager::GetSingleton();
                    mgr.ConfirmCycle(static_cast<OutfitSituation>(sit));
                    TailorUI::GetSingleton().SendWigSituationData();
                    TailorUI::GetSingleton().SendWigTargetUpdate();
                }
            } catch (const nlohmann::json::exception& e) {
                logger::error("wiggyConfirmSituationCycle: {}", e.what());
            }
        });
    });

    RegisterAction("wiggyClearWigSituation", [](const char* arg) {
        QueueOpenAction([d = std::string(arg)]() {
            try {
                auto json = nlohmann::json::parse(d);
                int sit = json.value("situation", 0);
                auto& mgr = WigManager::GetSingleton();
                auto* target = mgr.GetTarget();
                if (target && sit >= 1 && sit <= 4) {
                    auto& assignments = WigAssignments::GetSingleton();
                    assignments.ClearSituation(target->GetFormID(), static_cast<OutfitSituation>(sit));
                    assignments.SaveSituations();
                    SituationHandler::GetSingleton()->ForceApplyForSituation(target);
                    TailorUI::GetSingleton().SendWigSituationData();
                }
            } catch (const nlohmann::json::exception& e) {
                logger::error("wiggyClearWigSituation: {}", e.what());
            }
        });
    });

    RegisterAction("wiggyClearAllWigSituations", [](const char*) {
        QueueOpenAction([]() {
            auto& mgr = WigManager::GetSingleton();
            auto* target = mgr.GetTarget();
            if (target) {
                auto& assignments = WigAssignments::GetSingleton();
                assignments.ClearAllSituations(target->GetFormID());
                assignments.SaveSituations();
                SituationHandler::GetSingleton()->ForceApplyForSituation(target);
                TailorUI::GetSingleton().SendWigSituationData();
            }
        });
    });

    // --- Live NPC stage geometry ---

    RegisterAction("tailorPreviewViewport", [](const char* arg) {
        QueueOpenAction([d = std::string(arg)]() {
            try {
                const auto json = nlohmann::json::parse(d);
                auto& ui = TailorUI::GetSingleton();
                if (!ui.IsOpen()) return;

                const auto openGeneration = json.value("openGeneration", std::uint64_t{0});
                if (openGeneration != ui._previewOpenGeneration.load()) return;

                Tailor::Preview::ViewportRect viewport{
                    json.value("x", 0.0f),
                    json.value("y", 0.0f),
                    json.value("width", 0.0f),
                    json.value("height", 0.0f),
                    json.value("hairMode", false)
                };
                auto& preview = Tailor::Preview::TailorPreviewSession::GetSingleton();
                auto* target = OutfitManager::GetSingleton().GetTarget();
                if (!WigManager::GetSingleton().SetWigScreen(viewport.hairMode)) {
                    logger::warn("TailorUI: headwear transition could not complete");
                    TailorUI::GetSingleton().ShowEquipmentWarning(target,
                        "Headgear could not be changed. Another equipment rule may be protecting it.");
                }
                preview.SetViewport(viewport);
            } catch (const nlohmann::json::exception& e) {
                logger::error("tailorPreviewViewport: {}", e.what());
            }
        });
    });

    RegisterAction("tailorPreviewRotate", [](const char* arg) {
        QueueOpenAction([d = std::string(arg)]() {
            try {
                const auto json = nlohmann::json::parse(d);
                auto& ui = TailorUI::GetSingleton();
                if (!ui.IsOpen() || !ui.HasFocus()) return;
                const auto generation = json.value("openGeneration", std::uint64_t{0});
                if (!generation || generation != ui._previewOpenGeneration.load()) return;
                Tailor::Preview::TailorPreviewSession::GetSingleton().SetOrbit(
                    json.at("yaw").get<float>(), json.at("sequence").get<std::uint64_t>());
            } catch (const nlohmann::json::exception& e) {
                logger::warn("tailorPreviewRotate: {}", e.what());
            }
        });
    });

    // The footer's switch between the NPC the session opened on and the player.
    RegisterAction("tailorSwitchTarget", [](const char*) {
        QueueOpenAction([]() {
            TailorUI::GetSingleton().SwitchTarget();
        });
    });

    // A Settings switch, saved for every save at once. Disable Tailor Favorite applies now; Hide
    // Weapons and Hide Helmets at the first situation poll after Tailor closes.
    RegisterAction("tailorSetSetting", [](const char* data) {
        std::string name;
        bool on = false;
        try {
            const auto json = nlohmann::json::parse(data ? data : "");
            name = json.at("name").get<std::string>();
            on = json.at("on").get<bool>();
        } catch (const nlohmann::json::exception& e) {
            logger::warn("tailorSetSetting: {}", e.what());
            return;
        }
        QueueOpenAction([name = std::move(name), on]() {
            if (!PreferenceStore::GetSingleton().Set(name, on)) return;
            if (name == "disableFavorite") PowerHandler::ApplyFavorite();
            TailorUI::GetSingleton().SendSettings();
        });
    });

    // Load both blacklists from disk
    LoadBlacklist();
    LoadWigBlacklist();

    _initialized = true;
    Tailor::ImGuiUI::ImGuiHost::GetSingleton().Initialize();
    logger::info("TailorUI: initialized native ImGui UI with {} actions", _actions.size());
}

void TailorUI::Toggle()
{
    if (_isOpen) {
        Close();
    } else {
        Open();
    }
}

// Open and Close are idempotent: calling Open while already open (or Close while
// already closed) is a no-op. External callers reaching these through the C API
// cannot see _isOpen, so they must be safe to call unconditionally.
void TailorUI::Open()
{
    if (!_initialized || IsOpen()) return;
    // Tailor never handles children: with one under the crosshair it doesn't open, rather than open on the player.
    if (Tailor::Children::CrosshairIsChild()) {
        RE::SendHUDMessage::ShowHUDMessage(Tailor::Children::kRefusalMessage);
        logger::info("TailorUI: not opening on a child");
        return;
    }
    (void)Tailor::ImGuiUI::ImGuiHost::GetSingleton().RequestOpen();
}

// Native menu creation has succeeded; the host dispatches this on the game thread.
void TailorUI::OnNativeMenuShown()
{
    if (IsOpen()) return;
    if (!Tailor::ImGuiUI::ImGuiHost::GetSingleton().HasFocus()) return;
    logger::info("TailorUI: opening menu");

    auto& outfitMgr = OutfitManager::GetSingleton();
    // The crosshair moved to a child since Open: close instead of opening on the player.
    if (Tailor::Children::CrosshairIsChild()) {
        RE::SendHUDMessage::ShowHUDMessage(Tailor::Children::kRefusalMessage);
        logger::info("TailorUI: closing; a child is under the crosshair");
        Tailor::ImGuiUI::ImGuiHost::GetSingleton().RequestClose();
        return;
    }
    outfitMgr.UpdateTargetFromCrosshair();
    // No NPC under the crosshair: Tailor opens on the player.
    if (!outfitMgr.GetTarget()) outfitMgr.TargetPlayer();

    auto* target = outfitMgr.GetTarget();
    if (!WigManager::GetSingleton().SetTarget(target)) {
        logger::warn("TailorUI: previous headgear restoration must finish before changing target");
        Tailor::ImGuiUI::ImGuiHost::GetSingleton().RequestClose();
        return;
    }

    SendTargetUpdate();
    SendCategories();
    SendWigTargetUpdate();
    SendWigCategories();
    SendSettings();
    SendLibraryState();

    _isOpen = true;
    if (auto* calendar = RE::Calendar::GetSingleton(); calendar && calendar->timeScale) {
        _originalTimeScale = calendar->GetTimescale();
        calendar->timeScale->value = 1.0f;
        logger::info("TailorUI: timescale {} -> 1 while open", *_originalTimeScale);
    }
    // Exit a pre-existing TFC session before the preview captures its return
    // camera. Only run after focus succeeds, including opens without an NPC.
    if (!Tailor::Preview::ExitFlyCameraForOpen(RE::PlayerCamera::GetSingleton())) {
        logger::warn("TailorUI: could not exit the existing fly camera; closing");
        CloseForLifecycle(Tailor::Preview::EndReason::SetupFailed);
        return;
    }
    if (target) Tailor::Preview::TailorPreviewSession::GetSingleton().Begin(target->GetHandle());
    const auto previewOpenGeneration = ++_previewOpenGeneration;
    Publish("tailorSetPreviewOpenGeneration", previewOpenGeneration);
    Publish("tailorShowPanel");
    SendPreviewState();
}

void TailorUI::Close()
{
    CloseForLifecycle(Tailor::Preview::EndReason::UserClose);
}

void TailorUI::CloseForLifecycle(Tailor::Preview::EndReason reason)
{
    const bool wasOpen = _isOpen.exchange(false);
    ++_previewOpenGeneration;
    Tailor::ImGuiUI::ImGuiHost::GetSingleton().RequestClose();
    // Restore even on repeated or interrupted lifecycle cleanup.
    if (_originalTimeScale.has_value()) {
        const float originalTimeScale = *_originalTimeScale;
        _originalTimeScale.reset();
        if (auto* calendar = RE::Calendar::GetSingleton(); calendar && calendar->timeScale) {
            calendar->timeScale->value = originalTimeScale;
            logger::info("TailorUI: restored timescale {}", originalTimeScale);
        }
    }
    // Also retry a pending exact-copy restoration on repeated close calls.
    WigManager::GetSingleton().SetWigScreen(false);
    if (!wasOpen) {
        Tailor::Preview::TailorPreviewSession::GetSingleton().End(reason);
        return;
    }

    logger::info("TailorUI: closing menu");

    CancelTargetWork(reason);

    Tailor::Preview::TailorPreviewSession::GetSingleton().End(reason);


    RestorePlayerRunMode();
}

bool TailorUI::IsOpen() const
{
    return _isOpen;
}

bool TailorUI::HasFocus() const
{
    return _isOpen && Tailor::ImGuiUI::ImGuiHost::GetSingleton().HasFocus();
}

void TailorUI::CancelTargetWork(Tailor::Preview::EndReason reason)
{
    auto& outfitMgr = OutfitManager::GetSingleton();
    const bool worldReverting = reason == Tailor::Preview::EndReason::PreLoadGame ||
        reason == Tailor::Preview::EndReason::NewGame;
    // The load replaces the player's inventory: forget a player preview instead of undoing it.
    if (worldReverting) Tailor::Player::PlayerWardrobe::GetSingleton().DropPreview();

    // Cancel outfit cycling or create-outfit preview
    outfitMgr.CancelCycle();
    // PrepareForGameLoad restores captured pointers without equipment/morph
    // work when the world is about to be reverted.
    if (!worldReverting) {
        outfitMgr.EndCreateOutfit();
    }

    // Cancel wig cycling/preview — EquipWig/RemoveCurrentWig reconcile the
    // actor's inventory on every switch, so no separate cleanup pass is needed.
    auto& wigMgr = WigManager::GetSingleton();
    if (wigMgr.IsPreviewing()) {
        wigMgr.EndPreview();
    }
    if (wigMgr.IsCycling()) {
        wigMgr.CancelCycle();
    }
}

void TailorUI::SwitchTarget()
{
    auto& outfitMgr = OutfitManager::GetSingleton();
    auto& wigMgr = WigManager::GetSingleton();
    auto* current = outfitMgr.GetTarget();
    if (!IsOpen() || !current) return;
    // Everything in progress on the current target ends first, exactly as on Close.
    CancelTargetWork(Tailor::Preview::EndReason::TargetSwitched);
    if (!wigMgr.SetWigScreen(false)) {
        ShowEquipmentWarning(current, "Headgear could not be changed. Another equipment rule may be protecting it.");
        return;
    }
    // Headgear Tailor took off the other one and still has to put back goes back on first. If it can't, nothing moves:
    // the outfit side, the wig side and the preview all stay on this target.
    // Only someone the switch can reach: GetSessionNpc checks the NPC as TargetSessionNpc does, and the player is
    // checked as TargetPlayer does, so a player who can't be dressed (beast form, for one) has nothing put back.
    auto* player = RE::PlayerCharacter::GetSingleton();
    auto* destination = current->IsPlayerRef() ? outfitMgr.GetSessionNpc() : (Tailor::Player::CanDress(player) ? player : nullptr);
    if (destination && !wigMgr.RestorePendingHeadwear(destination)) {
        logger::warn("TailorUI: could not switch the target; its headgear could not be put back");
        SendTargetUpdate();
        ShowEquipmentWarning(current, "Couldn't switch: headgear could not be put back. Another equipment rule may be protecting it.");
        return;
    }
    const auto before = outfitMgr.SaveTarget();
    const bool switched = current->IsPlayerRef() ? outfitMgr.TargetSessionNpc() : outfitMgr.TargetPlayer();
    auto* target = outfitMgr.GetTarget();
    if (!switched || !target || !wigMgr.SetTarget(target)) {
        logger::warn("TailorUI: could not switch the target");
        // The outfit side goes back exactly as it was, without TargetPlayer's checks, to the target the wig side and
        // the preview still have; then the screen is told, and the toast shows on that target.
        outfitMgr.RestoreTarget(before);
        SendTargetUpdate();
        ShowEquipmentWarning(current, "Couldn't switch the target.");
        return;
    }
    auto& preview = Tailor::Preview::TailorPreviewSession::GetSingleton();
    preview.End(Tailor::Preview::EndReason::TargetSwitched);
    preview.Begin(target->GetHandle());
    // A new open generation drops actions aimed at the old target, and the screen
    // starts over: its opening page, a fresh viewport report and orbit.
    const auto generation = ++_previewOpenGeneration;
    Publish("tailorSetPreviewOpenGeneration", generation);
    SendTargetUpdate();
    SendCategories();
    SendWigTargetUpdate();
    SendWigCategories();
    Publish("tailorShowPanel");
    SendPreviewState();
    logger::info("TailorUI: switched the target to {}", target->GetDisplayFullName());
}

void TailorUI::SendPreviewState()
{
    auto& preview = Tailor::Preview::TailorPreviewSession::GetSingleton();
    nlohmann::json state{
        {"active", preview.IsReady()},
        {"message", preview.StatusMessage()},
        {"generation", preview.Generation()}
    };
    Publish("tailorSetPreviewState", state);
}

// ================================================================
// OUTFIT native state publishers
// ================================================================

void TailorUI::SendTargetUpdate()
{

    auto& mgr = OutfitManager::GetSingleton();

    nlohmann::json data;
    data["name"] = mgr.GetTargetName();
    data["sex"] = mgr.GetTargetSex();

    // Resolve current outfit name for the target
    std::string currentOutfit;
    auto* target = mgr.GetTarget();
    const bool player = target && target->IsPlayerRef();
    if (player) {
        // The player wears what the wardrobe put on; none means Own Gear.
        const int worn = Tailor::Player::PlayerWardrobe::GetSingleton().WornOutfitId();
        if (const auto* outfit = worn > 0 ? OutfitStore::GetSingleton().GetOutfitById(worn) : nullptr) currentOutfit = outfit->name;
    } else if (target) {
        auto* assignment = OutfitAssignments::GetSingleton().GetAssignment(target->GetFormID());
        if (assignment) {
            // A water exit can restore the previous selection even after the
            // location/day changed. Show the actual applied outfit when known.
            auto situation = SituationHandler::GetSingleton()->EvaluateSituation(target);
            auto applied = assignment->HasAnySituation()
                ? SituationHandler::GetSingleton()->GetAppliedOutfitId(target->GetFormID()) : std::nullopt;
            const int outfitId = applied ? *applied : SituationHandler::GetSingleton()->ResolveOutfitForSituation(target->GetFormID(), situation);
            if (outfitId > 0) {
                auto* outfit = OutfitStore::GetSingleton().GetOutfitById(outfitId);
                if (outfit) currentOutfit = outfit->name;
            }
        }
    }
    data["currentOutfit"] = currentOutfit;
    data["isPlayer"] = player;
    // The footer's switch button: from an NPC to the player, or back to the session's NPC.
    auto* pc = RE::PlayerCharacter::GetSingleton();
    std::string switchTo;
    if (player) {
        if (auto* npc = mgr.GetSessionNpc()) switchTo = SanitizeUtf8(npc->GetDisplayFullName());
    } else if (target && Tailor::Player::CanDress(pc)) {
        switchTo = "Player";
    }
    data["switchTo"] = switchTo;
    if (!target && Tailor::Player::InBeastForm(pc)) {
        data["notice"] = "Tailor can't dress you in this form.\nLook at an NPC, then reopen this menu.";
    }

    Publish("tailorSetTarget", data);
}

void TailorUI::SendCategories()
{

    auto& lib = OutfitLibrary::GetSingleton();
    auto categories = lib.GetCategories();
    // What the target can wear; with no target, every saved outfit counts.
    const auto wearable = OutfitManager::WearableBy(OutfitManager::GetSingleton().GetTarget());

    nlohmann::json arr = nlohmann::json::array();
    for (auto& cat : categories) {
        arr.push_back({
            {"id", cat.id},
            {"name", cat.name},
            {"displayName", lib.GetCategoryDisplayName(cat.id)},
            {"situationType", cat.situationType},
            {"armorType", OutfitArmorTypeName(cat.armorType)},
            {"outfitCount", static_cast<int>(cat.outfitIds.size())},
            {"fitCount", static_cast<int>(std::ranges::count_if(cat.outfitIds, wearable))},
            {"isDefault", cat.isDefault}
        });
    }

    Publish("tailorSetCategories", arr);
}

void TailorUI::SendCycleState()
{

    auto& mgr = OutfitManager::GetSingleton();
    auto* state = mgr.GetCycleState();
    if (!state) return;

    auto& store = OutfitStore::GetSingleton();

    nlohmann::json data;
    data["name"] = mgr.GetCycleOutfitName();
    data["index"] = state->index;
    data["total"] = static_cast<int>(state->outfitIds.size());

    // Armor rating of current outfit
    auto* currentOutfit = (state->index >= 0 && state->index < static_cast<int>(state->outfitIds.size()))
        ? store.GetOutfitById(state->outfitIds[state->index]) : nullptr;
    data["armorRating"] = currentOutfit ? CalcOutfitArmorRating(*currentOutfit) : 0;

    // Include the full item list for the native search dropdown
    nlohmann::json items = nlohmann::json::array();
    for (int i = 0; i < static_cast<int>(state->outfitIds.size()); i++) {
        auto* outfit = store.GetOutfitById(state->outfitIds[i]);
        if (outfit) {
            nlohmann::json outfitItems = nlohmann::json::array();
            for (auto& item : outfit->items) {
                std::string armorType = "Unknown";
                auto* armor = item.Resolve();
                if (armor) {
                    switch (armor->GetArmorType()) {
                    case RE::BGSBipedObjectForm::ArmorType::kLightArmor: armorType = "Light"; break;
                    case RE::BGSBipedObjectForm::ArmorType::kHeavyArmor: armorType = "Heavy"; break;
                    case RE::BGSBipedObjectForm::ArmorType::kClothing:   armorType = "Clothing"; break;
                    }
                }
                auto enchData = GetArmorEnchantData(armor);
                outfitItems.push_back({
                    {"formId", item.formId},
                    {"plugin", item.plugin},
                    {"name", item.name},
                    {"type", armorType},
                    {"armorRating", enchData["armorRating"]},
                    {"enchanted", enchData["enchanted"]},
                    {"enchantments", enchData["enchantments"]}
                });
            }
            items.push_back({
                {"index", i},
                {"name", outfit->name},
                {"armorRating", CalcOutfitArmorRating(*outfit)},
                {"items", outfitItems}
            });
        }
    }
    data["items"] = items;

    Publish("tailorSetCycleState", data);
}

void TailorUI::SendTransferData()
{
    nlohmann::json data;
    try {
        data = {{"outfits", OutfitTransfer::Catalog()}, {"files", OutfitTransfer::ListFiles()}};
    } catch (const std::exception& error) {
        data = {{"outfits", nlohmann::json::array()}, {"files", nlohmann::json::array()}, {"error", error.what()}};
    }
    Publish("tailorSetTransferData", data);
}

void TailorUI::ShowEquipmentWarning(RE::Actor* actor, std::string_view message)
{
    if (!IsOpen() || actor != OutfitManager::GetSingleton().GetTarget() ||
        !IsOpen()) return;
    const nlohmann::json text = message;
    Publish("toast", {{"message", text}, {"kind", "danger"}});
}

void TailorUI::ShowLibraryProblem(std::string_view message)
{
    Publish("toast", {{"message", std::string(message)}, {"kind", "danger"}});
}

void TailorUI::SendOutfits()
{

    auto& store = OutfitStore::GetSingleton();
    auto& outfits = store.GetOutfits();

    auto& lib = OutfitLibrary::GetSingleton();
    auto& categories = lib.GetCategories();

    nlohmann::json arr = nlohmann::json::array();
    for (auto& outfit : outfits) {
        nlohmann::json catNames = nlohmann::json::array();
        nlohmann::json categoryIds = nlohmann::json::array();
        for (auto& cat : categories) {
            for (auto id : cat.outfitIds) {
                if (id == outfit.id) {
                    catNames.push_back(lib.GetCategoryDisplayName(cat.id));
                    categoryIds.push_back(cat.id);
                    break;
                }
            }
        }
        bool hasEnchanted = false;
        for (auto& item : outfit.items) {
            if (auto* armor = item.Resolve()) {
                if (armor->formEnchanting) { hasEnchanted = true; break; }
            }
        }
        arr.push_back({
            {"id", outfit.id},
            {"name", outfit.name},
            {"sex", static_cast<int>(outfit.sex)},
            {"itemCount", static_cast<int>(outfit.items.size())},
            {"armorRating", CalcOutfitArmorRating(outfit)},
            {"categories", catNames},
            {"categoryIds", categoryIds},
            {"hasEnchanted", hasEnchanted}
        });
    }

    Publish("tailorSetOutfits", arr);
}

void TailorUI::SendDiscoveredOutfits()
{
    auto outfits = Tailor::Discovered::DiscoveredOutfits::GetSingleton().Snapshot();

    nlohmann::json arr = nlohmann::json::array();
    for (auto& outfit : outfits) {
        arr.push_back({
            {"id", outfit.id},
            {"name", outfit.name},
            {"sex", static_cast<int>(outfit.sex)},
            {"itemCount", static_cast<int>(outfit.items.size())},
        });
    }

    Publish("tailorSetDiscoveredOutfits", arr);
}

void TailorUI::SendCategoryOutfits(int categoryId)
{

    auto* cat = OutfitLibrary::GetSingleton().GetCategoryById(categoryId);
    auto& store = OutfitStore::GetSingleton();

    nlohmann::json data;
    data["categoryId"] = categoryId;
    data["outfits"] = nlohmann::json::array();

    if (cat) {
        for (auto outfitId : cat->outfitIds) {
            auto* outfit = store.GetOutfitById(outfitId);
            if (outfit) {
                data["outfits"].push_back({
                    {"id", outfit->id},
                    {"name", outfit->name},
                    {"itemCount", static_cast<int>(outfit->items.size())},
                    {"armorRating", CalcOutfitArmorRating(*outfit)}
                });
            }
        }
    }

    Publish("tailorSetCategoryOutfits", data);
}

void TailorUI::SendArmorPlugins()
{

    auto plugins = OutfitStore::GetSingleton().GetArmorPluginNames();

    nlohmann::json arr = nlohmann::json::array();
    for (auto& name : plugins) {
        if (IsBlacklisted(name)) continue;
        arr.push_back(name);
    }

    Publish("tailorSetArmorPlugins", arr);
}

void TailorUI::SendArmorForPlugin(const std::string& plugin)
{

    auto armors = OutfitStore::GetSingleton().GetArmorForPlugin(plugin);

    nlohmann::json arr = nlohmann::json::array();
    for (auto& item : armors) {
        std::string armorType = "Unknown";
        auto* armor = item.Resolve();
        if (armor) {
            switch (armor->GetArmorType()) {
            case RE::BGSBipedObjectForm::ArmorType::kLightArmor: armorType = "Light"; break;
            case RE::BGSBipedObjectForm::ArmorType::kHeavyArmor: armorType = "Heavy"; break;
            case RE::BGSBipedObjectForm::ArmorType::kClothing:   armorType = "Clothing"; break;
            }
        }
        auto enchData = GetArmorEnchantData(armor);
        arr.push_back({
            {"formId", item.formId},
            {"plugin", item.plugin},
            {"name", item.name},
            {"type", armorType},
            {"slot", GetArmorSlotName(armor)},
            {"armorRating", enchData["armorRating"]},
            {"enchanted", enchData["enchanted"]},
            {"enchantments", enchData["enchantments"]}
        });
    }

    nlohmann::json data;
    data["plugin"] = plugin;
    data["armors"] = arr;

    Publish("tailorSetArmorForPlugin", data);
}

void TailorUI::SendOutfitData(int outfitId)
{

    auto* outfit = OutfitStore::GetSingleton().GetOutfitById(outfitId);
    if (!outfit) return;

    std::vector<int> categoryIds;
    auto& categories = OutfitLibrary::GetSingleton().GetCategories();
    for (auto& cat : categories) {
        for (auto id : cat.outfitIds) {
            if (id == outfitId) {
                categoryIds.push_back(cat.id);
                break;
            }
        }
    }

    nlohmann::json data;
    data["outfitId"] = outfit->id;
    data["name"] = outfit->name;
    data["sex"] = static_cast<int>(outfit->sex);
    data["armorRating"] = CalcOutfitArmorRating(*outfit);
    data["categoryIds"] = categoryIds;
    data["categoryId"] = categoryIds.empty() ? 0 : categoryIds.front();  // Older views.
    data["items"] = nlohmann::json::array();

    for (auto& item : outfit->items) {
        std::string armorType = "Unknown";
        auto* armor = item.Resolve();
        if (armor) {
            switch (armor->GetArmorType()) {
            case RE::BGSBipedObjectForm::ArmorType::kLightArmor: armorType = "Light"; break;
            case RE::BGSBipedObjectForm::ArmorType::kHeavyArmor: armorType = "Heavy"; break;
            case RE::BGSBipedObjectForm::ArmorType::kClothing:   armorType = "Clothing"; break;
            }
        }
        auto enchData = GetArmorEnchantData(armor);
        data["items"].push_back({
            {"formId", item.formId},
            {"plugin", item.plugin},
            {"name", item.name},
            {"type", armorType},
            {"slot", GetArmorSlotName(armor)},
            {"armorRating", enchData["armorRating"]},
            {"enchanted", enchData["enchanted"]},
            {"enchantments", enchData["enchantments"]}
        });
    }

    Publish("tailorSetOutfitData", data);
}

void TailorUI::SendAllCategories()
{

    auto& lib = OutfitLibrary::GetSingleton();
    auto& allCats = lib.GetCategories();

    nlohmann::json catArr = nlohmann::json::array();
    nlohmann::json poolArr = nlohmann::json::array();
    for (auto& cat : allCats) {
        if (cat.isDefault) {
            continue;
        }

        catArr.push_back({
            {"id", cat.id},
            {"name", cat.name},
            {"displayName", lib.GetCategoryDisplayName(cat.id)},
            {"outfitCount", static_cast<int>(cat.outfitIds.size())}
        });
    }
    for (const auto& [name, type] : std::vector<std::pair<std::string, std::string>>{
            {"Adventuring", "adventuring"}, {"Town", "town"}, {"Home", "home"}, {"Sleep", "sleep"}, {"Swimming", "swimming"}, {"Warm", "warm"}}) {
        poolArr.push_back({{"name", name}, {"situationType", type},
            {"outfitCount", static_cast<int>(lib.GetSituationOutfitIds(type).size())}});
    }

    nlohmann::json data;
    data["categories"] = catArr;
    data["situationPools"] = poolArr;
    data["customCount"] = catArr.size();

    Publish("tailorSetAllCategories", data);
}

void TailorUI::SendSituationData()
{

    auto& mgr = OutfitManager::GetSingleton();
    auto* target = mgr.GetTarget();
    if (!target) return;

    auto& assignments = OutfitAssignments::GetSingleton();
    auto* sa = assignments.GetAssignment(target->GetFormID());

    auto& store = OutfitStore::GetSingleton();

    auto getOutfitName = [&](int id) -> std::string {
        if (id <= 0) return "";
        auto* o = store.GetOutfitById(id);
        return o ? o->name : "";
    };

    auto getOutfitAR = [&](int id) -> int {
        if (id <= 0) return 0;
        auto* o = store.GetOutfitById(id);
        return o ? CalcOutfitArmorRating(*o) : 0;
    };

    auto getOutfitEnch = [&](int id) -> bool {
        if (id <= 0) return false;
        auto* o = store.GetOutfitById(id);
        if (!o) return false;
        for (auto& item : o->items) {
            if (auto* armor = item.Resolve()) {
                if (armor->formEnchanting) return true;
            }
        }
        return false;
    };

    nlohmann::json data;
    data["adventuringId"] = sa ? sa->adventuringId : 0;
    data["adventuringName"] = getOutfitName(sa ? sa->adventuringId : 0);
    data["adventuringAR"] = getOutfitAR(sa ? sa->adventuringId : 0);
    data["adventuringEnch"] = getOutfitEnch(sa ? sa->adventuringId : 0);
    data["townId"] = sa ? sa->townId : 0;
    data["townName"] = getOutfitName(sa ? sa->townId : 0);
    data["townAR"] = getOutfitAR(sa ? sa->townId : 0);
    data["townEnch"] = getOutfitEnch(sa ? sa->townId : 0);
    data["homeId"] = sa ? sa->homeId : 0;
    data["homeName"] = getOutfitName(sa ? sa->homeId : 0);
    data["homeAR"] = getOutfitAR(sa ? sa->homeId : 0);
    data["homeEnch"] = getOutfitEnch(sa ? sa->homeId : 0);
    data["sleepId"] = sa ? sa->sleepId : 0;
    data["sleepName"] = getOutfitName(sa ? sa->sleepId : 0);
    data["sleepAR"] = getOutfitAR(sa ? sa->sleepId : 0);
    data["sleepEnch"] = getOutfitEnch(sa ? sa->sleepId : 0);
    data["swimmingId"] = sa ? sa->swimmingId : 0;
    data["swimmingName"] = getOutfitName(sa ? sa->swimmingId : 0);
    data["swimmingAR"] = getOutfitAR(sa ? sa->swimmingId : 0);
    data["swimmingEnch"] = getOutfitEnch(sa ? sa->swimmingId : 0);
    data["warmId"] = sa ? sa->warmId : 0;
    data["warmName"] = getOutfitName(sa ? sa->warmId : 0);
    data["warmAR"] = getOutfitAR(sa ? sa->warmId : 0);
    data["warmEnch"] = getOutfitEnch(sa ? sa->warmId : 0);

    // Randomize flags
    data["adventuringRandom"] = sa ? sa->adventuringRandom : false;
    data["townRandom"] = sa ? sa->townRandom : false;
    data["homeRandom"] = sa ? sa->homeRandom : false;
    data["sleepRandom"] = sa ? sa->sleepRandom : false;
    data["swimmingRandom"] = sa ? sa->swimmingRandom : false;
    data["warmRandom"] = sa ? sa->warmRandom : false;

    // Situation category outfit counts (for "Random from pool (X outfits)" display),
    // counting only what the target can wear.
    auto& lib = OutfitLibrary::GetSingleton();
    const auto wearable = OutfitManager::WearableBy(target);
    auto getCatCount = [&](const std::string& sitType) -> int {
        return static_cast<int>(std::ranges::count_if(lib.GetSituationOutfitIds(sitType), wearable));
    };
    const auto armorType = sa ? sa->adventuringArmorType : OutfitArmorType::Any;
    data["adventuringArmorType"] = OutfitArmorTypeName(armorType);
    auto matchingCount = [&](const std::vector<int>& ids) {
        return lib.FilterAdventuringEligible(ids, armorType, wearable).size();
    };
    data["adventuringCatCount"] = matchingCount(lib.GetSituationOutfitIds("adventuring"));
    data["adventuringAssignedCompatible"] = !sa || sa->adventuringId <= 0 ||
        lib.IsAdventuringEligible(sa->adventuringId, armorType, wearable);
    // Nothing is in both Adventuring and the chosen type, so all of Adventuring is in use.
    data["adventuringTypeFallback"] = armorType != OutfitArmorType::Any && !lib.HasAdventuringOutfitsOfType(armorType, wearable);
    data["adventuringCategoryCounts"] = nlohmann::json::object();
    bool hasDressOptions = false;
    for (const auto& category : lib.GetCategories()) {
        const auto count = matchingCount(category.outfitIds);
        data["adventuringCategoryCounts"][std::to_string(category.id)] = count;
        hasDressOptions = hasDressOptions || count > 0;
    }
    data["adventuringHasDressOptions"] = hasDressOptions;
    data["townCatCount"] = getCatCount("town");
    data["homeCatCount"] = getCatCount("home");
    data["sleepCatCount"] = getCatCount("sleep");
    data["swimmingCatCount"] = getCatCount("swimming");
    data["warmCatCount"] = getCatCount("warm");
    // A fixed outfit tagged for the other sex is skipped in game; its card warns.
    for (const auto& [key, id] : std::initializer_list<std::pair<const char*, int>>{
             {"adventuring", sa ? sa->adventuringId : 0}, {"town", sa ? sa->townId : 0},
             {"home", sa ? sa->homeId : 0}, {"sleep", sa ? sa->sleepId : 0}, {"swimming", sa ? sa->swimmingId : 0},
             {"warm", sa ? sa->warmId : 0}}) {
        data[std::string(key) + "Fits"] = id <= 0 || !store.GetOutfitById(id) || wearable(id);
    }

    Publish("tailorSetSituationData", data);
}

void TailorUI::SendOutfitUsage(int outfitId)
{

    auto actorIds = OutfitAssignments::GetSingleton().GetActorsUsingOutfit(outfitId);

    nlohmann::json data;
    data["outfitId"] = outfitId;
    data["actors"] = nlohmann::json::array();

    for (auto id : actorIds) {
        auto* actor = RE::TESForm::LookupByID<RE::Actor>(id);
        std::string name = actor ? SanitizeUtf8(actor->GetDisplayFullName()) : std::format("0x{:X}", id);
        data["actors"].push_back({{"name", name}, {"formId", id}});
    }

    Publish("tailorSetOutfitUsage", data);
}

// ================================================================
// WIG native state publishers
// ================================================================

void TailorUI::SendWigTargetUpdate()
{

    auto& outfitMgr = OutfitManager::GetSingleton();
    auto* target = outfitMgr.GetTarget();

    nlohmann::json data;
    data["name"] = outfitMgr.GetTargetName();
    data["sex"] = outfitMgr.GetTargetSex();
    data["currentWig"] = "";

    if (target) {
        auto assignment = WigAssignments::GetSingleton().GetAssignment(target->GetFormID());
        if (assignment) {
            data["currentWig"] = assignment->name;
        }
    }

    data["hasHairColor"] = false;
    data["isNFFManaged"] = false;
    if (target) {
        auto state = WigAssignments::GetSingleton().GetState(target->GetFormID());
        if (state && state->HasHairColor()) {
            data["hasHairColor"] = true;
            data["hairColorR"] = static_cast<int>(state->hairColorR);
            data["hairColorG"] = static_cast<int>(state->hairColorG);
            data["hairColorB"] = static_cast<int>(state->hairColorB);
        }
        // NFF manages followers, never the player.
        data["isNFFManaged"] = !target->IsPlayerRef() && WigManager::GetSingleton().IsNFFManaged(target);
    }

    Publish("wiggySetTarget", data);
}

void TailorUI::SendWigCategories()
{

    auto& library = WigLibrary::GetSingleton();

    nlohmann::json categories = nlohmann::json::array();
    for (size_t i = 0; i < kCategoryCount; ++i) {
        auto cat = static_cast<WigCategory>(i);
        auto wigs = library.GetCategory(cat);

        nlohmann::json wigsJson = nlohmann::json::array();
        for (auto& w : wigs) {
            wigsJson.push_back({
                {"formId", w.formId},
                {"plugin", w.plugin},
                {"name", w.name}
            });
        }

        categories.push_back({
            {"index", static_cast<int>(i)},
            {"name", std::string(kCategoryDisplayNames[i])},
            {"count", static_cast<int>(wigs.size())},
            {"wigs", wigsJson}
        });
    }

    Publish("wiggySetCategories", categories);
}

void TailorUI::SendWigCycleState()
{

    auto& mgr = WigManager::GetSingleton();
    auto wig = mgr.GetCurrentCycleWig();

    nlohmann::json state;
    if (wig) {
        state["name"] = wig->name;
        state["index"] = mgr.GetCycleIndex();
        state["total"] = mgr.GetCycleCount();

        // Include full item list so JS can populate the search dropdown
        nlohmann::json items = nlohmann::json::array();
        auto wigs = mgr.GetCycleWigs();
        for (int i = 0; i < static_cast<int>(wigs.size()); i++) {
            items.push_back({{"index", i}, {"name", wigs[i].name}});
        }
        state["items"] = items;
    } else {
        state["name"] = "";
        state["index"] = -1;
        state["total"] = 0;
        state["items"] = nlohmann::json::array();
    }

    Publish("wiggySetCycleState", state);
}

void TailorUI::SendDefaultHairResult(bool success)
{
    Publish("wiggyDefaultHairResult", success);
}

void TailorUI::SendModWigs()
{

    auto& mgr = WigManager::GetSingleton();
    auto mods = mgr.ScanAllModWigs();

    nlohmann::json arr = nlohmann::json::array();
    for (auto& mod : mods) {
        if (IsWigBlacklisted(mod.modName)) continue;

        nlohmann::json wigsArr = nlohmann::json::array();
        for (auto& w : mod.wigs) {
            wigsArr.push_back({
                {"formId", w.formId},
                {"plugin", w.plugin},
                {"name", w.name}
            });
        }
        arr.push_back({
            {"name", mod.modName},
            {"wigs", wigsArr}
        });
    }

    Publish("wiggySetModWigs", arr);
}

void TailorUI::SendWigBlacklistData()
{

    auto& mgr = WigManager::GetSingleton();
    auto mods = mgr.ScanAllModWigs();

    nlohmann::json arr = nlohmann::json::array();
    for (auto& mod : mods) {
        arr.push_back({
            {"name", mod.modName},
            {"wigCount", static_cast<int>(mod.wigs.size())},
            {"blacklisted", IsWigBlacklisted(mod.modName)}
        });
    }

    Publish("wiggySetBlacklistData", arr);
}

void TailorUI::SendHairColorState()
{

    auto* target = OutfitManager::GetSingleton().GetTarget();

    nlohmann::json data;
    if (target) {
        auto state = WigAssignments::GetSingleton().GetState(target->GetFormID());
        if (state && state->HasHairColor()) {
            data["r"] = static_cast<int>(state->hairColorR);
            data["g"] = static_cast<int>(state->hairColorG);
            data["b"] = static_cast<int>(state->hairColorB);
            data["isDefault"] = false;
        } else {
            data["isDefault"] = true;
        }
    } else {
        data["isDefault"] = true;
    }

    // Ship the user's custom color library alongside the per-actor state so
    // the Hair Color and Custom Colors screens can render both presets and
    // saved customs from a single round trip.
    auto customColors = CustomColorLibrary::GetSingleton().GetColors();
    auto arr = nlohmann::json::array();
    for (auto& c : customColors) {
        nlohmann::json entry;
        entry["r"] = static_cast<int>(c.r);
        entry["g"] = static_cast<int>(c.g);
        entry["b"] = static_cast<int>(c.b);
        arr.push_back(entry);
    }
    data["customColors"] = arr;

    Publish("wiggySetHairColor", data);
}

void TailorUI::SendWigSituationData()
{

    auto& mgr = WigManager::GetSingleton();
    auto* target = mgr.GetTarget();
    if (!target) return;

    auto& assignments = WigAssignments::GetSingleton();
    auto* sa = assignments.GetSituationalAssignment(target->GetFormID());

    nlohmann::json data;
    data["adventuringName"] = (sa && sa->adventuring.formId != 0) ? sa->adventuring.name : "";
    data["townName"]        = (sa && sa->town.formId != 0) ? sa->town.name : "";
    data["homeName"]        = (sa && sa->home.formId != 0) ? sa->home.name : "";
    data["sleepName"]       = (sa && sa->sleep.formId != 0) ? sa->sleep.name : "";

    Publish("wiggySetWigSituationData", data);
}

void TailorUI::SendSettings()
{
    const auto preferences = PreferenceStore::GetSingleton().Get();
    Publish("tailorSetSettings", {{"disableFavorite", preferences.disableFavorite},
        {"hideWeapons", preferences.hideWeapons}, {"hideHelmets", preferences.hideHelmets}});
}

// While outfits.json or library.json has saves off (a file Tailor couldn't fully read at startup), the screen
// disables Create Outfit, Save, Delete, Set Sex, Copy, Import and the Categories page's Create, Save Name and Delete
// Category, and Manage Outfits says why. Published at every open as a value,
// since a toast sent while the menu opens is swallowed by the screen's reset.
void TailorUI::SendLibraryState()
{
    const bool outfits = OutfitStore::GetSingleton().SaveAllowed();
    const bool library = OutfitLibrary::GetSingleton().SaveAllowed();
    std::string note;
    if (!outfits || !library) {
        note = std::format("Read-only: Tailor couldn't fully read {}. See Tailor.log, fix it and restart.",
            !outfits && !library ? "outfits.json and library.json" : !outfits ? "outfits.json" : "library.json");
    }
    Publish("tailorSetLibraryState", {{"readOnly", !outfits || !library}, {"note", note}});
}

// ================================================================
// OUTFIT Blacklist
// ================================================================

std::filesystem::path TailorUI::GetBlacklistPath() const
{
    auto path = std::filesystem::path("Data/SKSE/Plugins/Tailor");
    std::filesystem::create_directories(path);
    return path / "blacklist.json";
}

void TailorUI::LoadBlacklist()
{
    // Saves stay off until this load has read the whole file: a file Tailor couldn't read is never written over.
    _blacklistSaveAllowed = false;
    try {
        const auto path = GetBlacklistPath();
        std::error_code error;
        if (!std::filesystem::exists(path, error)) {
            // A file Tailor can't check is not a missing file: saves stay off.
            if (error) throw std::filesystem::filesystem_error("could not check blacklist.json", path, error);
            _blacklist.clear();
            _blacklistSaveAllowed = true;
            logger::info("TailorUI: no outfit blacklist.json found, starting empty");
            return;
        }

        std::ifstream file(path);
        const auto json = nlohmann::json::parse(file);
        if (!json.is_object() || (json.contains("version") && json["version"] != 1) ||
            !json.contains("plugins") || !json["plugins"].is_array()) {
            throw std::runtime_error("unsupported blacklist document");
        }
        std::set<std::string> parsed;
        bool complete = true;
        std::size_t row = 0;
        for (const auto& name : json["plugins"]) {
            ++row;
            if (name.is_string()) {
                parsed.insert(name.get<std::string>());
            } else {
                complete = false;
                logger::error("TailorUI: outfit blacklist row {} is not a plugin name; the other rows stay, blacklist.json is kept as it is and saves are off", row);
            }
        }
        _blacklist = std::move(parsed);
        _blacklistSaveAllowed = complete;
        logger::info("TailorUI: loaded {} outfit-blacklisted plugin(s)", _blacklist.size());
    } catch (const std::exception& e) {
        logger::error("TailorUI: failed to load the outfit blacklist; blacklist.json is kept as it is and saves are off: {}", e.what());
    }
}

void TailorUI::SaveBlacklist() const
{
    if (!_blacklistSaveAllowed) {
        logger::error("TailorUI: outfit blacklist save skipped because blacklist.json was not loaded completely");
        return;
    }

    nlohmann::json json;
    json["version"] = 1;
    json["plugins"] = nlohmann::json::array();
    for (auto& name : _blacklist) {
        json["plugins"].push_back(name);
    }

    try {
        const auto path = GetBlacklistPath();
        const auto contents = json.dump(2);
        std::string error;
        if (!Tailor::Persistence::WriteJsonFile(path, contents, error)) {
            logger::error("TailorUI: failed to save outfit blacklist.json: {}", error);
            return;
        }
        logger::info("TailorUI: saved {} outfit-blacklisted plugin(s)", _blacklist.size());
    } catch (const std::exception& e) {
        logger::error("TailorUI: failed to save outfit blacklist: {}", e.what());
    }
}

void TailorUI::BlacklistPlugin(const std::string& pluginName)
{
    _blacklist.insert(pluginName);
    SaveBlacklist();
}

void TailorUI::UnblacklistPlugin(const std::string& pluginName)
{
    _blacklist.erase(pluginName);
    SaveBlacklist();
}

void TailorUI::ClearBlacklist()
{
    _blacklist.clear();
    SaveBlacklist();
}

bool TailorUI::IsBlacklisted(const std::string& pluginName) const
{
    return _blacklist.contains(pluginName);
}

void TailorUI::SendBlacklistData()
{

    auto plugins = OutfitStore::GetSingleton().GetArmorPluginNames();

    nlohmann::json arr = nlohmann::json::array();
    for (auto& name : plugins) {
        arr.push_back({
            {"name", name},
            {"blacklisted", IsBlacklisted(name)}
        });
    }

    Publish("tailorSetBlacklistData", arr);
}

// ================================================================
// WIG Blacklist
// ================================================================

std::filesystem::path TailorUI::GetWigBlacklistPath() const
{
    auto path = std::filesystem::path("Data/SKSE/Plugins/Wiggy");
    std::filesystem::create_directories(path);
    return path / "blacklist.json";
}

void TailorUI::LoadWigBlacklist()
{
    // Saves stay off until this load has read the whole file: a file Tailor couldn't read is never written over.
    _wigBlacklistSaveAllowed = false;
    try {
        const auto path = GetWigBlacklistPath();
        std::error_code error;
        if (!std::filesystem::exists(path, error)) {
            // A file Tailor can't check is not a missing file: saves stay off.
            if (error) throw std::filesystem::filesystem_error("could not check the wig blacklist.json", path, error);
            _wigBlacklist.clear();
            _wigBlacklistSaveAllowed = true;
            logger::info("TailorUI: no wig blacklist.json found, starting empty");
            return;
        }

        std::ifstream file(path);
        const auto json = nlohmann::json::parse(file);
        if (!json.is_object() || (json.contains("version") && json["version"] != 1) ||
            !json.contains("plugins") || !json["plugins"].is_array()) {
            throw std::runtime_error("unsupported blacklist document");
        }
        std::set<std::string> parsed;
        bool complete = true;
        std::size_t row = 0;
        for (const auto& name : json["plugins"]) {
            ++row;
            if (name.is_string()) {
                parsed.insert(name.get<std::string>());
            } else {
                complete = false;
                logger::error("TailorUI: wig blacklist row {} is not a plugin name; the other rows stay, blacklist.json is kept as it is and saves are off", row);
            }
        }
        _wigBlacklist = std::move(parsed);
        _wigBlacklistSaveAllowed = complete;
        logger::info("TailorUI: loaded {} wig-blacklisted plugin(s)", _wigBlacklist.size());
    } catch (const std::exception& e) {
        logger::error("TailorUI: failed to load the wig blacklist; blacklist.json is kept as it is and saves are off: {}", e.what());
    }
}

void TailorUI::SaveWigBlacklist() const
{
    if (!_wigBlacklistSaveAllowed) {
        logger::error("TailorUI: wig blacklist save skipped because blacklist.json was not loaded completely");
        return;
    }

    nlohmann::json json;
    json["version"] = 1;
    json["plugins"] = nlohmann::json::array();
    for (auto& name : _wigBlacklist) {
        json["plugins"].push_back(name);
    }

    try {
        const auto path = GetWigBlacklistPath();
        const auto contents = json.dump(2);
        std::string error;
        if (!Tailor::Persistence::WriteJsonFile(path, contents, error)) {
            logger::error("TailorUI: failed to save wig blacklist.json: {}", error);
            return;
        }
        logger::info("TailorUI: saved {} wig-blacklisted plugin(s)", _wigBlacklist.size());
    } catch (const std::exception& e) {
        logger::error("TailorUI: failed to save wig blacklist: {}", e.what());
    }
}

void TailorUI::WigBlacklistPlugin(const std::string& pluginName)
{
    _wigBlacklist.insert(pluginName);
    SaveWigBlacklist();
}

void TailorUI::WigUnblacklistPlugin(const std::string& pluginName)
{
    _wigBlacklist.erase(pluginName);
    SaveWigBlacklist();
}

void TailorUI::ClearWigBlacklist()
{
    _wigBlacklist.clear();
    SaveWigBlacklist();
}

bool TailorUI::IsWigBlacklisted(const std::string& pluginName) const
{
    return _wigBlacklist.contains(pluginName);
}
