// Part of Tailor (GPL-3.0-or-later).
//
// Scan + generate for auto-discovered outfit sets. The scan is adapted from
// Fitting Room's StyleCatalog::Build (hygiene filters, slot masks, armor
// types) and the orchestration from its AutoPresets::Generate (detect,
// complete-from-outfits, build). Differences from Fitting Room:
//   - No variant collapsing of enchanted re-issues (Tailor-side simplification;
//     the detector's own variant rows still surface alternates).
//   - No PreviewRenderer thumbnails (Phase 2); Tailor's Dressing Room is the
//     preview.
//   - Sex TAGGING per set (Male/Female/Unisex) instead of Fitting Room's
//     player-body fullyFits drop: Tailor dresses NPCs of either sex and its
//     existing OutfitFits() gate filters at preview time.
//   - Weapons are not scanned in Phase 1: Tailor's CustomOutfit/ArmorItem
//     model is armor-only.
#include "outfit/DiscoveredOutfits.h"

#include "outfit/Utf8.h"
#include "outfit/discovered/SetDetector.h"

#include <algorithm>
#include <unordered_map>

namespace Tailor::Discovered {

    namespace
    {
        // Fitting Room's DetectStyle.armorType: 0 light, 1 heavy, 2 clothing.
        std::uint8_t ArmorTypeByte(RE::BGSBipedObjectForm::ArmorType a_type)
        {
            using AT = RE::BGSBipedObjectForm::ArmorType;
            switch (a_type) {
            case AT::kHeavyArmor: return 1;
            case AT::kClothing:   return 2;
            case AT::kLightArmor:
            default:              return 0;
            }
        }

        // Does any race-valid armature of this armor carry a model for the
        // given sex slot? Mirrors Fitting Room's EffectiveBipedModel rule:
        // the engine falls back female -> male, but for TAGGING we ask the
        // strict question per sex (no fallback), so a female-only piece is
        // genuinely tagged Female.
        bool HasModelForSex(RE::TESObjectARMO* a_armo,
                           const std::vector<RE::TESRace*>& a_races,
                           RE::SEXES::SEX a_sex)
        {
            for (auto* arma : a_armo->armorAddons) {
                if (!arma) {
                    continue;
                }
                bool raceOk = false;
                for (auto* race : a_races) {
                    if (arma->IsValidRace(race)) {
                        raceOk = true;
                        break;
                    }
                    // The CK's Armor Race (RNAM): what the engine resolves an
                    // armature through. One level only, like Fitting Room.
                    if (auto* parent = race->armorParentRace;
                        parent && parent != race && arma->IsValidRace(parent)) {
                        raceOk = true;
                        break;
                    }
                }
                if (!raceOk) {
                    continue;
                }
                const char* path = arma->bipedModels[a_sex].GetModel();
                if (path && *path) {
                    return true;
                }
            }
            return false;
        }
    }

    DiscoveredOutfits& DiscoveredOutfits::GetSingleton()
    {
        static DiscoveredOutfits singleton;
        return singleton;
    }

    void DiscoveredOutfits::Regenerate()
    {
        auto* dh = RE::TESDataHandler::GetSingleton();
        if (!dh) {
            logger::warn("DiscoveredOutfits: no data handler; nothing discovered");
            return;
        }

        // Playable races for the IsValidRace gate (filters child gear,
        // creature armor, NPC-locked refits).
        std::vector<RE::TESRace*> playableRaces;
        for (auto* race : dh->GetFormArray<RE::TESRace>()) {
            if (race && race->GetPlayable()) {
                playableRaces.push_back(race);
            }
        }

        // ---- Stage A: scan every ARMO into plain DetectStyle rows ----
        std::vector<SetDetector::DetectStyle> styles;
        std::vector<RE::TESObjectARMO*>       forms;  // parallel to styles
        for (auto* armo : dh->GetFormArray<RE::TESObjectARMO>()) {
            if (!armo) {
                continue;
            }
            // Must be playable...
            if ((armo->formFlags & RE::TESObjectARMO::RecordFlags::kNonPlayable) != 0) {
                continue;
            }
            // ...named...
            const char* name = armo->GetFullName();
            if (!name || !*name) {
                continue;
            }
            // ...have armor addons (this also drops the naked-body skin in
            // practice: skins carry no display name and are filtered above)...
            if (armo->armorAddons.empty()) {
                continue;
            }
            // ...and sit on a styleable slot.
            const std::uint32_t mask = armo->GetSlotMask().underlying();
            if (mask == 0) {
                continue;
            }
            auto* file = armo->GetFile(0);
            if (!file) {
                continue;
            }

            const bool maleOk   = HasModelForSex(armo, playableRaces, RE::SEXES::kMale);
            const bool femaleOk = HasModelForSex(armo, playableRaces, RE::SEXES::kFemale);
            if (!maleOk && !femaleOk) {
                continue;  // no race-valid armature with a model for either sex
            }

            SetDetector::DetectStyle s;
            s.name       = SanitizeUtf8(name);
            s.source     = file->GetFilename();
            if (const char* ed = armo->GetFormEditorID(); ed && *ed) {
                s.edid = ed;  // best-effort corroborating stem for set detection
            }
            s.slotMask   = mask;
            s.primaryBit = PrimaryBitFor(mask);
            s.armorType  = ArmorTypeByte(armo->GetArmorType());
            s.fits       = true;
            s.maleOk     = maleOk;
            s.femaleOk   = femaleOk;
            s.key        = { s.source, armo->GetLocalFormID() };
            styles.push_back(std::move(s));
            forms.push_back(armo);
        }

        // ---- Stage B: flatten the game's own OUTFIT records ----
        // One level of leveled-list expansion, no recursion (Fitting Room's
        // measured rule: guard helmets are LVLI, deeper nesting is noise).
        std::unordered_map<RE::FormID, StyleRefKey> keyByForm;
        keyByForm.reserve(styles.size() * 2);
        for (std::size_t i = 0; i < styles.size(); ++i) {
            keyByForm.emplace(forms[i]->GetFormID(), styles[i].key);
        }
        std::vector<SetDetector::DetectOutfit> outfits;
        for (auto* record : dh->GetFormArray<RE::BGSOutfit>()) {
            if (!record) {
                continue;
            }
            SetDetector::DetectOutfit flat;
            const auto take = [&](RE::TESForm* a_form) {
                if (!a_form) {
                    return;
                }
                if (const auto it = keyByForm.find(a_form->GetFormID());
                    it != keyByForm.end()) {
                    flat.pieces.push_back(it->second);
                }
            };
            for (auto* item : record->outfitItems) {
                if (!item) {
                    continue;
                }
                if (auto* list = item->As<RE::TESLevItem>()) {
                    for (std::uint32_t i = 0; i < list->numEntries; ++i) {
                        take(list->entries[i].form);
                    }
                    continue;
                }
                take(item);
            }
            if (flat.pieces.size() > 1) {
                outfits.push_back(std::move(flat));
            }
        }

        // ---- Stage C: detect, complete, build ----
        SetDetector::Options opts;  // defaults: residual cap 8, variant rows 8
        SetDetector::Stats   stats;
        auto sets = SetDetector::Detect(styles, opts, &stats);
        const auto completed =
            SetDetector::CompleteFromOutfits(sets, outfits, styles);

        std::unordered_map<StyleRefKey, const SetDetector::DetectStyle*, StyleRefKeyHash> byKey;
        byKey.reserve(styles.size() * 2);
        for (const auto& s : styles) {
            byKey.emplace(s.key, &s);
        }

        std::vector<CustomOutfit> built;
        built.reserve(sets.size());
        int nextId = -1;  // negative ids: never collide with OutfitStore's 1, 2, ...
        for (auto& set : sets) {
            CustomOutfit outfit;
            outfit.id   = nextId--;
            outfit.name = set.name;

            // Sex tag from the pieces: Female when every piece has a female
            // model and at least one piece lacks a male model (and mirror for
            // Male); otherwise Unisex. Tailor's OutfitFits() enforces it at
            // preview time.
            bool allMaleOk = true, allFemaleOk = true;
            set.outfit.ForEachStyle([&](std::uint32_t, const StyleRefKey& a_key) {
                const auto it = byKey.find(a_key);
                if (it == byKey.end()) {
                    return;
                }
                ArmorItem item;
                item.formId = a_key.localFormID;
                item.plugin = a_key.modName;
                item.name   = it->second->name;
                outfit.items.push_back(std::move(item));
                if (!it->second->maleOk) {
                    allMaleOk = false;
                }
                if (!it->second->femaleOk) {
                    allFemaleOk = false;
                }
            });
            if (allFemaleOk && !allMaleOk) {
                outfit.sex = OutfitSex::Female;
            } else if (allMaleOk && !allFemaleOk) {
                outfit.sex = OutfitSex::Male;
            } else {
                outfit.sex = OutfitSex::Unisex;
            }

            built.push_back(std::move(outfit));
        }

        {
            std::lock_guard lock(_mutex);
            _outfits = std::move(built);
        }
        logger::info("DiscoveredOutfits: {} set(s) from {} scanned piece(s); "
                     "Detect: {} clusters, {} qualified, {} deduped, {} variant row(s); "
                     "outfit records completed {} set(s)",
                     _outfits.size(), styles.size(),
                     stats.clusters, stats.qualified, stats.deduped, stats.variantRows,
                     completed);
    }

    std::vector<CustomOutfit> DiscoveredOutfits::Snapshot() const
    {
        std::lock_guard lock(_mutex);
        return _outfits;
    }

    std::optional<CustomOutfit> DiscoveredOutfits::GetById(int id) const
    {
        std::lock_guard lock(_mutex);
        for (const auto& outfit : _outfits) {
            if (outfit.id == id) {
                return outfit;
            }
        }
        return std::nullopt;
    }

    std::size_t DiscoveredOutfits::Count() const
    {
        std::lock_guard lock(_mutex);
        return _outfits.size();
    }

}  // namespace Tailor::Discovered
