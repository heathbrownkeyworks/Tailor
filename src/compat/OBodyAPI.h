#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

namespace RE
{
    class Actor;
    class TESForm;
}

namespace OBody::API
{
    using Actor = RE::Actor;
    using TESForm = RE::TESForm;

    enum class PluginAPIVersion
    {
        Invalid = 0,
        v1 = 1,
        Latest = v1
    };

    class IActorChangeEventListener;

    struct PresetCounts
    {
        std::uint32_t female;
        std::uint32_t femaleBlacklisted;
        std::uint32_t male;
        std::uint32_t maleBlacklisted;
    };

    enum PresetCategory : std::uint64_t
    {
        PresetCategoryNone = 0,
        PresetCategoryFemale = 1 << 0,
        PresetCategoryFemaleBlacklisted = 1 << 1,
        PresetCategoryMale = 1 << 2,
        PresetCategoryMaleBlacklisted = 1 << 3
    };

    struct PresetAssignmentInformation
    {
        enum Flags : std::uint64_t
        {
            None = 0,
            IsFemale = 1 << 0
        };

        Flags flags = Flags::None;
        std::string_view presetName;
    };

    struct AssignPresetPayload
    {
        enum Flags : std::uint64_t
        {
            None = 0,
            ForceImmediateApplicationOfMorphs = 1 << 0,
            DoNotApplyMorphs = 1 << 1
        };

        Flags flags = Flags::ForceImmediateApplicationOfMorphs;
        std::string_view presetName;
    };

    class IPluginInterfaceVersionIndependent
    {
    public:
        const char* owner;
        void*       context = nullptr;

        virtual PluginAPIVersion PluginAPIVersion() = 0;
        virtual const char* SetOwner(const char* owner) = 0;
    };

    class IPluginInterface : public IPluginInterfaceVersionIndependent
    {
    public:
        virtual bool ActorIsNaked(Actor* actor) = 0;
        virtual bool ActorIsNaked(Actor* actor, bool actorIsEquippingArmor, const TESForm* armor) = 0;
        virtual bool ActorHasORefitApplied(Actor* actor) = 0;
        virtual bool ActorIsProcessed(Actor* actor) = 0;
        virtual bool ActorIsBlacklisted(Actor* actor) = 0;
        virtual bool IsORefitEnabled() = 0;

        virtual bool RegisterEventListener(IActorChangeEventListener& eventListener) = 0;
        virtual bool DeregisterEventListener(IActorChangeEventListener& eventListener) = 0;
        virtual bool HasRegisteredEventListener(IActorChangeEventListener& eventListener) = 0;

        virtual void GetPresetCounts(PresetCounts& payload) = 0;
        virtual std::size_t GetPresetNames(
            PresetCategory category,
            std::string_view* buffer,
            std::size_t bufferLength,
            std::size_t offset = 0,
            std::size_t limit = (std::numeric_limits<std::size_t>::max)()) = 0;

        virtual void EnsureActorIsProcessed(Actor* actor) = 0;
        virtual void ApplyOBodyMorphsToActor(Actor* actor) = 0;
        virtual void RemoveOBodyMorphsFromActor(Actor* actor) = 0;
        virtual void ForcefullyChangeORefitForActor(Actor* actor, bool orefitShouldBeApplied) = 0;
        virtual void GetPresetAssignedToActor(Actor* actor, PresetAssignmentInformation& payload) = 0;
        virtual bool AssignPresetToActor(Actor* actor, AssignPresetPayload& payload) = 0;
    };

    class IActorChangeEventListener
    {
    public:
        struct OnActorGenerated
        {
            struct Payload
            {
                IPluginInterfaceVersionIndependent* responsiblePluginInterface;
                const std::string_view presetName;
            };
            enum Flags : std::uint64_t
            {
                None = 0,
                IsClothed = 1 << 0,
                IsORefitApplied = 1 << 1,
                IsORefitEnabled = 1 << 2
            };
            enum class Response : std::uint64_t { None = 0 };
        };

        virtual OnActorGenerated::Response OnActorGenerated(
            Actor*, OnActorGenerated::Flags, OnActorGenerated::Payload&)
        {
            return OnActorGenerated::Response::None;
        }

        struct OnActorPresetChangedWithoutGeneration
        {
            struct Payload
            {
                IPluginInterfaceVersionIndependent* responsiblePluginInterface;
                const std::string_view presetName;
            };
            enum Flags : std::uint64_t
            {
                None = 0,
                PresetWasUnassigned = 1 << 0
            };
            enum class Response : std::uint64_t { None = 0 };
        };

        virtual OnActorPresetChangedWithoutGeneration::Response OnActorPresetChangedWithoutGeneration(
            Actor*,
            OnActorPresetChangedWithoutGeneration::Flags,
            OnActorPresetChangedWithoutGeneration::Payload&)
        {
            return OnActorPresetChangedWithoutGeneration::Response::None;
        }

        struct OnActorClothingUpdate
        {
            struct Payload
            {
                IPluginInterfaceVersionIndependent* responsiblePluginInterface;
                const TESForm* changedEquipment;
            };
            enum Flags : std::uint64_t
            {
                None = 0,
                IsClothed = 1 << 0,
                IsORefitApplied = 1 << 1,
                IsORefitEnabled = 1 << 2,
                IsProcessed = 1 << 3,
                IsBlacklisted = 1 << 4,
                ActorIsEquipping = 1 << 5
            };
            enum class Response : std::uint64_t { None = 0 };
        };

        virtual OnActorClothingUpdate::Response OnActorClothingUpdate(
            Actor*, OnActorClothingUpdate::Flags, OnActorClothingUpdate::Payload&)
        {
            return OnActorClothingUpdate::Response::None;
        }

        struct OnORefitForcefullyChanged
        {
            struct Payload
            {
                IPluginInterfaceVersionIndependent* responsiblePluginInterface;
            };
            enum Flags : std::uint64_t
            {
                None = 0,
                IsORefitApplied = 1 << 1,
                IsORefitEnabled = 1 << 2
            };
            enum class Response : std::uint64_t { None = 0 };
        };

        virtual OnORefitForcefullyChanged::Response OnORefitForcefullyChanged(
            Actor*, OnORefitForcefullyChanged::Flags, OnORefitForcefullyChanged::Payload&)
        {
            return OnORefitForcefullyChanged::Response::None;
        }

        struct OnActorMorphsCleared
        {
            struct Payload
            {
                IPluginInterfaceVersionIndependent* responsiblePluginInterface;
            };
            enum Flags : std::uint64_t { None = 0 };
            enum class Response : std::uint64_t { None = 0 };
        };

        virtual OnActorMorphsCleared::Response OnActorMorphsCleared(
            Actor*, OnActorMorphsCleared::Flags, OnActorMorphsCleared::Payload&)
        {
            return OnActorMorphsCleared::Response::None;
        }
    };

    class IOBodyReadinessEventListener
    {
    public:
        virtual void OBodyIsReady() = 0;
        virtual void OBodyIsNoLongerReady() = 0;
        virtual void OBodyIsBecomingReady() {}
        virtual void OBodyIsBecomingUnready() {}
    };

    namespace SKSEMessages
    {
        struct RequestPluginInterface
        {
            static constexpr std::uint32_t type = 0xC0B0D9CC;

            PluginAPIVersion version;
            IPluginInterface** pluginInterface;
            IOBodyReadinessEventListener* readinessEventListener;
        };
    }
}

static_assert(sizeof(OBody::API::SKSEMessages::RequestPluginInterface) == sizeof(void*) * 3);
