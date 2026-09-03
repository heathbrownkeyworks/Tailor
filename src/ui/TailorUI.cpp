#include "ui/TailorUI.h"

#include "events/SituationHandler.h"
#include "outfit/OutfitAssignments.h"
#include "outfit/OutfitLibrary.h"
#include "outfit/OutfitStore.h"
#include "preview/TailorPreviewSession.h"
#include "wig/CustomColorLibrary.h"
#include "wig/WigAssignments.h"
#include "wig/WigCategory.h"
#include "wig/WigLibrary.h"
#include "wig/WigManager.h"

extern Meridian::UI::View::IViewAPI* g_MeridianView;

namespace
{
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
        // (Meridian closes it through the UI message queue, same as PrismaUI
        // did), and the engine re-derives data.running during that teardown —
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

void TailorUI::Initialize()
{
    if (!g_MeridianView) {
        logger::error("TailorUI: Meridian.View/1 not available");
        return;
    }

    Meridian::UI::View::ViewCreateInfo viewInfo{};
    viewInfo.ownerName = "tailor";
    viewInfo.viewName = "main";
    viewInfo.startUrl = "mod://tailor/index.html";
    viewInfo.initiallyVisible = false;
    viewInfo.onDOMReady = [](Meridian::UI::View::ViewHandle) {
        logger::info("TailorUI: DOM ready");
    };
    _view = g_MeridianView->CreateView(&viewInfo);

    if (_view == Meridian::UI::View::INVALID_VIEW_HANDLE) {
        logger::error("TailorUI: failed to create Meridian view");
        return;
    }

    // ================================================================
    // OUTFIT JS -> C++ Listeners (33)
    // ================================================================

    // 1. tailorSelectCategory — start cycling in a category
    g_MeridianView->RegisterListener(_view, "tailorSelectCategory", [](const char* arg) {
        SKSE::GetTaskInterface()->AddTask([d = std::string(arg)]() {
            try {
                auto json = nlohmann::json::parse(d);
                int id = json.value("id", 0);
                OutfitManager::GetSingleton().StartCycle(id);
                TailorUI::GetSingleton().SendCycleState();
            } catch (const nlohmann::json::exception& e) {
                logger::error("tailorSelectCategory: {}", e.what());
            }
        });
    });

    // 2. tailorCycleNext
    g_MeridianView->RegisterListener(_view, "tailorCycleNext", [](const char*) {
        SKSE::GetTaskInterface()->AddTask([]() {
            OutfitManager::GetSingleton().CycleNext();
            TailorUI::GetSingleton().SendCycleState();
        });
    });

    // 3. tailorCyclePrev
    g_MeridianView->RegisterListener(_view, "tailorCyclePrev", [](const char*) {
        SKSE::GetTaskInterface()->AddTask([]() {
            OutfitManager::GetSingleton().CyclePrev();
            TailorUI::GetSingleton().SendCycleState();
        });
    });

    // 3b. tailorCycleToIndex — jump to a specific index in the cycle list
    g_MeridianView->RegisterListener(_view, "tailorCycleToIndex", [](const char* arg) {
        SKSE::GetTaskInterface()->AddTask([d = std::string(arg)]() {
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
    g_MeridianView->RegisterListener(_view, "tailorConfirmCycle", [](const char*) {
        SKSE::GetTaskInterface()->AddTask([]() {
            OutfitManager::GetSingleton().ConfirmCycle();
            TailorUI::GetSingleton().SendTargetUpdate();
        });
    });

    // 5. tailorCancelCycle
    g_MeridianView->RegisterListener(_view, "tailorCancelCycle", [](const char*) {
        SKSE::GetTaskInterface()->AddTask([]() {
            OutfitManager::GetSingleton().CancelCycle();
        });
    });

    // 6. tailorResetOutfit
    g_MeridianView->RegisterListener(_view, "tailorResetOutfit", [](const char*) {
        SKSE::GetTaskInterface()->AddTask([]() {
            auto& mgr = OutfitManager::GetSingleton();
            auto* target = mgr.GetTarget();
            if (target) {
                mgr.ResetOutfit(target);
                TailorUI::GetSingleton().SendTargetUpdate();
            }
        });
    });

    // 7. tailorRequestOutfits — send all custom outfits
    g_MeridianView->RegisterListener(_view, "tailorRequestOutfits", [](const char*) {
        SKSE::GetTaskInterface()->AddTask([]() {
            TailorUI::GetSingleton().SendOutfits();
        });
    });

    // 8. tailorAddOutfitToCategory
    g_MeridianView->RegisterListener(_view, "tailorAddOutfitToCategory", [](const char* arg) {
        SKSE::GetTaskInterface()->AddTask([d = std::string(arg)]() {
            try {
                auto json = nlohmann::json::parse(d);
                int categoryId = json.value("categoryId", 0);
                int outfitId = json.value("outfitId", 0);

                if (categoryId > 0 && outfitId > 0) {
                    auto& lib = OutfitLibrary::GetSingleton();
                    lib.AddOutfitToCategory(categoryId, outfitId);
                    lib.Save();
                    TailorUI::GetSingleton().SendCategoryOutfits(categoryId);
                    TailorUI::GetSingleton().SendCategories();
                }
            } catch (const nlohmann::json::exception& e) {
                logger::error("tailorAddOutfitToCategory: {}", e.what());
            }
        });
    });

    // 9. tailorRemoveOutfitFromCategory
    g_MeridianView->RegisterListener(_view, "tailorRemoveOutfitFromCategory", [](const char* arg) {
        SKSE::GetTaskInterface()->AddTask([d = std::string(arg)]() {
            try {
                auto json = nlohmann::json::parse(d);
                int categoryId = json.value("categoryId", 0);
                int outfitId = json.value("outfitId", 0);

                if (categoryId > 0 && outfitId > 0) {
                    auto& lib = OutfitLibrary::GetSingleton();
                    lib.RemoveOutfitFromCategory(categoryId, outfitId);
                    lib.Save();
                    TailorUI::GetSingleton().SendCategoryOutfits(categoryId);
                    TailorUI::GetSingleton().SendCategories();
                }
            } catch (const nlohmann::json::exception& e) {
                logger::error("tailorRemoveOutfitFromCategory: {}", e.what());
            }
        });
    });

    // 10. tailorRequestCategoryOutfits
    g_MeridianView->RegisterListener(_view, "tailorRequestCategoryOutfits", [](const char* arg) {
        SKSE::GetTaskInterface()->AddTask([d = std::string(arg)]() {
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
    g_MeridianView->RegisterListener(_view, "tailorDeleteOutfit", [](const char* arg) {
        SKSE::GetTaskInterface()->AddTask([d = std::string(arg)]() {
            try {
                auto json = nlohmann::json::parse(d);
                int outfitId = json.value("outfitId", 0);

                if (outfitId > 0) {
                    auto& store = OutfitStore::GetSingleton();
                    auto& lib = OutfitLibrary::GetSingleton();

                    lib.RemoveOutfitFromAllCategories(outfitId);
                    lib.Save();
                    store.DeleteOutfit(outfitId);
                    store.Save();

                    OutfitAssignments::GetSingleton().RemoveOutfitFromAllAssignments(outfitId);

                    TailorUI::GetSingleton().SendOutfits();
                    TailorUI::GetSingleton().SendCategories();
                }
            } catch (const nlohmann::json::exception& e) {
                logger::error("tailorDeleteOutfit: {}", e.what());
            }
        });
    });

    // 11b. tailorCheckOutfitUsage
    g_MeridianView->RegisterListener(_view, "tailorCheckOutfitUsage", [](const char* arg) {
        SKSE::GetTaskInterface()->AddTask([d = std::string(arg)]() {
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
    g_MeridianView->RegisterListener(_view, "tailorRequestArmorPlugins", [](const char*) {
        SKSE::GetTaskInterface()->AddTask([]() {
            TailorUI::GetSingleton().SendArmorPlugins();
        });
    });

    // 13. tailorRequestArmorForPlugin
    g_MeridianView->RegisterListener(_view, "tailorRequestArmorForPlugin", [](const char* arg) {
        SKSE::GetTaskInterface()->AddTask([d = std::string(arg)]() {
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
    g_MeridianView->RegisterListener(_view, "tailorEquipArmorItem", [](const char* arg) {
        SKSE::GetTaskInterface()->AddTask([d = std::string(arg)]() {
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
    g_MeridianView->RegisterListener(_view, "tailorUnequipArmorItem", [](const char* arg) {
        SKSE::GetTaskInterface()->AddTask([d = std::string(arg)]() {
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
    g_MeridianView->RegisterListener(_view, "tailorBeginCreateOutfit", [](const char*) {
        SKSE::GetTaskInterface()->AddTask([]() {
            auto& mgr = OutfitManager::GetSingleton();
            auto* target = mgr.GetTarget();
            if (target) {
                mgr.BeginCreateOutfit(target);
            }
        });
    });

    // 17. tailorSaveOutfit
    g_MeridianView->RegisterListener(_view, "tailorSaveOutfit", [](const char* arg) {
        SKSE::GetTaskInterface()->AddTask([d = std::string(arg)]() {
            try {
                auto json = nlohmann::json::parse(d);
                auto name = json.value("name", std::string{});
                int categoryId = json.value("categoryId", 0);

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
                    int outfitId = store.AddOutfit(name, items);
                    store.Save();

                    if (categoryId > 0) {
                        auto& lib = OutfitLibrary::GetSingleton();
                        lib.AddOutfitToCategory(categoryId, outfitId);
                        lib.Save();
                    }

                    auto& mgr = OutfitManager::GetSingleton();
                    auto* target = mgr.GetTarget();
                    if (target) {
                        mgr.EndCreateOutfit(target);
                        TailorUI::GetSingleton().RestoreCorrectOutfit(target);
                    }

                    TailorUI::GetSingleton().SendOutfits();
                    TailorUI::GetSingleton().SendCategories();
                    logger::info("Saved custom outfit '{}' (id={}) with {} items",
                        name, outfitId, items.size());
                }
            } catch (const nlohmann::json::exception& e) {
                logger::error("tailorSaveOutfit: {}", e.what());
            }
        });
    });

    // 18. tailorCancelCreateOutfit
    g_MeridianView->RegisterListener(_view, "tailorCancelCreateOutfit", [](const char*) {
        SKSE::GetTaskInterface()->AddTask([]() {
            auto& mgr = OutfitManager::GetSingleton();
            auto* target = mgr.GetTarget();
            if (target) {
                mgr.EndCreateOutfit(target);
                TailorUI::GetSingleton().RestoreCorrectOutfit(target);
            }
        });
    });

    // 19. tailorClose
    g_MeridianView->RegisterListener(_view, "tailorClose", [](const char*) {
        SKSE::GetTaskInterface()->AddTask([]() {
            TailorUI::GetSingleton().Close();
        });
    });

    // 20. tailorRequestOutfitData
    g_MeridianView->RegisterListener(_view, "tailorRequestOutfitData", [](const char* arg) {
        SKSE::GetTaskInterface()->AddTask([d = std::string(arg)]() {
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

    // 21. tailorUpdateOutfit
    g_MeridianView->RegisterListener(_view, "tailorUpdateOutfit", [](const char* arg) {
        SKSE::GetTaskInterface()->AddTask([d = std::string(arg)]() {
            try {
                auto json = nlohmann::json::parse(d);
                int outfitId = json.value("outfitId", 0);
                auto name = json.value("name", std::string{});
                int categoryId = json.value("categoryId", 0);

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
                    store.UpdateOutfit(outfitId, name, items);
                    store.Save();

                    auto& lib = OutfitLibrary::GetSingleton();
                    lib.RemoveOutfitFromAllCategories(outfitId);
                    if (categoryId > 0) {
                        lib.AddOutfitToCategory(categoryId, outfitId);
                    }
                    lib.Save();

                    auto& mgr = OutfitManager::GetSingleton();
                    auto* target = mgr.GetTarget();
                    if (target) {
                        mgr.EndCreateOutfit(target);
                        TailorUI::GetSingleton().RestoreCorrectOutfit(target);
                    }

                    TailorUI::GetSingleton().SendOutfits();
                    TailorUI::GetSingleton().SendCategories();
                    logger::info("Updated outfit '{}' (id={}) with {} items", name, outfitId, items.size());
                }
            } catch (const nlohmann::json::exception& e) {
                logger::error("tailorUpdateOutfit: {}", e.what());
            }
        });
    });

    // 22. tailorRequestAllCategories
    g_MeridianView->RegisterListener(_view, "tailorRequestAllCategories", [](const char*) {
        SKSE::GetTaskInterface()->AddTask([]() {
            TailorUI::GetSingleton().SendAllCategories();
        });
    });

    // 23. tailorAddCategory
    g_MeridianView->RegisterListener(_view, "tailorAddCategory", [](const char* arg) {
        SKSE::GetTaskInterface()->AddTask([d = std::string(arg)]() {
            try {
                auto json = nlohmann::json::parse(d);
                auto name = json.value("name", std::string{});
                auto sex = json.value("sex", std::string{});

                if (name.empty() || sex.empty()) return;

                auto& lib = OutfitLibrary::GetSingleton();
                lib.AddCategory(name, sex);
                lib.Save();
                TailorUI::GetSingleton().SendAllCategories();
                TailorUI::GetSingleton().SendCategories();
            } catch (const nlohmann::json::exception& e) {
                logger::error("tailorAddCategory: {}", e.what());
            }
        });
    });

    // 24. tailorRenameCategory
    g_MeridianView->RegisterListener(_view, "tailorRenameCategory", [](const char* arg) {
        SKSE::GetTaskInterface()->AddTask([d = std::string(arg)]() {
            try {
                auto json = nlohmann::json::parse(d);
                int categoryId = json.value("categoryId", 0);
                auto name = json.value("name", std::string{});

                if (categoryId > 0 && !name.empty()) {
                    auto& lib = OutfitLibrary::GetSingleton();
                    lib.RenameCategory(categoryId, name);
                    lib.Save();
                    TailorUI::GetSingleton().SendAllCategories();
                    TailorUI::GetSingleton().SendCategories();
                }
            } catch (const nlohmann::json::exception& e) {
                logger::error("tailorRenameCategory: {}", e.what());
            }
        });
    });

    // 25. tailorDeleteCategory
    g_MeridianView->RegisterListener(_view, "tailorDeleteCategory", [](const char* arg) {
        SKSE::GetTaskInterface()->AddTask([d = std::string(arg)]() {
            try {
                auto json = nlohmann::json::parse(d);
                int categoryId = json.value("categoryId", 0);

                if (categoryId > 0) {
                    auto& lib = OutfitLibrary::GetSingleton();
                    lib.DeleteCategory(categoryId);
                    lib.Save();
                    TailorUI::GetSingleton().SendAllCategories();
                    TailorUI::GetSingleton().SendCategories();
                }
            } catch (const nlohmann::json::exception& e) {
                logger::error("tailorDeleteCategory: {}", e.what());
            }
        });
    });

    // 26. tailorRequestBlacklist
    g_MeridianView->RegisterListener(_view, "tailorRequestBlacklist", [](const char*) {
        SKSE::GetTaskInterface()->AddTask([]() {
            TailorUI::GetSingleton().SendBlacklistData();
        });
    });

    // 27. tailorBlacklistPlugin
    g_MeridianView->RegisterListener(_view, "tailorBlacklistPlugin", [](const char* arg) {
        SKSE::GetTaskInterface()->AddTask([d = std::string(arg)]() {
            try {
                auto json = nlohmann::json::parse(d);
                auto name = json.value("plugin", std::string{});
                if (!name.empty()) {
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
    g_MeridianView->RegisterListener(_view, "tailorUnblacklistPlugin", [](const char* arg) {
        SKSE::GetTaskInterface()->AddTask([d = std::string(arg)]() {
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
    g_MeridianView->RegisterListener(_view, "tailorClearBlacklist", [](const char*) {
        SKSE::GetTaskInterface()->AddTask([]() {
            auto& ui = TailorUI::GetSingleton();
            ui.ClearBlacklist();
            ui.SendBlacklistData();
        });
    });

    // 30. tailorRequestSituations
    g_MeridianView->RegisterListener(_view, "tailorRequestSituations", [](const char*) {
        SKSE::GetTaskInterface()->AddTask([]() {
            TailorUI::GetSingleton().SendSituationData();
        });
    });

    // 31. tailorConfirmSituationCycle
    g_MeridianView->RegisterListener(_view, "tailorConfirmSituationCycle", [](const char* arg) {
        SKSE::GetTaskInterface()->AddTask([d = std::string(arg)]() {
            try {
                auto json = nlohmann::json::parse(d);
                int sit = json.value("situation", 0);
                logger::info("tailorConfirmSituationCycle: situation={}", sit);
                if (sit >= 1 && sit <= 4) {
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
                }
            } catch (const nlohmann::json::exception& e) {
                logger::error("tailorConfirmSituationCycle: {}", e.what());
            }
        });
    });

    // 32. tailorClearSituation
    g_MeridianView->RegisterListener(_view, "tailorClearSituation", [](const char* arg) {
        SKSE::GetTaskInterface()->AddTask([d = std::string(arg)]() {
            try {
                auto json = nlohmann::json::parse(d);
                int sit = json.value("situation", 0);
                auto& mgr = OutfitManager::GetSingleton();
                auto* target = mgr.GetTarget();
                if (target && sit >= 1 && sit <= 4) {
                    auto& assignments = OutfitAssignments::GetSingleton();
                    assignments.ClearSituation(target->GetFormID(), static_cast<OutfitSituation>(sit));
                    assignments.Save();
                    SituationHandler::GetSingleton()->ForceApplyForSituation(target);
                    TailorUI::GetSingleton().SendSituationData();
                }
            } catch (const nlohmann::json::exception& e) {
                logger::error("tailorClearSituation: {}", e.what());
            }
        });
    });

    // 33. tailorClearAllSituations
    g_MeridianView->RegisterListener(_view, "tailorClearAllSituations", [](const char*) {
        SKSE::GetTaskInterface()->AddTask([]() {
            auto& mgr = OutfitManager::GetSingleton();
            auto* target = mgr.GetTarget();
            if (target) {
                auto& assignments = OutfitAssignments::GetSingleton();
                assignments.ClearAllSituations(target->GetFormID());
                assignments.Save();
                SituationHandler::GetSingleton()->ForceApplyForSituation(target);
                TailorUI::GetSingleton().SendSituationData();
            }
        });
    });

    // 34. tailorToggleSituationRandom
    g_MeridianView->RegisterListener(_view, "tailorToggleSituationRandom", [](const char* arg) {
        SKSE::GetTaskInterface()->AddTask([d = std::string(arg)]() {
            try {
                auto json = nlohmann::json::parse(d);
                int sit = json.value("situation", 0);
                bool random = json.value("random", false);
                auto& mgr = OutfitManager::GetSingleton();
                auto* target = mgr.GetTarget();
                if (target && sit >= 1 && sit <= 4) {
                    auto& assignments = OutfitAssignments::GetSingleton();
                    assignments.SetSituationRandom(target->GetFormID(),
                        static_cast<OutfitSituation>(sit), random);
                    assignments.Save();
                    SituationHandler::GetSingleton()->ForceApplyForSituation(target);
                    TailorUI::GetSingleton().SendSituationData();
                }
            } catch (const nlohmann::json::exception& e) {
                logger::error("tailorToggleSituationRandom: {}", e.what());
            }
        });
    });

    // 35. tailorCopyOutfit
    g_MeridianView->RegisterListener(_view, "tailorCopyOutfit", [](const char* arg) {
        SKSE::GetTaskInterface()->AddTask([d = std::string(arg)]() {
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

                // Copy out before AddOutfit — it push_backs into the same vector `source`
                // points into, so the pointer can dangle the moment it reallocates.
                auto sourceName = source->name;
                auto items = source->items;

                int newId = store.AddOutfit(name, items);
                store.Save();

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
                if (!targetCategories.empty()) {
                    lib.Save();
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

    // ================================================================
    // WIG JS -> C++ Listeners (21)
    // ================================================================

    g_MeridianView->RegisterListener(_view, "wiggySelectCategory", [](const char* arg) {
        try {
            auto json = nlohmann::json::parse(arg);
            int catIdx = json.value("category", -1);
            if (catIdx < 0 || catIdx >= static_cast<int>(kCategoryCount)) {
                logger::warn("wiggySelectCategory: invalid category {}", catIdx);
                return;
            }

            auto category = static_cast<WigCategory>(catIdx);
            SKSE::GetTaskInterface()->AddTask([category]() {
                auto& mgr = WigManager::GetSingleton();
                auto* target = mgr.GetTarget();
                if (!target) {
                    logger::warn("wiggySelectCategory: no target");
                    return;
                }
                mgr.StartCycling(target, category);
                TailorUI::GetSingleton().SendWigCycleState();
            });
        } catch (const nlohmann::json::exception& e) {
            logger::error("wiggySelectCategory: JSON parse error: {}", e.what());
        }
    });

    g_MeridianView->RegisterListener(_view, "wiggyCycleNext", [](const char*) {
        SKSE::GetTaskInterface()->AddTask([]() {
            WigManager::GetSingleton().CycleNext();
            TailorUI::GetSingleton().SendWigCycleState();
        });
    });

    g_MeridianView->RegisterListener(_view, "wiggyCyclePrev", [](const char*) {
        SKSE::GetTaskInterface()->AddTask([]() {
            WigManager::GetSingleton().CyclePrev();
            TailorUI::GetSingleton().SendWigCycleState();
        });
    });

    g_MeridianView->RegisterListener(_view, "wiggyCycleToIndex", [](const char* arg) {
        SKSE::GetTaskInterface()->AddTask([d = std::string(arg)]() {
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

    g_MeridianView->RegisterListener(_view, "wiggyConfirmCycle", [](const char*) {
        SKSE::GetTaskInterface()->AddTask([]() {
            WigManager::GetSingleton().ConfirmCycle();
        });
    });

    g_MeridianView->RegisterListener(_view, "wiggyCancelCycle", [](const char*) {
        SKSE::GetTaskInterface()->AddTask([]() {
            WigManager::GetSingleton().CancelCycle();
        });
    });

    g_MeridianView->RegisterListener(_view, "wiggyResetWig", [](const char*) {
        SKSE::GetTaskInterface()->AddTask([]() {
            auto& mgr = WigManager::GetSingleton();
            auto* target = mgr.GetTarget();
            if (target) {
                mgr.ResetWig(target);
            }
            if (mgr.IsCycling()) {
                mgr.ConfirmCycle();
            }
        });
    });

    g_MeridianView->RegisterListener(_view, "wiggyRequestMods", [](const char*) {
        SKSE::GetTaskInterface()->AddTask([]() {
            TailorUI::GetSingleton().SendModWigs();
        });
    });

    g_MeridianView->RegisterListener(_view, "wiggyAddWig", [](const char* arg) {
        SKSE::GetTaskInterface()->AddTask([d = std::string(arg)]() {
            try {
                auto json = nlohmann::json::parse(d);
                int catIdx = json.value("category", -1);
                if (catIdx < 0 || catIdx >= static_cast<int>(kCategoryCount)) {
                    logger::warn("wiggyAddWig: invalid category {}", catIdx);
                    return;
                }

                WigEntry entry;
                entry.formId = json.value("formId", static_cast<RE::FormID>(0));
                entry.plugin = json.value("plugin", std::string{});
                entry.name = json.value("name", std::string{});

                auto category = static_cast<WigCategory>(catIdx);
                WigLibrary::GetSingleton().AddWig(category, entry);
                WigLibrary::GetSingleton().Save();
                TailorUI::GetSingleton().SendWigCategories();
            } catch (const nlohmann::json::exception& e) {
                logger::error("wiggyAddWig: JSON parse error: {}", e.what());
            }
        });
    });

    g_MeridianView->RegisterListener(_view, "wiggyRemoveWig", [](const char* arg) {
        SKSE::GetTaskInterface()->AddTask([d = std::string(arg)]() {
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

    g_MeridianView->RegisterListener(_view, "wiggyMoveWig", [](const char* arg) {
        SKSE::GetTaskInterface()->AddTask([d = std::string(arg)]() {
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

    g_MeridianView->RegisterListener(_view, "wiggyPreviewWig", [](const char* arg) {
        SKSE::GetTaskInterface()->AddTask([d = std::string(arg)]() {
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

    g_MeridianView->RegisterListener(_view, "wiggyEndPreview", [](const char*) {
        SKSE::GetTaskInterface()->AddTask([]() {
            auto& mgr = WigManager::GetSingleton();
            if (mgr.IsPreviewing()) {
                mgr.EndPreview();
            }
        });
    });

    // --- Wig blacklist listeners ---

    g_MeridianView->RegisterListener(_view, "wiggyRequestBlacklist", [](const char*) {
        SKSE::GetTaskInterface()->AddTask([]() {
            TailorUI::GetSingleton().SendWigBlacklistData();
        });
    });

    g_MeridianView->RegisterListener(_view, "wiggyBlacklistPlugin", [](const char* arg) {
        SKSE::GetTaskInterface()->AddTask([d = std::string(arg)]() {
            try {
                auto json = nlohmann::json::parse(d);
                auto name = json.value("plugin", std::string{});
                if (!name.empty()) {
                    auto& ui = TailorUI::GetSingleton();
                    ui.WigBlacklistPlugin(name);
                    ui.SendWigBlacklistData();
                }
            } catch (const nlohmann::json::exception& e) {
                logger::error("wiggyBlacklistPlugin: JSON parse error: {}", e.what());
            }
        });
    });

    g_MeridianView->RegisterListener(_view, "wiggyUnblacklistPlugin", [](const char* arg) {
        SKSE::GetTaskInterface()->AddTask([d = std::string(arg)]() {
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

    g_MeridianView->RegisterListener(_view, "wiggyClearBlacklist", [](const char*) {
        SKSE::GetTaskInterface()->AddTask([]() {
            auto& ui = TailorUI::GetSingleton();
            ui.ClearWigBlacklist();
            ui.SendWigBlacklistData();
        });
    });

    // --- Hair color listeners ---

    g_MeridianView->RegisterListener(_view, "wiggyOpenHairColor", [](const char*) {
        SKSE::GetTaskInterface()->AddTask([]() {
            TailorUI::GetSingleton().SendHairColorState();
        });
    });

    g_MeridianView->RegisterListener(_view, "wiggyApplyHairColor", [](const char* arg) {
        SKSE::GetTaskInterface()->AddTask([d = std::string(arg)]() {
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

    g_MeridianView->RegisterListener(_view, "wiggyConfirmHairColor", [](const char* arg) {
        SKSE::GetTaskInterface()->AddTask([d = std::string(arg)]() {
            try {
                auto json = nlohmann::json::parse(d);
                auto r = static_cast<int16_t>(json.value("r", 0));
                auto g = static_cast<int16_t>(json.value("g", 0));
                auto b = static_cast<int16_t>(json.value("b", 0));

                auto& mgr = WigManager::GetSingleton();
                auto* target = mgr.GetTarget();
                if (target) {
                    mgr.ApplyHairColor(target,
                        static_cast<uint8_t>(r),
                        static_cast<uint8_t>(g),
                        static_cast<uint8_t>(b));
                    WigAssignments::GetSingleton().SetHairColor(target->GetFormID(), r, g, b);
                    WigAssignments::GetSingleton().Save();
                    // Heal any neighbour still sharing a hair material bled by an older build.
                    mgr.RetintNearbyActors(target);
                    logger::info("Confirmed hair color ({}, {}, {}) for {}",
                        r, g, b, target->GetDisplayFullName());
                }
            } catch (const nlohmann::json::exception& e) {
                logger::error("wiggyConfirmHairColor: JSON parse error: {}", e.what());
            }
        });
    });

    g_MeridianView->RegisterListener(_view, "wiggyResetHairColor", [](const char*) {
        SKSE::GetTaskInterface()->AddTask([]() {
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
            }
        });
    });

    g_MeridianView->RegisterListener(_view, "wiggyCloseHairColor", [](const char*) {
        // Purely a UI navigation event — JS handles panel switching.
    });

    // --- Custom color library listeners ---

    g_MeridianView->RegisterListener(_view, "wiggyAddCustomColor", [](const char* arg) {
        SKSE::GetTaskInterface()->AddTask([d = std::string(arg)]() {
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

    g_MeridianView->RegisterListener(_view, "wiggyDeleteCustomColor", [](const char* arg) {
        SKSE::GetTaskInterface()->AddTask([d = std::string(arg)]() {
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

    g_MeridianView->RegisterListener(_view, "wiggyRequestWigSituations", [](const char*) {
        SKSE::GetTaskInterface()->AddTask([]() {
            TailorUI::GetSingleton().SendWigSituationData();
        });
    });

    g_MeridianView->RegisterListener(_view, "wiggyConfirmSituationCycle", [](const char* arg) {
        SKSE::GetTaskInterface()->AddTask([d = std::string(arg)]() {
            try {
                auto json = nlohmann::json::parse(d);
                int sit = json.value("situation", 0);
                if (sit >= 1 && sit <= 4) {
                    auto& mgr = WigManager::GetSingleton();
                    mgr.ConfirmCycle(static_cast<OutfitSituation>(sit));
                    TailorUI::GetSingleton().SendWigSituationData();
                }
            } catch (const nlohmann::json::exception& e) {
                logger::error("wiggyConfirmSituationCycle: {}", e.what());
            }
        });
    });

    g_MeridianView->RegisterListener(_view, "wiggyClearWigSituation", [](const char* arg) {
        SKSE::GetTaskInterface()->AddTask([d = std::string(arg)]() {
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

    g_MeridianView->RegisterListener(_view, "wiggyClearAllWigSituations", [](const char*) {
        SKSE::GetTaskInterface()->AddTask([]() {
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

    g_MeridianView->RegisterListener(_view, "tailorPreviewViewport", [](const char* arg) {
        SKSE::GetTaskInterface()->AddTask([d = std::string(arg)]() {
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
                    json.value("height", 0.0f)
                };
                auto& preview = Tailor::Preview::TailorPreviewSession::GetSingleton();
                if (preview.IsActive()) {
                    preview.SetViewport(viewport);
                } else if (auto* target = OutfitManager::GetSingleton().GetTarget()) {
                    preview.Begin(target->GetHandle(), viewport);
                }
                ui.SendPreviewState();
            } catch (const nlohmann::json::exception& e) {
                logger::error("tailorPreviewViewport: {}", e.what());
            }
        });
    });

    // Load both blacklists from disk
    LoadBlacklist();
    LoadWigBlacklist();

    logger::info("TailorUI: initialized with Meridian UI view");
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
    if (!g_MeridianView || _view == Meridian::UI::View::INVALID_VIEW_HANDLE) {
        logger::warn("TailorUI::Open: not initialized");
        return;
    }
    if (_isOpen) {
        return;
    }

    // Block if another Meridian view is already focused (e.g. Horde)
    if (g_MeridianView->HasAnyFocus() && !g_MeridianView->HasFocus(_view)) {
        return;
    }

    logger::info("TailorUI: opening menu");

    auto& outfitMgr = OutfitManager::GetSingleton();
    outfitMgr.UpdateTargetFromCrosshair();

    auto* target = outfitMgr.GetTarget();
    if (target) {
        WigManager::GetSingleton().SetTarget(target);
    }

    SendTargetUpdate();
    SendCategories();
    SendWigTargetUpdate();
    SendWigCategories();

    // Cancel any deferred Hide() from a previous Close() — the dock is back.
    ++_hideGeneration;

    g_MeridianView->Show(_view);
    // Unpaused: the game world keeps ticking while the preview session softly
    // holds the target. Meridian's FocusMenu supplies the cursor and captures input.
    const auto focusResult = g_MeridianView->TryFocus(
        _view, Meridian::UI::View::FocusMode::Unpaused);
    if (focusResult != Meridian::UI::View::FocusResult::Granted &&
        focusResult != Meridian::UI::View::FocusResult::AlreadyFocused) {
        g_MeridianView->Hide(_view);
        if (focusResult != Meridian::UI::View::FocusResult::Busy) {
            logger::warn("TailorUI: Meridian focus request failed ({})",
                         static_cast<std::uint32_t>(focusResult));
        }
        return;
    }

    _isOpen = true;
    const auto previewOpenGeneration = ++_previewOpenGeneration;
    g_MeridianView->ExecuteJavaScript(_view, std::format(
        "tailorSetPreviewOpenGeneration({})", previewOpenGeneration).c_str());
    g_MeridianView->ExecuteJavaScript(_view, "tailorShowPanel()");
}

void TailorUI::Close()
{
    CloseForLifecycle(Tailor::Preview::EndReason::UserClose);
}

void TailorUI::CloseForLifecycle(Tailor::Preview::EndReason reason)
{
    const bool wasOpen = _isOpen.exchange(false);
    ++_previewOpenGeneration;
    if (!wasOpen) {
        Tailor::Preview::TailorPreviewSession::GetSingleton().End(reason);
        return;
    }

    logger::info("TailorUI: closing menu");

    // Cancel outfit cycling or create-outfit preview
    auto& outfitMgr = OutfitManager::GetSingleton();
    outfitMgr.CancelCycle();
    if (auto* target = outfitMgr.GetTarget()) {
        outfitMgr.EndCreateOutfit(target);
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

    Tailor::Preview::TailorPreviewSession::GetSingleton().End(reason);

    if (g_MeridianView && _view != Meridian::UI::View::INVALID_VIEW_HANDLE) {
        g_MeridianView->Unfocus(_view);

        // Let the dock play its 280ms slide-out before hiding the view. Unfocus
        // (above) releases input immediately so the player can move during the
        // animation; Hide is deferred past the transition. A reopen bumps
        // _hideGeneration, which makes the pending Hide a no-op — the view stays
        // visible and the JS dock reverses into its slide-in.
        g_MeridianView->ExecuteJavaScript(_view, "tailorHidePanel()");
        const auto generation = _hideGeneration.load();
        std::thread([this, generation]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(320));
            auto view = _view;
            SKSE::GetTaskInterface()->AddTask([this, view, generation]() {
                if (generation != _hideGeneration.load()) return;  // reopened meanwhile
                if (g_MeridianView) g_MeridianView->Hide(view);
                // The deferred Hide lands after its own menu-teardown pass, whose
                // run-state re-derive can land after the Close()-anchored restore
                // writes. Re-anchor the restore brackets to this teardown too.
                RestorePlayerRunMode();
            });
        }).detach();
    }

    RestorePlayerRunMode();
}

bool TailorUI::IsOpen() const
{
    return _isOpen;
}

bool TailorUI::HasFocus() const
{
    return _isOpen && g_MeridianView &&
           _view != Meridian::UI::View::INVALID_VIEW_HANDLE &&
           g_MeridianView->HasFocus(_view);
}

void TailorUI::SendPreviewState()
{
    if (!g_MeridianView || _view == Meridian::UI::View::INVALID_VIEW_HANDLE) return;
    auto& preview = Tailor::Preview::TailorPreviewSession::GetSingleton();
    nlohmann::json state{
        {"active", preview.IsActive()},
        {"generation", preview.Generation()}
    };
    g_MeridianView->ExecuteJavaScript(
        _view, std::format("tailorSetPreviewState({})", state.dump()).c_str());
}

// ================================================================
// OUTFIT C++ -> JS helpers
// ================================================================

void TailorUI::SendTargetUpdate()
{
    if (!g_MeridianView || _view == Meridian::UI::View::INVALID_VIEW_HANDLE) return;

    auto& mgr = OutfitManager::GetSingleton();

    nlohmann::json data;
    data["name"] = mgr.GetTargetName();
    data["sex"] = mgr.GetTargetSex();

    // Resolve current outfit name for the target
    std::string currentOutfit;
    auto* target = mgr.GetTarget();
    if (target) {
        auto* assignment = OutfitAssignments::GetSingleton().GetAssignment(target->GetFormID());
        if (assignment) {
            // Check situational outfit first, then adventuring fallback
            auto situation = SituationHandler::GetSingleton()->EvaluateSituation(target);
            int outfitId = assignment->GetSlot(situation);
            if (outfitId <= 0)
                outfitId = assignment->adventuringId;
            if (outfitId > 0) {
                auto* outfit = OutfitStore::GetSingleton().GetOutfitById(outfitId);
                if (outfit) currentOutfit = outfit->name;
            }
        }
    }
    data["currentOutfit"] = currentOutfit;

    std::string js = std::format("tailorSetTarget({})", data.dump());
    g_MeridianView->ExecuteJavaScript(_view, js.c_str());
}

void TailorUI::SendCategories()
{
    if (!g_MeridianView || _view == Meridian::UI::View::INVALID_VIEW_HANDLE) return;

    auto& mgr = OutfitManager::GetSingleton();
    auto sex = mgr.GetTargetSex();

    auto categories = OutfitLibrary::GetSingleton().GetCategoriesForSex(sex);

    nlohmann::json arr = nlohmann::json::array();
    for (auto& cat : categories) {
        arr.push_back({
            {"id", cat.id},
            {"name", cat.name},
            {"outfitCount", static_cast<int>(cat.outfitIds.size())},
            {"isDefault", cat.isDefault}
        });
    }

    std::string js = std::format("tailorSetCategories({})", arr.dump());
    g_MeridianView->ExecuteJavaScript(_view, js.c_str());
}

void TailorUI::SendCycleState()
{
    if (!g_MeridianView || _view == Meridian::UI::View::INVALID_VIEW_HANDLE) return;

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

    // Include full item list so JS can populate the search dropdown
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

    std::string js = std::format("tailorSetCycleState({})", data.dump());
    g_MeridianView->ExecuteJavaScript(_view, js.c_str());
}

void TailorUI::SendOutfits()
{
    if (!g_MeridianView || _view == Meridian::UI::View::INVALID_VIEW_HANDLE) return;

    auto& store = OutfitStore::GetSingleton();
    auto& outfits = store.GetOutfits();

    auto& lib = OutfitLibrary::GetSingleton();
    auto& categories = lib.GetCategories();

    nlohmann::json arr = nlohmann::json::array();
    for (auto& outfit : outfits) {
        nlohmann::json catNames = nlohmann::json::array();
        for (auto& cat : categories) {
            for (auto id : cat.outfitIds) {
                if (id == outfit.id) {
                    catNames.push_back(cat.name);
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
            {"itemCount", static_cast<int>(outfit.items.size())},
            {"armorRating", CalcOutfitArmorRating(outfit)},
            {"categories", catNames},
            {"hasEnchanted", hasEnchanted}
        });
    }

    std::string js = std::format("tailorSetOutfits({})", arr.dump());
    g_MeridianView->ExecuteJavaScript(_view, js.c_str());
}

void TailorUI::SendCategoryOutfits(int categoryId)
{
    if (!g_MeridianView || _view == Meridian::UI::View::INVALID_VIEW_HANDLE) return;

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

    std::string js = std::format("tailorSetCategoryOutfits({})", data.dump());
    g_MeridianView->ExecuteJavaScript(_view, js.c_str());
}

void TailorUI::SendArmorPlugins()
{
    if (!g_MeridianView || _view == Meridian::UI::View::INVALID_VIEW_HANDLE) return;

    auto plugins = OutfitStore::GetSingleton().GetArmorPluginNames();

    nlohmann::json arr = nlohmann::json::array();
    for (auto& name : plugins) {
        if (IsBlacklisted(name)) continue;
        arr.push_back(name);
    }

    std::string js = std::format("tailorSetArmorPlugins({})", arr.dump());
    g_MeridianView->ExecuteJavaScript(_view, js.c_str());
}

void TailorUI::SendArmorForPlugin(const std::string& plugin)
{
    if (!g_MeridianView || _view == Meridian::UI::View::INVALID_VIEW_HANDLE) return;

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

    std::string js = std::format("tailorSetArmorForPlugin({})", data.dump());
    g_MeridianView->ExecuteJavaScript(_view, js.c_str());
}

void TailorUI::SendOutfitData(int outfitId)
{
    if (!g_MeridianView || _view == Meridian::UI::View::INVALID_VIEW_HANDLE) return;

    auto* outfit = OutfitStore::GetSingleton().GetOutfitById(outfitId);
    if (!outfit) return;

    int categoryId = 0;
    auto& categories = OutfitLibrary::GetSingleton().GetCategories();
    for (auto& cat : categories) {
        for (auto id : cat.outfitIds) {
            if (id == outfitId) {
                categoryId = cat.id;
                break;
            }
        }
        if (categoryId > 0) break;
    }

    nlohmann::json data;
    data["outfitId"] = outfit->id;
    data["name"] = outfit->name;
    data["armorRating"] = CalcOutfitArmorRating(*outfit);
    data["categoryId"] = categoryId;
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
            {"armorRating", enchData["armorRating"]},
            {"enchanted", enchData["enchanted"]},
            {"enchantments", enchData["enchantments"]}
        });
    }

    std::string js = std::format("tailorSetOutfitData({})", data.dump());
    g_MeridianView->ExecuteJavaScript(_view, js.c_str());
}

void TailorUI::SendAllCategories()
{
    if (!g_MeridianView || _view == Meridian::UI::View::INVALID_VIEW_HANDLE) return;

    auto& lib = OutfitLibrary::GetSingleton();
    auto& allCats = lib.GetCategories();

    int femaleCustomCount = 0;
    int maleCustomCount = 0;

    nlohmann::json catArr = nlohmann::json::array();
    nlohmann::json poolArr = nlohmann::json::array();
    for (auto& cat : allCats) {
        if (cat.isDefault) {
            // Surface situation pools (read-only) so the Categories panel can show their counts.
            if (!cat.situationType.empty()) {
                poolArr.push_back({
                    {"id", cat.id},
                    {"name", cat.name},
                    {"sex", cat.sex},
                    {"situationType", cat.situationType},
                    {"outfitCount", static_cast<int>(cat.outfitIds.size())}
                });
            }
            continue;
        }

        if (cat.sex == "female") femaleCustomCount++;
        if (cat.sex == "male") maleCustomCount++;

        catArr.push_back({
            {"id", cat.id},
            {"name", cat.name},
            {"sex", cat.sex},
            {"outfitCount", static_cast<int>(cat.outfitIds.size())}
        });
    }

    nlohmann::json data;
    data["categories"] = catArr;
    data["situationPools"] = poolArr;
    data["femaleCustomCount"] = femaleCustomCount;
    data["maleCustomCount"] = maleCustomCount;

    std::string js = std::format("tailorSetAllCategories({})", data.dump());
    g_MeridianView->ExecuteJavaScript(_view, js.c_str());
}

void TailorUI::SendSituationData()
{
    if (!g_MeridianView || _view == Meridian::UI::View::INVALID_VIEW_HANDLE) return;

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

    // Randomize flags
    data["adventuringRandom"] = sa ? sa->adventuringRandom : false;
    data["townRandom"] = sa ? sa->townRandom : false;
    data["homeRandom"] = sa ? sa->homeRandom : false;
    data["sleepRandom"] = sa ? sa->sleepRandom : false;

    // Situation category outfit counts (for "Random from pool (X outfits)" display)
    // Pool is sex-specific (v2.0+) — look up using the target's sex.
    auto& lib = OutfitLibrary::GetSingleton();
    auto* targetNpc = target->GetActorBase();
    std::string targetSex = (targetNpc && targetNpc->GetSex() == RE::SEX::kFemale) ? "female" : "male";
    auto getCatCount = [&](const std::string& sitType) -> int {
        auto* cat = lib.GetCategoryBySituationType(sitType, targetSex);
        return cat ? static_cast<int>(cat->outfitIds.size()) : 0;
    };
    data["adventuringCatCount"] = getCatCount("adventuring");
    data["townCatCount"] = getCatCount("town");
    data["homeCatCount"] = getCatCount("home");
    data["sleepCatCount"] = getCatCount("sleep");

    std::string js = std::format("tailorSetSituationData({})", data.dump());
    g_MeridianView->ExecuteJavaScript(_view, js.c_str());
}

void TailorUI::SendOutfitUsage(int outfitId)
{
    if (!g_MeridianView || _view == Meridian::UI::View::INVALID_VIEW_HANDLE) return;

    auto actorIds = OutfitAssignments::GetSingleton().GetActorsUsingOutfit(outfitId);

    nlohmann::json data;
    data["outfitId"] = outfitId;
    data["actors"] = nlohmann::json::array();

    for (auto id : actorIds) {
        auto* actor = RE::TESForm::LookupByID<RE::Actor>(id);
        std::string name = actor ? SanitizeUtf8(actor->GetDisplayFullName()) : std::format("0x{:X}", id);
        data["actors"].push_back({{"name", name}, {"formId", id}});
    }

    std::string js = std::format("tailorSetOutfitUsage({})", data.dump());
    g_MeridianView->ExecuteJavaScript(_view, js.c_str());
}

void TailorUI::RestoreCorrectOutfit(RE::Actor* actor)
{
    if (!actor) return;

    auto& assignments = OutfitAssignments::GetSingleton();
    auto actorId = actor->GetFormID();

    if (assignments.HasAnySituation(actorId)) {
        SituationHandler::GetSingleton()->ForceApplyForSituation(actor);
        return;
    }

    int outfitId = assignments.GetOutfitId(actorId);
    if (outfitId > 0) {
        auto* outfit = OutfitStore::GetSingleton().GetOutfitById(outfitId);
        if (outfit) {
            OutfitManager::GetSingleton().ApplyCustomOutfit(actor, *outfit);
            return;
        }
    }

    logger::info("RestoreCorrectOutfit: {} has no assignment, vanilla outfit restored", actor->GetDisplayFullName());
}

// ================================================================
// WIG C++ -> JS helpers
// ================================================================

void TailorUI::SendWigTargetUpdate()
{
    if (!g_MeridianView || _view == Meridian::UI::View::INVALID_VIEW_HANDLE) return;

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
        data["isNFFManaged"] = WigManager::GetSingleton().IsNFFManaged(target);
    }

    std::string js = std::format("wiggySetTarget({})", data.dump());
    g_MeridianView->ExecuteJavaScript(_view, js.c_str());
}

void TailorUI::SendWigCategories()
{
    if (!g_MeridianView || _view == Meridian::UI::View::INVALID_VIEW_HANDLE) return;

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

    std::string js = std::format("wiggySetCategories({})", categories.dump());
    g_MeridianView->ExecuteJavaScript(_view, js.c_str());
}

void TailorUI::SendWigCycleState()
{
    if (!g_MeridianView || _view == Meridian::UI::View::INVALID_VIEW_HANDLE) return;

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

    std::string js = std::format("wiggySetCycleState({})", state.dump());
    g_MeridianView->ExecuteJavaScript(_view, js.c_str());
}

void TailorUI::SendModWigs()
{
    if (!g_MeridianView || _view == Meridian::UI::View::INVALID_VIEW_HANDLE) return;

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

    std::string js = std::format("wiggySetModWigs({})", arr.dump());
    g_MeridianView->ExecuteJavaScript(_view, js.c_str());
}

void TailorUI::SendWigBlacklistData()
{
    if (!g_MeridianView || _view == Meridian::UI::View::INVALID_VIEW_HANDLE) return;

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

    std::string js = std::format("wiggySetBlacklistData({})", arr.dump());
    g_MeridianView->ExecuteJavaScript(_view, js.c_str());
}

void TailorUI::SendHairColorState()
{
    if (!g_MeridianView || _view == Meridian::UI::View::INVALID_VIEW_HANDLE) return;

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

    std::string hairJs = std::format("wiggySetHairColor({})", data.dump());
    g_MeridianView->ExecuteJavaScript(_view, hairJs.c_str());
}

void TailorUI::SendWigSituationData()
{
    if (!g_MeridianView || _view == Meridian::UI::View::INVALID_VIEW_HANDLE) return;

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

    std::string js = std::format("wiggySetWigSituationData({})", data.dump());
    g_MeridianView->ExecuteJavaScript(_view, js.c_str());
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
    _blacklist.clear();

    auto path = GetBlacklistPath();
    if (!std::filesystem::exists(path)) {
        logger::info("TailorUI: no outfit blacklist.json found, starting empty");
        return;
    }

    try {
        std::ifstream file(path);
        auto json = nlohmann::json::parse(file);

        if (json.contains("plugins") && json["plugins"].is_array()) {
            for (auto& name : json["plugins"]) {
                _blacklist.insert(name.get<std::string>());
            }
        }

        logger::info("TailorUI: loaded {} outfit-blacklisted plugin(s)", _blacklist.size());
    } catch (const std::exception& e) {
        logger::error("TailorUI: failed to load outfit blacklist: {}", e.what());
    }
}

void TailorUI::SaveBlacklist() const
{
    nlohmann::json json;
    json["version"] = 1;
    json["plugins"] = nlohmann::json::array();
    for (auto& name : _blacklist) {
        json["plugins"].push_back(name);
    }

    try {
        auto path = GetBlacklistPath();
        std::ofstream file(path);
        if (!file.is_open()) {
            logger::error("TailorUI: failed to open outfit blacklist.json for writing");
            return;
        }
        file << json.dump(2);
        file.flush();
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
    if (!g_MeridianView || _view == Meridian::UI::View::INVALID_VIEW_HANDLE) return;

    auto plugins = OutfitStore::GetSingleton().GetArmorPluginNames();

    nlohmann::json arr = nlohmann::json::array();
    for (auto& name : plugins) {
        arr.push_back({
            {"name", name},
            {"blacklisted", IsBlacklisted(name)}
        });
    }

    std::string js = std::format("tailorSetBlacklistData({})", arr.dump());
    g_MeridianView->ExecuteJavaScript(_view, js.c_str());
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
    _wigBlacklist.clear();

    auto path = GetWigBlacklistPath();
    if (!std::filesystem::exists(path)) {
        logger::info("TailorUI: no wig blacklist.json found, starting empty");
        return;
    }

    try {
        std::ifstream file(path);
        auto json = nlohmann::json::parse(file);

        if (json.contains("plugins") && json["plugins"].is_array()) {
            for (auto& name : json["plugins"]) {
                _wigBlacklist.insert(name.get<std::string>());
            }
        }

        logger::info("TailorUI: loaded {} wig-blacklisted plugin(s)", _wigBlacklist.size());
    } catch (const std::exception& e) {
        logger::error("TailorUI: failed to load wig blacklist: {}", e.what());
    }
}

void TailorUI::SaveWigBlacklist() const
{
    nlohmann::json json;
    json["version"] = 1;
    json["plugins"] = nlohmann::json::array();
    for (auto& name : _wigBlacklist) {
        json["plugins"].push_back(name);
    }

    try {
        auto path = GetWigBlacklistPath();
        std::ofstream file(path);
        if (!file.is_open()) {
            logger::error("TailorUI: failed to open wig blacklist.json for writing");
            return;
        }
        file << json.dump(2);
        file.flush();
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
