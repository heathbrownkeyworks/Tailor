#include "ui/imgui/InputDispatchHook.h"
#include "ui/imgui/ImGuiHost.h"
#include "ui/imgui/KeyboardOwnership.h"
#include "keyhandler/keyhandler.h"

#include <atomic>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace Tailor::ImGuiUI
{
    namespace
    {
        using Sink = RE::BSTEventSink<RE::InputEvent*>;
        bool forwarding = false;  // input is polled and dispatched on the main thread only
        std::atomic<bool> inputOwned{false};
        std::uint32_t toggleKey = 0;
        std::function<bool()> togglePredicate;
        std::optional<bool> polledHotkey;

        struct HotkeyPoll
        {
            std::optional<bool> previous = std::exchange(polledHotkey,
                togglePredicate && togglePredicate());
            ~HotkeyPoll() { polledHotkey = previous; }
        };

        struct DispatchInput
        {
            static void thunk(RE::BSTEventSource<RE::InputEvent*>* source, RE::InputEvent* const* events)
            {
                struct Link { RE::InputEvent* event; RE::InputEvent* next; bool hide; };
                static input::KeyboardOwnership ownership;
                static std::vector<Link> links;
                auto& host = ImGuiHost::GetSingleton();
                const bool owned = host.HasFocus();
                inputOwned.store(owned);  // every poll, even an empty one, so release is prompt
                if (!events || !*events) { func(source, events); return; }
                const HotkeyPoll poll;
                bool anyHidden = false;
                links.clear();
                for (auto* event = *events; event; event = event->next) {
                    bool hide = false;
                    if (event->eventType == RE::INPUT_EVENT_TYPE::kChar) hide = input::KeyboardOwnership::HideCharacter(owned);
                    else if (event->eventType == RE::INPUT_EVENT_TYPE::kButton) {
                        const auto* button = event->AsButtonEvent();
                        if (button && button->GetDevice() == RE::INPUT_DEVICE::kKeyboard) {
                            using Phase = input::KeyboardOwnership::Phase;
                            const bool shortcut = button->GetIDCode() == toggleKey && IsToggleHotkeyActive();
                            hide = ownership.HideKey(owned || shortcut, button->GetIDCode(), button->IsDown() ? Phase::Down : button->IsUp() ? Phase::Up : Phase::Held);
                        }
                    }
                    anyHidden |= hide;
                    links.push_back({event, event->next, hide});
                }
                if (!anyHidden) { func(source, events); return; }
                // Tailor reads the whole batch first. Everyone else then walks the same
                // events with the hidden ones unlinked; the chain is restored afterwards
                // because the engine still owns these events.
                host.ProcessEvent(events, source);
                static_cast<Sink*>(KeyHandler::GetSingleton())->ProcessEvent(events, source);
                RE::InputEvent* head = nullptr;
                RE::InputEvent* tail = nullptr;
                for (const auto& link : links) if (!link.hide) {
                    (tail ? tail->next : head) = link.event;
                    tail = link.event;
                }
                if (tail) tail->next = nullptr;
                RE::InputEvent* const visible[]{head};
                forwarding = true;
                func(source, visible);
                forwarding = false;
                for (const auto& link : links) link.event->next = link.next;
            }
            static inline REL::Relocation<decltype(thunk)> func;
        };
    }

    bool IsForwardingInput() { return forwarding; }
    bool IsInputOwned() { return inputOwned.load(); }

    void ConfigureToggleHotkey(std::uint32_t key, std::function<bool()> predicate)
    {
        toggleKey = key;
        togglePredicate = std::move(predicate);
    }

    bool IsToggleHotkeyActive()
    {
        if (polledHotkey.has_value()) return *polledHotkey;
        return togglePredicate && togglePredicate();
    }

    bool InstallInputDispatchHook()
    {
        static std::once_flag once;
        static bool installed = false;
        std::call_once(once, [] {
            // BSInputDeviceManager::PollInputDevices' call that sends the polled batch to
            // every input listener. Keep any existing call hook in the chain.
            const auto call = REL::RelocationID(67315, 68617).address() + REL::Relocate(0x7B, 0x7B, 0x81);
            if (*reinterpret_cast<const std::uint8_t*>(call) != 0xE8) {
                logger::warn("Tailor input dispatch call is unsupported on this runtime; other mods' hotkeys stay live while typing");
                return;
            }
            DispatchInput::func = SKSE::GetTrampoline().write_call<5>(call, DispatchInput::thunk);
            installed = true;
            logger::info("Tailor input dispatch guard installed");
        });
        return installed;
    }
}
