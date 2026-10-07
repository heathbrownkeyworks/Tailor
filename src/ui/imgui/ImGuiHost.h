#pragma once

#include <nlohmann/json.hpp>
#include <memory>
#include <string>

namespace Tailor::ImGuiUI
{
    // Menu/lifecycle entry points run on the game thread. Only DrawFrame touches
    // ImGui; input and published state cross that boundary through locked queues.
    class ImGuiHost final : public RE::BSTEventSink<RE::InputEvent*>
    {
    public:
        static ImGuiHost& GetSingleton();
        void Initialize();
        bool CanOpen() const;
        bool RequestOpen();
        void RequestClose();
        [[nodiscard]] bool HasFocus() const;
        [[nodiscard]] bool WantsTextInput() const;
        void Publish(std::string topic, nlohmann::json value);
        void DrawFrame();
        void OnShown();
        void OnHidden();
        RE::BSEventNotifyControl ProcessEvent(RE::InputEvent* const*,
            RE::BSTEventSource<RE::InputEvent*>*) override;

    private:
        ImGuiHost();
        ~ImGuiHost() override;
        struct State;
        std::unique_ptr<State> _state;
        bool InitializeRenderer();
        void RestoreGameInput();
        void SyncTextInput(std::uint64_t token);
        void QueueLifecycleClose();
    };
}
