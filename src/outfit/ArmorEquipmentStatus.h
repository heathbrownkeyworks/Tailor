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

    // False when the armor has a model for some body but none that shows on this race
    // and sex: worn, it would only hide the body it covers (a female-only cuirass on a
    // male, for one). A female body shows the male model when hers is blank, as vanilla
    // shields rely on. Armor with no model at all, like most rings, fits anyone.
    bool FitsBody(RE::TESObjectARMO* armor, RE::TESRace* race, RE::SEX sex);
    // FitsBody for the actor's current race and sex.
    bool FitsActorBody(RE::Actor* actor, RE::TESObjectARMO* armor);
}
