#include "PapyrusBridge.h"

namespace
{
    bool DispatchActorBoolMethod(RE::Actor* actor, const char* methodName, bool value)
    {
        if (!actor) return false;

        auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
        if (!vm) {
            logger::warn("PapyrusBridge: VM not available for Actor.{}", methodName);
            return false;
        }

        auto* policy = vm->GetObjectHandlePolicy();
        if (!policy) {
            logger::warn("PapyrusBridge: no handle policy for Actor.{}", methodName);
            return false;
        }

        RE::VMHandle handle = policy->GetHandleForObject(
            static_cast<RE::VMTypeID>(RE::FormType::ActorCharacter), actor);
        if (handle == policy->EmptyHandle()) {
            logger::warn("PapyrusBridge: empty VM handle for actor {:08X}", actor->GetFormID());
            return false;
        }

        auto args = RE::MakeFunctionArguments(std::move(value));
        RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> callback;
        bool ok = vm->DispatchMethodCall(
            handle,
            RE::BSFixedString("Actor"),
            RE::BSFixedString(methodName),
            args,
            callback);
        if (!ok) {
            logger::warn("PapyrusBridge: DispatchMethodCall Actor.{}({}) failed", methodName, value);
        }
        return ok;
    }
}

bool PapyrusBridge::SetActorRestrained(RE::Actor* actor, bool restrained)
{
    return DispatchActorBoolMethod(actor, "SetRestrained", restrained);
}

bool PapyrusBridge::SetActorDontMove(RE::Actor* actor, bool dontMove)
{
    return DispatchActorBoolMethod(actor, "SetDontMove", dontMove);
}
