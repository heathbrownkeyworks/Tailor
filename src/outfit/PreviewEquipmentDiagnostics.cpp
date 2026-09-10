#include "outfit/PreviewEquipmentDiagnostics.h"

namespace Tailor::Outfits
{
    namespace
    {
        using Json = nlohmann::json;
        constexpr std::size_t kMaxEntries = 256;

        std::string FormId(const RE::TESForm* form)
        {
            return std::format("{:08X}", form ? form->GetFormID() : 0);
        }

        Json DescribeNode(RE::NiAVObject* node)
        {
            if (!node) return nullptr;
            auto* parent = node->parent;
            return {
                {"address", std::format("{}", static_cast<const void*>(node))},
                {"name", node->name.c_str()},
                {"flags", node->GetFlags().underlying()},
                {"culled", node->GetAppCulled()},
                {"parent", std::format("{}", static_cast<const void*>(parent))},
                {"parentName", parent ? parent->name.c_str() : ""}
            };
        }
    }

    void LogPreviewEquipment(RE::Actor* actor, std::string_view phase, std::uint64_t generation)
    {
        if (!actor) return;
        try {
            auto* npc = actor->GetActorBase();
            auto* inventory = actor->GetInventoryChanges(true);  // no initialization
            auto* root = actor->Get3D(false);
            Json snapshot{
                {"build", "headwear-diag-20260910"}, {"phase", phase},
                {"generation", generation}, {"actor", FormId(actor)},
                {"name", actor->GetDisplayFullName()},
                {"race", FormId(npc ? npc->GetRace() : nullptr)},
                {"sex", npc ? static_cast<int>(npc->GetSex()) : -1},
                {"defaultOutfit", FormId(npc ? npc->defaultOutfit : nullptr)},
                {"sleepOutfit", FormId(npc ? npc->sleepOutfit : nullptr)},
                {"inventoryAvailable", inventory != nullptr},
                {"root", DescribeNode(root)},
                {"baseInventory", Json::array()}, {"inventory", Json::array()},
                {"biped", Json::array()}, {"geometry", Json::array()},
                {"armorRecords", Json::object()}, {"truncated", false}
            };
            auto recordArmor = [&](RE::TESForm* form) {
                auto* armor = form ? form->As<RE::TESObjectARMO>() : nullptr;
                if (!armor || snapshot["armorRecords"].contains(FormId(armor))) return;
                Json addons = Json::array();
                for (auto* addon : armor->armorAddons) {
                    if (!addon) continue;
                    addons.push_back({
                        {"id", FormId(addon)}, {"slots", addon->GetSlotMask().underlying()},
                        {"validRace", npc && npc->GetRace() && addon->IsValidRace(npc->GetRace())},
                        {"maleModel", addon->bipedModels[0].GetModel()},
                        {"femaleModel", addon->bipedModels[1].GetModel()}
                    });
                }
                auto* file = armor->GetFile(0);
                snapshot["armorRecords"][FormId(armor)] = {
                    {"name", armor->GetName()}, {"plugin", file ? file->GetFilename() : ""},
                    {"slots", armor->GetSlotMask().underlying()}, {"addons", std::move(addons)}
                };
            };
            // Keep records for missing outfit entries too, so an entirely
            // rejected piece can still be traced to its plugin and model.
            if (npc && npc->defaultOutfit) {
                for (auto* item : npc->defaultOutfit->outfitItems) recordArmor(item);
            }
            if (auto* container = actor->GetContainer()) {
                container->ForEachContainerObject([&](RE::ContainerObject& entry) {
                    if (entry.obj && entry.obj->Is(RE::FormType::Armor)) {
                        if (snapshot["baseInventory"].size() >= kMaxEntries) {
                            snapshot["truncated"] = true;
                            return RE::BSContainer::ForEachResult::kStop;
                        }
                        snapshot["baseInventory"].push_back({{"item", FormId(entry.obj)}, {"count", entry.count}});
                        recordArmor(entry.obj);
                    }
                    return RE::BSContainer::ForEachResult::kContinue;
                });
            }
            if (inventory && inventory->entryList) {
                for (auto* entry : *inventory->entryList) {
                    if (!entry || !entry->object || !entry->object->Is(RE::FormType::Armor)) continue;
                    if (snapshot["inventory"].size() >= kMaxEntries) {
                        snapshot["truncated"] = true;
                        break;
                    }
                    Json instances = Json::array();
                    if (entry->extraLists) {
                        for (auto* extra : *entry->extraLists) {
                            if (!extra) continue;
                            if (instances.size() >= kMaxEntries) {
                                snapshot["truncated"] = true;
                                break;
                            }
                            auto* marker = extra->GetByType<RE::ExtraOutfitItem>();
                            instances.push_back({
                                {"address", std::format("{}", static_cast<const void*>(extra))},
                                {"count", extra->GetCount()}, {"worn", extra->GetWorn()},
                                {"cannotWear", extra->HasType<RE::ExtraCannotWear>()},
                                {"outfitMarker", marker ? Json(std::format("{:08X}", marker->id)) : Json(nullptr)}
                            });
                        }
                    }
                    // Include ordinary/unmarked entries and negative deltas too.
                    snapshot["inventory"].push_back({
                        {"item", FormId(entry->object)}, {"countDelta", entry->countDelta},
                        {"instances", std::move(instances)}
                    });
                    recordArmor(entry->object);
                }
            }
            const auto& biped = actor->GetBiped(false);
            snapshot["bipedAvailable"] = static_cast<bool>(biped);
            if (biped) {
                for (bool buffered : {false, true}) {
                    const auto* objects = buffered ? biped->bufferedObjects : biped->objects;
                    for (std::uint32_t i = 0; i < RE::BIPED_OBJECTS::kTotal; ++i) {
                        const auto& part = objects[i];
                        if (!part.item && !part.addon && !part.partClone) continue;
                        snapshot["biped"].push_back({
                            {"buffered", buffered}, {"index", i},
                            {"armorSlot", i < RE::BIPED_OBJECTS::kEditorTotal ? Json(i + 30) : Json(nullptr)},
                            {"item", FormId(part.item)}, {"addon", FormId(part.addon)},
                            {"model", part.part ? part.part->GetModel() : ""},
                            {"clone", DescribeNode(part.partClone.get())}
                        });
                        recordArmor(part.item);
                    }
                }
            }
            if (root) {
                RE::BSVisit::TraverseScenegraphGeometries(root, [&](RE::BSGeometry* geometry) {
                    if (snapshot["geometry"].size() >= kMaxEntries) {
                        snapshot["truncated"] = true;
                        return RE::BSVisit::BSVisitControl::kStop;
                    }
                    // Also capture meshes which no longer have a biped entry.
                    snapshot["geometry"].push_back(DescribeNode(geometry));
                    return RE::BSVisit::BSVisitControl::kContinue;
                });
            }
            logger::info("PreviewDiag {}", snapshot.dump(-1, ' ', false, Json::error_handler_t::replace));
        } catch (const std::exception& error) {
            logger::warn("PreviewDiag: snapshot failed at {}: {}", phase, error.what());
        }
    }
}
