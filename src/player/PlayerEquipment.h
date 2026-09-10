#pragma once

#include "player/PlayerEquipmentData.h"

namespace Tailor::Player
{
    class Equipment
    {
    public:
        static Equipment& GetSingleton();
        bool BeginPreview(Channel channel);
        bool EndPreview(Channel channel, bool confirm);
        bool Apply(Channel channel, const std::vector<RE::TESForm*>& items);
        bool RestoreBaseline(Channel channel, bool forget = false);
        void RefreshModels();
        bool IsPreviewing() const { return _preview[0].has_value() || _preview[1].has_value(); }
        void Clear(); // load/revert: forget runtime state without touching the new world
        const std::array<EquipmentState, 2>& GetState() const { return _state; }
        void SetState(std::array<EquipmentState, 2> state) { _state = std::move(state); }

    private:
        struct Preview
        {
            EquipmentState original;
            EquipmentSnapshot worn;
        };
        EquipmentSnapshot Capture();
        bool Restore(Channel channel, const EquipmentSnapshot& snapshot);
        bool Affects(Channel channel, RE::TESObjectARMO* armor) const;
        bool Prune(Channel channel, const std::vector<Instance>& keep);
        void FinishChange();
        std::array<EquipmentState, 2> _state;
        std::array<std::optional<Preview>, 2> _preview;
        bool _changing = false;
    };
}
