#pragma once

namespace Tailor::Outfits
{
    struct ArmorEquipmentStatus
    {
        bool worn = false;
        bool compatibleModel = false;
        bool attached = false;
        bool graphVisible = false;
    };

    // Inspects native state only. A visible attached node is not proof that
    // textures, NIF partitions or final rendered pixels are correct.
    ArmorEquipmentStatus InspectArmorEquipment(RE::Actor* actor, RE::TESObjectARMO* armor);
}
