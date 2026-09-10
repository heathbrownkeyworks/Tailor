#pragma once

namespace Tailor::Outfits
{
    bool UnequipArmorInstance(RE::Actor* actor, RE::TESBoundObject* object, RE::ExtraDataList* extra);
    // Only exact engine-owned instances of these two scratch outfits qualify.
    // Retire their captured mesh clones before replacement can reuse slot metadata.
    bool UnequipPreviewItems(RE::Actor* actor, RE::FormID primary, RE::FormID alternate);
    bool HasPreviewItems(RE::Actor* actor, RE::FormID primary, RE::FormID alternate);
    bool IsOutfitInstance(RE::Actor* actor, RE::TESBoundObject* object,
        RE::ExtraDataList* extra, RE::FormID outfit);
}
