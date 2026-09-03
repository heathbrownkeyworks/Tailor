#pragma once

class CellHandler : public RE::BSTEventSink<RE::TESCellAttachDetachEvent>,
                    public RE::BSTEventSink<RE::TESCellFullyLoadedEvent>
{
public:
    static CellHandler* GetSingleton();
    static void         Register();
    static void QueueOutfitReEquip(RE::ActorHandle actorHandle, int32_t delayMs = 0);
    static void InvalidatePendingOutfitTasks();

private:
    CellHandler() = default;

    RE::BSEventNotifyControl ProcessEvent(
        const RE::TESCellAttachDetachEvent*                 event,
        RE::BSTEventSource<RE::TESCellAttachDetachEvent>*) override;

    RE::BSEventNotifyControl ProcessEvent(
        const RE::TESCellFullyLoadedEvent*                  event,
        RE::BSTEventSource<RE::TESCellFullyLoadedEvent>*) override;
};
