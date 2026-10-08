// Ported from Fitting Room (https://github.com/maartenharms/fitting-room)
//   src/SetDetector.h — verbatim logic, adapted to Tailor.
// Fitting Room is GPL-3.0; this file stays GPL-3.0 as part of Tailor
// (GPL-3.0-or-later).
//
// Adaptations vs the original:
//   - namespace OS::SetDetector -> Tailor::Discovered::SetDetector
//   - Outfit/StyleRefKey/SlotEntry/WeaponClass now come from
//     outfit/discovered/Types.h (minimal pure types)
//   - DetectStyle gains maleOk/femaleOk so the caller can tag each
//     discovered set with Tailor's OutfitSex instead of Fitting Room's
//     player-body fullyFits gate.
#pragma once

#include "outfit/discovered/Types.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Tailor::Discovered::SetDetector {

    // A scanned armor piece flattened to plain data so the algorithm stays
    // engine-free and unit-testable.
    struct DetectStyle {
        std::string   name;
        std::string   source;    // defining plugin filename
        std::string   edid;      // best-effort; empty on runtimes without EDID retention
        std::uint32_t slotMask{ 0 };
        std::uint32_t primaryBit{ 0 };  // the one slot it lists under
        std::uint8_t  armorType{ 0 };   // 0 light, 1 heavy, 2 clothing
        bool          fits{ true };     // has a usable model for at least one sex
        bool          maleOk{ false };   // some race-valid armature has a male model
        bool          femaleOk{ false }; // some race-valid armature has a female model
        StyleRefKey   key;
    };

    // A weapon/ammo entry flattened to engine-free data. Weapons do not
    // participate in armor clustering. Once a coherent armor set exists,
    // LinkWeapons attaches only same-plugin, same-stem looks to it.
    //
    // NOTE (Phase 1): Tailor's outfit model (CustomOutfit/ArmorItem) is
    // armor-only, so DiscoveredOutfits does not scan weapons yet. LinkWeapons
    // is ported for completeness; nothing calls it until a weapon dimension
    // is added to the model.
    struct DetectWeapon {
        std::string   name;
        std::string   source;
        std::string   edid;
        WeaponClass   weaponClass{ WeaponClass::Sword };
        StyleRefKey   key;
    };

    // One of the game's own OUTFIT records (OTFT), flattened to the catalog
    // keys of the armour it names. Leveled lists are expanded by the caller,
    // and anything the catalog dropped is simply absent.
    //
    // THIS EXISTS BECAUSE NO NAMING RULE COULD EVER HAVE FOUND THESE PIECES.
    // The guard outfit record is ArmorGuardCuirassWhiterun plus
    // ArmorStormcloakBoots: the boots are the STORMCLOAK set's boots, sharing
    // not one word with the cuirass. Across the five masters, 495 outfit
    // records are body-anchored with two or more major slots, and 258 of them
    // carry FEET the stem rule cannot reach, 106 HANDS.
    //
    // NO PLUGIN FIELD, DELIBERATELY. The match is the body piece's identity
    // and nothing else: a mod that ships an outfit record pairing its own
    // cuirass with vanilla boots is describing a real set, and a same-plugin
    // rule would refuse exactly that.
    struct DetectOutfit {
        std::vector<StyleRefKey> pieces;  // catalog keys; order is the record's
    };

    struct Options {
        // A small plugin whose pieces have no usable name/EDID stem is treated
        // as one outfit; a large one is dropped (anti-Frankenstein).
        std::size_t              maxResidualPieces{ 8 };
        // Body-slot keys already shipped as authored presets - sets whose body
        // piece matches one are dropped.
        std::vector<StyleRefKey> excludeBodyKeys;
        // How many VARIANT rows a set may add beyond its base: one browser row
        // per variant, one slot at a time, never every combination.
        std::size_t              maxVariantRows{ 8 };
    };

    struct DetectedSet {
        std::string      name;          // human preset name
        std::string      source;        // clean plugin name (browser group header)
        std::string      sourcePlugin;  // raw filename, used for exact weapon matching
        std::string      stem;          // normalized set identity, used for weapon matching
        DiscoveredOutfit outfit;
        bool             fullyFits{ true };  // every representative has a usable model
        int              coverage{ 0 };      // major slots filled (head/body/hands/feet)
        // (primaryBit, count of unused alternates) per filled slot.
        std::vector<std::pair<std::uint32_t, int>> variants;
    };

    // Optional diagnostics filled by Detect - answers "why so few sets?".
    struct Stats {
        int clusters{ 0 };          // total clusters formed (named + residual)
        int residualClusters{ 0 };  // of those, empty-stem (nameless) clusters
        int residualDropped{ 0 };   // residual clusters skipped (plugin too big)
        int qualified{ 0 };         // clusters that produced a set (pre-dedup)
        int deduped{ 0 };           // sets dropped as already owned/authored
        int variantRows{ 0 };       // rows added for an alternate piece
        int variantsDropped{ 0 };   // alternates the per-set cap refused
    };

    // Cluster the styles into coherent single-plugin sets, sorted by
    // (clean plugin name, coverage desc, name). a_stats, if non-null, receives
    // per-run diagnostics.
    [[nodiscard]] std::vector<DetectedSet> Detect(const std::vector<DetectStyle>& a_styles,
                                                  const Options& a_opts,
                                                  Stats* a_stats = nullptr);

    // Fill the EMPTY slots of already-detected sets from the game's own outfit
    // records, and re-sort. a_styles supplies the slot/fit facts for the keys
    // a_outfits names; a key that is not in it is skipped.
    //
    // IT ADDS NO SETS AND REPLACES NO PIECE. Detection from outfit records
    // would put ~495 vanilla records into the browser as rows of their own,
    // most of them things nobody wants to wear; completing what the
    // clusterer already found adds a boot to the guard set and not one row
    // anywhere.
    //
    // THE ANTI-FRANKENSTEIN GUARD IS THE BODY PIECE'S IDENTITY PLUS
    // AGREEMENT: a record contributes only when it names that exact body
    // piece AND every other record naming it offers the same piece for that
    // slot.
    //
    // ONLY FITTING PIECES ARE ADDED. A PIECE MUST NOT CONTEST A SLOT THE SET
    // ALREADY OCCUPIES (whole slot masks are tested, not just primary bits).
    // AND JEWELLERY IS NEVER COMPLETED: an amulet or ring in an outfit
    // record is what that NPC happens to be wearing, never set membership.
    struct Completion {
        enum class Reason {
            kFilled,
            kContested,  // the records naming this body disagreed
            kOverlaps,   // the piece contests a slot the set already occupies
        };
        std::string   setName;
        std::string   pieceName;
        std::uint32_t bit{ 0 };
        Reason        reason{ Reason::kFilled };
    };

    // Returns how many sets gained a slot. a_log collects the fills,
    // a_declined the refusals with their reasons. Reported rather than
    // logged here: this file names no engine or config type.
    std::size_t CompleteFromOutfits(std::vector<DetectedSet>& a_sets,
                                    const std::vector<DetectOutfit>& a_outfits,
                                    const std::vector<DetectStyle>& a_styles,
                                    std::vector<Completion>* a_log = nullptr,
                                    std::vector<Completion>* a_declined = nullptr);

    // Attach weapon/ammo styles whose raw plugin and normalized name stem both
    // match a detected armor set. Every unmatched class remains passthrough.
    void LinkWeapons(std::vector<DetectedSet>& a_sets,
                     const std::vector<DetectWeapon>& a_weapons);

    // The set identity: display name minus slot-nouns and variant tokens.
    [[nodiscard]] std::string NameStem(std::string_view a_displayName);

    // A plugin filename turned into a readable label: no extension/version,
    // camelCase and separators split, title-cased.
    [[nodiscard]] std::string CleanPluginName(std::string_view a_source);

}  // namespace Tailor::Discovered::SetDetector
