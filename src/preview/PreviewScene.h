#pragma once

#include "preview/VisibilityLedger.h"
#include <array>
#include <unordered_map>

namespace Tailor::Preview
{
    class PreviewScene
    {
    public:
        bool Begin(RE::Actor* actor);
        bool Tick(RE::Actor* actor, const RE::NiPoint3& camera,
            const RE::NiPoint3& approach, const RE::NiPoint3& center,
            float distance, float fov, std::int64_t now, bool appearanceChanged);
        void End() noexcept;
    private:
        bool Fit(const RE::NiPoint3& camera, const RE::NiPoint3& approach,
            const RE::NiPoint3& center, float distance, float fov);
        bool Protected(const RE::NiAVObject* node) const;
        void HideBranch(RE::NiAVObject* node);
        void KeepActorVisible(RE::NiAVObject* node);
        void Sweep(RE::Actor* actor);
        void HideWorldFeeders();

        RE::NiPointer<RE::ShadowSceneNode> _scene;
        RE::NiPointer<RE::NiNode> _stageParent;
        RE::NiPointer<RE::NiNode> _stage;
        RE::NiPointer<RE::NiAVObject> _wall;
        RE::NiPointer<RE::NiAVObject> _actorRoot;
        std::array<RE::NiPointer<RE::NiPointLight>, 2> _lights;
        VisibilityLedger<RE::NiPointer<RE::NiAVObject>> _hidden;
        std::unordered_map<RE::NiAVObject*, RE::NiPointer<RE::NiAVObject>> _alwaysDraw;
        std::int64_t _nextSweep{0};
        std::uint64_t _worldFeederReculls{0};
        RE::TESObjectCELL* _cell{nullptr}; // identity only, session ends on cell change
        bool _logged{false};
    };
}
