// Ported from Fitting Room (https://github.com/maartenharms/fitting-room)
//   src/Outfit.h  (StyleRefKey, SlotEntry)
//   src/SlotMask.h (slot-bit helpers)
//   src/WeaponSlots.h (WeaponClass)
// Fitting Room is GPL-3.0; this file stays GPL-3.0 as part of Tailor
// (GPL-3.0-or-later). Only the pieces SetDetector needs are carried over —
// the dye/head-part/weapon-hand machinery of Fitting Room's Outfit is not.
#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

namespace Tailor::Discovered {

    // A style piece, stored load-order-independently: plugin filename plus the
    // form's LOCAL id. Resolved to a TESObjectARMO* only by the caller, through
    // RE::TESDataHandler::LookupForm.
    struct StyleRefKey {
        std::string   modName;
        std::uint32_t localFormID{ 0 };

        [[nodiscard]] bool Empty() const { return modName.empty() && localFormID == 0; }
        friend bool operator==(const StyleRefKey&, const StyleRefKey&) = default;
    };

    struct StyleRefKeyHash {
        std::size_t operator()(const StyleRefKey& a_k) const noexcept {
            std::size_t h1 = std::hash<std::string>{}(a_k.modName);
            std::size_t h2 = std::hash<std::uint32_t>{}(a_k.localFormID);
            return h1 ^ (h2 + 0x9e3779b9u + (h1 << 6) + (h1 >> 2));
        }
    };

    struct SlotEntry {
        enum class Kind : std::uint8_t { kPassthrough = 0, kStyle = 1, kHide = 2 };
        Kind        kind{ Kind::kPassthrough };
        StyleRefKey style;
    };

    // Weapon classes, mirroring Fitting Room's WeaponSlots.h. An unrelated
    // index space from the armor slot bits below.
    enum class WeaponClass : std::uint8_t {
        Sword,
        Dagger,
        WarAxe,
        Mace,
        Greatsword,
        BattleaxeWarhammer,
        Bow,
        Crossbow,
        Staff,
        Arrows,
        Bolts,
        kTotal,
    };

    inline constexpr std::size_t kWeaponClassCount =
        static_cast<std::size_t>(WeaponClass::kTotal);

    // ---- Slot bits ------------------------------------------------------------
    // CommonLib's BipedObjectSlot is a bitmask: kHead(1<<0) is editor slot 30.
    // bit = editorSlot - 30.

    inline constexpr std::uint32_t kBitCount = 32;

    constexpr std::uint32_t BitForEditorSlot(std::uint32_t a_editorSlot) {
        return a_editorSlot - 30u;  // precondition: a_editorSlot >= 30
    }
    constexpr std::uint32_t MaskForEditorSlot(std::uint32_t a_editorSlot) {
        return 1u << BitForEditorSlot(a_editorSlot);  // precondition: slot in [30,61]
    }

    inline constexpr std::uint32_t kBitBody     = BitForEditorSlot(32);
    inline constexpr std::uint32_t kBitAmulet   = BitForEditorSlot(35);
    inline constexpr std::uint32_t kBitRing     = BitForEditorSlot(36);
    // The community's slot 52 (SOS/TNG). A GENERATED set must never own it:
    // claiming it would strip another mod's cover the moment the set is worn.
    inline constexpr std::uint32_t kBitGenitals = BitForEditorSlot(52);

    // Which ONE slot a multi-slot garment lists under. The body wins when it
    // is claimed at all; everything else keeps the lowest bit.
    [[nodiscard]] constexpr std::uint32_t PrimaryBitFor(std::uint32_t a_slotMask) {
        if (a_slotMask == 0) {
            return 0;
        }
        if ((a_slotMask & (1u << kBitBody)) != 0) {
            return kBitBody;
        }
        return static_cast<std::uint32_t>(std::countr_zero(a_slotMask));
    }

    // ---- Minimal outfit -------------------------------------------------------
    // The exact API surface Fitting Room's SetDetector uses, and nothing more.
    // Tailor's own CustomOutfit (id/name/items/sex) is what the UI and the
    // store speak; this type exists only while the detector runs.
    class DiscoveredOutfit {
    public:
        std::string name;

        void SetStyle(std::uint32_t a_bit, StyleRefKey a_key) {
            if (a_bit >= kBitCount) {
                return;
            }
            entries_[a_bit] = { SlotEntry::Kind::kStyle, std::move(a_key) };
        }

        [[nodiscard]] const SlotEntry& EntryFor(std::uint32_t a_bit) const {
            static const SlotEntry kNone{};
            return a_bit < kBitCount ? entries_[a_bit] : kNone;
        }

        template <class F>
        void ForEachStyle(F&& a_fn) const {
            for (std::uint32_t b = 0; b < kBitCount; ++b) {
                if (entries_[b].kind == SlotEntry::Kind::kStyle) {
                    a_fn(b, entries_[b].style);
                }
            }
        }

        void SetWeaponStyle(WeaponClass a_class, StyleRefKey a_key) {
            const auto i = static_cast<std::size_t>(a_class);
            if (i < kWeaponClassCount) {
                weaponEntries_[i] = { SlotEntry::Kind::kStyle, std::move(a_key) };
            }
        }

        [[nodiscard]] const SlotEntry& WeaponEntryFor(WeaponClass a_class) const {
            static const SlotEntry kNone{};
            const auto             i = static_cast<std::size_t>(a_class);
            return i < kWeaponClassCount ? weaponEntries_[i] : kNone;
        }

        template <class F>
        void ForEachWeaponStyle(F&& a_fn) const {
            for (std::size_t i = 0; i < kWeaponClassCount; ++i) {
                if (weaponEntries_[i].kind == SlotEntry::Kind::kStyle) {
                    a_fn(static_cast<WeaponClass>(i), weaponEntries_[i].style);
                }
            }
        }

    private:
        std::array<SlotEntry, kBitCount>         entries_{};
        std::array<SlotEntry, kWeaponClassCount>  weaponEntries_{};
    };

}  // namespace Tailor::Discovered
