#include "preview/PreviewScene.h"

#include <algorithm>
#include <cmath>

// Live-world isolation informed by Menu Studio's Declutter/StudioRig code:
// https://github.com/maartenharms/menu-studio/tree/8b64be319916223f7bc42700a48ec01ce190b9aa
// See THIRD-PARTY-NOTICES.md for attribution.
namespace Tailor::Preview
{
    namespace
    {
        bool AncestorOrSelf(const RE::NiAVObject* ancestor, const RE::NiAVObject* node)
        {
            for (auto* current = node; current; current = current->parent) {
                if (current == ancestor) return true;
            }
            return false;
        }
        RE::ShadowSceneNode* SceneOf(RE::NiAVObject* root)
        {
            for (auto* node = root; node; node = node->parent) {
                if (auto* scene = netimmerse_cast<RE::ShadowSceneNode*>(node)) return scene;
            }
            return nullptr;
        }
        void UpdateNode(RE::NiAVObject* node)
        {
            RE::NiUpdateData update{};
            update.flags.set(RE::NiUpdateData::Flag::kDirty, RE::NiUpdateData::Flag::kDisableCollision);
            node->Update(update);
        }
    }

    bool PreviewScene::Begin(RE::Actor* actor)
    {
        _actorRoot.reset(actor ? actor->Get3D(false) : nullptr);
        _scene.reset(SceneOf(_actorRoot.get()));
        _stageParent.reset(_actorRoot ? _actorRoot->parent : nullptr);
        _cell = actor ? actor->GetParentCell() : nullptr;
        if (!_actorRoot || !_scene || !_stageParent || !_cell) return false;

        // This small static occluder is the only model we load. The NPC stays
        // attached to its original Skyrim skeleton, shaders and skin instances.
        RE::NiPointer<RE::NiNode> model;
        RE::BSModelDB::DBTraits::ArgsType args{};
        const auto result = RE::BSModelDB::Demand("Tailor\\Preview\\tailor_brown_plane.nif", model, args);
        if (result != RE::BSResource::ErrorCode::kNone || !model) {
            logger::warn("Tailor brown backdrop unavailable ({})", static_cast<unsigned>(result));
            return false;
        }
        auto* clone = model->Clone();
        _wall.reset(clone ? clone->AsNode() : nullptr);
        if (!_wall) return false;
        // Preserve the engine-initialized shader, including its fade owner.
        // AE lighting passes read that owner's currentFade even for this
        // NoFade/NiNode backdrop; replacing it with a null-owner property
        // causes a null dereference in the backdrop's lighting pass.
        _stage.reset(RE::NiNode::Create(3));
        if (!_stage) return false;
        _stage->name = "TAILOR_LivePreviewStage";
        _stage->AttachChild(_wall.get(), true);
        // Place the backdrop beside the actor on the existing object/room
        // branch, as Menu Studio does. The shadow-scene node owns lighting and
        // portal routing; attaching there alone does not put the stage on the
        // actor's rendered branch.
        _stageParent->AttachChild(_stage.get(), true);
        KeepActorVisible(_stage.get());

        for (std::size_t i = 0; i < _lights.size(); ++i) {
            auto& light = _lights[i];
            light.reset(RE::NiPointLight::Create());
            if (!light) return false;
            light->name = i == 0 ? "TAILOR_PreviewKey" : "TAILOR_PreviewFill";
            auto& data = light->GetLightRuntimeData();
            data.ambient = {0.0f, 0.0f, 0.0f};
            data.diffuse = i == 0 ? RE::NiColor{0.8f, 0.76f, 0.7f} : RE::NiColor{0.38f, 0.42f, 0.5f};
            data.fade = 1.0f;
            data.radius = {600.0f, 600.0f, 600.0f};
            light->SetLightAttenuation(600.0f);
            _stage->AttachChild(light.get(), true);
            RE::ShadowSceneNode::LIGHT_CREATE_PARAMS params{};
            params.dynamic = true;
            params.neverFades = true;
            params.falloff = 1.0f;
            params.nearDistance = 5.0f;
            params.depthBias = 1.0f;
            if (!_scene->AddLight(light.get(), params)) return false;
        }
        _nextSweep = 0;
        _worldFeederReculls = 0;
        _logged = false;
        return true;
    }

    bool PreviewScene::Protected(const RE::NiAVObject* node) const
    {
        return AncestorOrSelf(node, _actorRoot.get()) || AncestorOrSelf(_actorRoot.get(), node) ||
            AncestorOrSelf(node, _stage.get()) || AncestorOrSelf(_stage.get(), node) ||
            netimmerse_cast<const RE::NiLight*>(node);
    }

    void PreviewScene::HideBranch(RE::NiAVObject* node)
    {
        if (!node) return;
        if (AncestorOrSelf(_actorRoot.get(), node) || AncestorOrSelf(_stage.get(), node) ||
            netimmerse_cast<RE::NiLight*>(node)) return;
        if (Protected(node)) {
            _hidden.Release(node);
        } else {
            if (node->GetAppCulled() && !_hidden.Owns(node)) return;
            _hidden.Hide(node);
        }
        // Room/portal rendering can visit descendants directly, bypassing a
        // parent's app-cull flag. Hide every off-target descendant as well.
        if (auto* branch = node->AsNode()) {
            for (auto& child : branch->GetChildren()) HideBranch(child.get());
        }
    }

    void PreviewScene::KeepActorVisible(RE::NiAVObject* node)
    {
        if (!node) return;
        _hidden.Release(node);
        using Flag = RE::NiAVObject::Flag;
        if (!node->GetFlags().any(Flag::kAlwaysDraw)) {
            _alwaysDraw.try_emplace(node, node);
            node->GetFlags().set(Flag::kAlwaysDraw);
        }
        // Keep authored per-part culls (hidden hair/body partitions) intact.
        if (auto* branch = node->AsNode()) {
            for (auto& child : branch->GetChildren()) KeepActorVisible(child.get());
        }
    }

    void PreviewScene::Sweep(RE::Actor* actor)
    {
        HideBranch(_scene.get());
        // Portal shared geometry and exterior terrain have additional entry
        // points into rendering; a sweep of placed references cannot reach them.
        if (auto* graph = _scene->GetRuntimeData().portalGraph) {
            HideBranch(graph->portalSharedNode.get());
        }
        const auto hideLand = [&](RE::TESObjectCELL* cell) {
            if (!cell) return;
            auto* land = cell->GetRuntimeData().cellLand;
            if (land && land->loadedData) {
                for (auto* mesh : land->loadedData->mesh) HideBranch(mesh);
            }
        };
        hideLand(actor->GetParentCell());
        if (auto* tes = RE::TES::GetSingleton()) {
            if (auto* grid = tes->gridCells) {
                for (std::uint32_t x = 0; x < grid->length; ++x) {
                    for (std::uint32_t y = 0; y < grid->length; ++y) hideLand(grid->GetCell(x, y));
                }
            }
            HideBranch(tes->lodLandRoot);
            HideBranch(tes->objLODWaterRoot);
        }
        if (auto* grass = RE::BGSGrassManager::GetSingleton()) HideBranch(grass->grassNode.get());
        if (auto* sky = RE::Sky::GetSingleton(); sky && sky->precip) {
            HideBranch(sky->precip->currentPrecip.get());
            HideBranch(sky->precip->lastPrecip.get());
        }
        if (auto* water = RE::TESWaterSystem::GetSingleton()) {
            for (auto& object : water->waterObjects) {
                if (object) HideBranch(object->shape.get());
            }
        }
        // First- and third-person player roots are distinct; retain the exact
        // nodes in the ledger instead of restoring through ambiguous Get3D().
        if (auto* player = RE::PlayerCharacter::GetSingleton()) {
            HideBranch(player->Get3D(false));
            HideBranch(player->Get3D(true));
        }
        KeepActorVisible(_actorRoot.get());
        if (!_logged) {
            logger::info("Tailor live scene isolated: target={:08X}, hidden nodes={}, actor/stage draw nodes={}",
                actor->GetFormID(), _hidden.Size(), _alwaysDraw.size());
            _logged = true;
        }
    }

    void PreviewScene::HideWorldFeeders()
    {
        // Sky/Weather can be siblings ABOVE the actor's ShadowSceneNode.
        // Skyrim re-enables them during unpaused updates (Menu Studio's
        // ReassertWorldFeederCulls handles the same sky-flash condition).
        // Check each tick, including branches that were already hidden when
        // the preview opened. The ledger still restores only culls we own.
        const auto hide = [&](RE::NiAVObject* node) {
            if (!node) return;
            if (_hidden.Owns(node) && !node->GetAppCulled()) ++_worldFeederReculls;
            HideBranch(node);
        };
        if (auto* sky = RE::Sky::GetSingleton()) hide(sky->root.get());
        for (auto* parent = _actorRoot->parent; parent; parent = parent->parent) {
            for (auto& child : parent->GetChildren()) {
                if (child && (child->name == "Sky" || child->name == "Weather" || child->name == "LODRoot")) {
                    hide(child.get());
                }
            }
        }
    }

    bool PreviewScene::Fit(const RE::NiPoint3& camera, const RE::NiPoint3& approach,
        const RE::NiPoint3& center, float distance, float fov)
    {
        if (!_stage || !_wall || !_stageParent || _stage->parent != _stageParent.get()) return false;
        const auto size = RE::BSGraphics::Renderer::GetScreenSize();
        const float aspect = static_cast<float>(size.width) / (std::max)(1.0f, static_cast<float>(size.height));
        const float tangent = std::tan(RE::deg_to_rad(fov * 0.5f));
        const float depth = distance + 350.0f;
        const RE::NiPoint3 forward = approach * -1.0f;
        const RE::NiPoint3 right{forward.y, -forward.x, 0.0f};
        const RE::NiPoint3 wallCenter = camera + forward * depth;
        const auto& parent = _stageParent->world;
        if (!std::isfinite(parent.scale) || std::abs(parent.scale) < 0.0001f) return false;
        const auto inverse = parent.rotate.Transpose();
        const float inverseScale = 1.0f / parent.scale;
        // Leave the static backdrop's engine-owned fade state intact. Update
        // its transform and the lights only on framing/parent changes instead
        // of dirtying the entire stage on every CursorMenu advance.
        bool placementChanged = !_logged;
        const auto place = [&](RE::NiAVObject* node, const RE::NiTransform& transform) {
            if (node->local != transform) {
                node->local = transform;
                placementChanged = true;
            }
        };
        RE::NiTransform stageTransform;
        stageTransform.rotate = inverse;
        stageTransform.scale = inverseScale;
        stageTransform.translate = (inverse * (wallCenter - parent.translate)) * inverseScale;
        place(_stage.get(), stageTransform);
        RE::NiTransform wallTransform;
        wallTransform.rotate.EulerAnglesToAxesZXY(0.0f, 0.0f, std::atan2(approach.x, approach.y));
        wallTransform.scale = (std::max)(depth * tangent / 130.0f, depth * tangent / aspect / 100.0f) * 1.3f;
        place(_wall.get(), wallTransform);
        if (_wall->GetAppCulled()) _wall->SetAppCulled(false);
        for (std::size_t i = 0; i < _lights.size(); ++i) {
            auto& light = _lights[i];
            if (!light) continue;
            const auto position = center + approach * 160.0f + right * (i == 0 ? -100.0f : 120.0f) +
                RE::NiPoint3{0.0f, 0.0f, i == 0 ? 80.0f : 25.0f};
            auto lightTransform = light->local;
            lightTransform.translate = position - wallCenter;
            place(light.get(), lightTransform);
        }
        if (placementChanged) {
            UpdateNode(_stage.get());
        }
        for (auto& light : _lights) {
            if (!light) continue;
            light->worldBound.center = light->world.translate;
            light->worldBound.radius = 600.0f;
        }
        if (!_logged) {
            logger::info("Tailor backdrop attached: parent='{}' type={}, rootType={}, depth={}, scale={}, boundRadius={}",
                _stageParent->name.empty() ? "<unnamed>" : _stageParent->name.c_str(),
                _stageParent->GetRTTI()->GetName(), _wall->GetRTTI()->GetName(),
                depth, _wall->world.scale, _wall->worldBound.radius);
            std::size_t geometries = 0;
            RE::BSVisit::TraverseScenegraphGeometries(_wall.get(), [&](RE::BSGeometry* geometry) {
                ++geometries;
                auto* shader = geometry->lightingShaderProp_cast();
                const auto emission = shader && shader->emissiveColor ? *shader->emissiveColor : RE::NiColor{};
                logger::info("Tailor backdrop geometry: type={}, culled={}, boundRadius={}, lighting={}, alpha={}, emission=({},{},{}), multiplier={}, fadeOwner={}",
                    geometry->GetRTTI()->GetName(), geometry->GetAppCulled(), geometry->worldBound.radius,
                    shader != nullptr, shader ? shader->alpha : -1.0f,
                    emission.red, emission.green, emission.blue, shader ? shader->emissiveMult : -1.0f,
                    shader && shader->fadeNode != nullptr);
                return RE::BSVisit::BSVisitControl::kContinue;
            });
            if (!geometries) logger::warn("Tailor backdrop has no renderable geometry after model load");
        }
        return true;
    }

    bool PreviewScene::Tick(RE::Actor* actor, const RE::NiPoint3& camera,
        const RE::NiPoint3& approach, const RE::NiPoint3& center,
        float distance, float fov, std::int64_t now, bool appearanceChanged)
    {
        auto* root = actor ? actor->Get3D(false) : nullptr;
        if (!root || actor->GetParentCell() != _cell || SceneOf(root) != _scene.get()) return false;
        const bool rebuilt = root != _actorRoot.get();
        _actorRoot.reset(root);
        if (!Fit(camera, approach, center, distance, fov)) return false;
        // Leave scene/sky fog to Skyrim. The frame probe found scene fog reset
        // before every tick; CursorMenu must not compete with those updates.
        HideWorldFeeders();
        _hidden.Reassert([&](const RE::NiAVObject* node) { return Protected(node); });
        if (rebuilt || appearanceChanged || now >= _nextSweep) {
            Sweep(actor);
            _nextSweep = now + 250;
        }
        return true;
    }

    void PreviewScene::End() noexcept
    {
        if (_logged) logger::info("Tailor preview background: world feeder visibility reasserted {} time(s)", _worldFeederReculls);
        _hidden.Restore();
        for (auto& [node, hold] : _alwaysDraw) node->GetFlags().reset(RE::NiAVObject::Flag::kAlwaysDraw);
        _alwaysDraw.clear();
        for (auto& light : _lights) {
            if (light && _scene) {
                light->SetAppCulled(true);
                _scene->RemoveLight(light.get());
            }
            light.reset();
        }
        if (_stage && _stage->parent) _stage->parent->DetachChild(_stage.get());
        _wall.reset();
        _stage.reset();
        _stageParent.reset();
        _actorRoot.reset();
        _scene.reset();
        _cell = nullptr;
        _nextSweep = 0;
    }
}
