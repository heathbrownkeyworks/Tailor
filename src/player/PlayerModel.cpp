#include "player/PlayerModel.h"

namespace Tailor::Player
{
    // Flag the model as changed and run the engine's model update itself, without the
    // NiNodeUpdate event CommonLib's wrapper adds (body-morph events stay with NotifyOutfitChanged).
    void RefreshPlayerModel(RE::Actor* player)
    {
        auto* process = player && player->Is3DLoaded() ? player->GetActorRuntimeData().currentProcess : nullptr;
        if (!process) return;
        process->Set3DUpdateFlag(RE::RESET_3D_FLAGS::kModel);
        using Update3DModel = void(RE::AIProcess*, RE::Actor*);
        static REL::Relocation<Update3DModel> update{ RELOCATION_ID(38404, 39395) };
        update(process, player);
    }
}
