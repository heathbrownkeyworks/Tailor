#include "ui/imgui/ImGuiHost.h"
#include "ui/imgui/InputDispatchHook.h"
#include "ui/imgui/InputMap.h"
#include "ui/imgui/ControllerInput.h"
#include "ui/imgui/FrameRecovery.h"
#include "ui/imgui/TailorMenu.h"
#include "ui/imgui/TailorScreen.h"
#include "ui/TailorUI.h"
#include "Settings.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <backends/imgui_impl_dx11.h>
#include <backends/imgui_impl_win32.h>
#include "RE/C/CharEvent.h"
#include "RE/C/CursorMenu.h"
#include "RE/D/DeviceConnectEvent.h"
#include "RE/M/MenuCursor.h"
#include "RE/M/MouseMoveEvent.h"
#include "RE/R/Renderer.h"
#include "RE/T/ThumbstickEvent.h"
#include <atomic>
#include <cmath>
#include <mutex>
#include <set>

namespace Tailor::ImGuiUI
{
    namespace
    {
        // Other native ImGui mods may leave their context current at PostDisplay.
        struct ContextScope
        {
            ImGuiContext* previous = ImGui::GetCurrentContext();
            ~ContextScope() { ImGui::SetCurrentContext(previous); }
        };
    }

    struct ImGuiHost::State
    {
        struct Input
        {
            enum class Kind { Key, Button, Wheel, Char } kind;
            int code = 0;
            bool down = false;
            float wheel = 0;
        };
        std::atomic<bool> active{false}, desired{false}, clearInput{true}, wantsText{false}, facadeReady{false};
        std::atomic<bool> closeQueued{false};
        std::atomic<std::uint64_t> token{0};
        std::atomic<std::uint32_t> gamepadHeld{0};
        std::atomic<bool> usingGamepad{false}, cursorMode{false};
        std::atomic<float> leftX{0}, leftY{0}, rightX{0}, rightY{0};
        std::atomic<bool> controllerArmed{false};
        std::atomic<HWND> window{nullptr};
        bool registered = false, closing = false, shown = false, textAllowed = false;
        bool ready = false;
        bool controllerEnabled = false;
        input::ControllerBindings bindings = input::DefaultControllerBindings;
        Model bindingNames = Model::object();
        std::string glyphs = "xbox";
        RE::GPtr<RE::GFxMovieView> hud;
        bool hudVisible = false;
        std::mutex inputLock, modelLock;
        std::vector<Input> pendingInput;
        Model model = Model::object();
        ImGuiContext* context = nullptr;
        Fonts fonts;
        ScreenState screen;
        std::uint64_t renderToken = 0, generation = 0, sequence = 0;
        Rect viewport;
        float yaw = 0;
        bool hairMode = false, sentViewport = false, cursorClickHeld = false;
        // Dear ImGui's error messages logged since Tailor opened (InitializeRenderer's ErrorCallback). The callback runs
        // inside DrawFrame, on the render thread, which also clears this at each open.
        std::set<std::string> imguiErrors;
    };

    ImGuiHost::ImGuiHost() : _state(std::make_unique<State>()) {}
    // The renderer owns the device lifetime. Do not call backend shutdown from
    // static destruction after Skyrim has already destroyed its D3D device.
    ImGuiHost::~ImGuiHost() = default;
    ImGuiHost& ImGuiHost::GetSingleton() { static ImGuiHost host; return host; }

    void ImGuiHost::Initialize()
    {
        auto& s = *_state;
        if (s.registered) return;
        auto* inputManager = RE::BSInputDeviceManager::GetSingleton();
        if (!inputManager || !TailorMenu::Register()) return;
        const auto& settings = Settings::GetSingleton();
        s.controllerEnabled = settings.GetControllerEnabled();
        s.bindings = settings.GetControllerBindings();
        s.glyphs = settings.GetControllerGlyphs();
        for (std::size_t i = 0; i < input::ControllerActions.size(); ++i)
            s.bindingNames[input::ControllerActions[i].name] = settings.GetControllerBindingNames()[i];
        inputManager->AddEventSink(this);
        s.registered = true;
        logger::info("Tailor native unpaused menu and input registered");
    }

    bool ImGuiHost::CanOpen() const
    {
        const auto& s = *_state;
        if (!s.registered || s.closing) return false;
        if (s.desired) return true;
        auto* queue = RE::UIMessageQueue::GetSingleton();
        auto* ui = RE::UI::GetSingleton();
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!queue || !ui || !player || !player->Is3DLoaded() || ui->GameIsPaused() ||
            ui->IsMenuOpen(RE::MainMenu::MENU_NAME) || ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME)) return false;
        return true;
    }

    bool ImGuiHost::RequestOpen()
    {
        auto& s = *_state;
        if (!CanOpen()) return false;
        if (s.desired) return true;
        ++s.token;
        s.closeQueued = false;
        s.desired = true;
        s.facadeReady = false;
        s.cursorMode = false;
        s.controllerArmed = false;
        s.clearInput = true;
        RE::UIMessageQueue::GetSingleton()->AddMessage(TailorMenu::MENU_NAME, RE::UI_MESSAGE_TYPE::kShow, nullptr);
        return true;
    }

    void ImGuiHost::RestoreGameInput()
    {
        auto& s = *_state;
        if (auto* map = RE::ControlMap::GetSingleton()) {
            if (s.textAllowed) map->AllowTextInput(false);
        }
        s.textAllowed = false;
        if (s.hud) s.hud->SetVisible(s.hudVisible);
        s.hud = nullptr;
    }

    // Text entry is allowed only while a text field has focus. The game switches wheel zoom off
    // during text entry, and the preview's free camera saves the controls when it starts and
    // restores them when it ends. Held for the whole session, text entry was released inside that
    // window, and wheel zoom came back switched off.
    void ImGuiHost::SyncTextInput(std::uint64_t token)
    {
        auto& s = *_state;
        if (s.token.load() != token || !s.active) return;
        const bool wants = s.wantsText.load();
        if (wants == s.textAllowed) return;
        if (auto* map = RE::ControlMap::GetSingleton()) {
            map->AllowTextInput(wants);
            s.textAllowed = wants;
        }
    }

    void ImGuiHost::RequestClose()
    {
        auto& s = *_state;
        const bool hadRequest = s.desired.exchange(false);
        s.active = false;
        s.facadeReady = false;
        s.wantsText = false;
        s.clearInput = true;
        RestoreGameInput();
        if (s.closing || (!hadRequest && !s.shown)) return;
        ++s.token; // cancels the queued shown callback and any input actions
        if (auto* queue = RE::UIMessageQueue::GetSingleton()) {
            s.closing = true;
            queue->AddMessage(TailorMenu::MENU_NAME, RE::UI_MESSAGE_TYPE::kHide, nullptr);
        }
    }

    bool ImGuiHost::HasFocus() const
    {
        const auto& s = *_state;
        const auto window = s.window.load();
        if (!s.active.load() || !s.desired.load() || (window && GetForegroundWindow() != window)) return false;
        auto* ui = RE::UI::GetSingleton();
        return ui && !ui->GameIsPaused() && ui->IsMenuOpen(TailorMenu::MENU_NAME);
    }
    bool ImGuiHost::WantsTextInput() const { return _state->wantsText.load(); }

    void ImGuiHost::OnShown()
    {
        auto& s = *_state;
        if (s.shown) return; // repeated show messages must not acquire input twice
        s.shown = true;
        if (!s.desired || s.closing) { RequestClose(); return; }
        const auto token = s.token.load();
        // Gameplay isolation belongs to this IMenu's input context. Global
        // enabled/stored control bits have no owner identity: restoring a
        // snapshot could undo another mod's concurrent control disable.
        // Text entry waits for a focused field (SyncTextInput).
        if (auto* ui = RE::UI::GetSingleton()) {
            if (auto hud = ui->GetMenu(RE::HUDMenu::MENU_NAME); hud && hud->uiMovie) {
                s.hud = hud->uiMovie;
                s.hudVisible = s.hud->GetVisible();
                s.hud->SetVisible(false);
            }
        }
        s.active = true;
        s.clearInput = true;
        SKSE::GetTaskInterface()->AddTask([this, token] {
            auto& current = *_state;
            if (current.token == token && current.desired && current.active) {
                TailorUI::GetSingleton().OnNativeMenuShown();
                current.facadeReady = current.token == token && TailorUI::GetSingleton().IsOpen();
                if (!current.facadeReady && current.token == token && current.desired)
                    QueueLifecycleClose();
            }
        });
    }

    void ImGuiHost::QueueLifecycleClose()
    {
        auto& s = *_state;
        if (s.closeQueued.exchange(true)) return;
        const auto token = s.token.load();
        SKSE::GetTaskInterface()->AddTask([this, token] {
            if (_state->token == token)
                TailorUI::GetSingleton().CloseForLifecycle(Tailor::Preview::EndReason::FocusLost);
        });
    }

    void ImGuiHost::OnHidden()
    {
        auto& s = *_state;
        const bool unexpected = s.desired.load();
        s.active = false;
        s.facadeReady = false;
        s.desired = false;
        s.shown = false;
        s.closing = false;
        s.wantsText = false;
        s.clearInput = true;
        RestoreGameInput();
        if (unexpected) QueueLifecycleClose();
    }

    void ImGuiHost::Publish(std::string topic, nlohmann::json value)
    {
        auto& s = *_state;
        std::scoped_lock lock(s.modelLock);
        auto& versions = s.model["_versions"];
        if (!versions.is_object()) versions = Model::object();
        versions[topic] = versions.value(topic, std::uint64_t{0}) + 1;
        s.model[topic] = std::move(value);
        s.model["_revision"] = s.model.value("_revision", std::uint64_t{0}) + 1;
    }

    bool ImGuiHost::InitializeRenderer()
    {
        auto& s = *_state;
        if (s.ready) return true;
        auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
        if (!renderer) return false;
        auto& data = renderer->GetRuntimeData();
        auto* device = reinterpret_cast<ID3D11Device*>(data.forwarder);
        auto* context = reinterpret_cast<ID3D11DeviceContext*>(data.context);
        auto* swapchain = reinterpret_cast<IDXGISwapChain*>(data.renderWindows[0].swapChain);
        if (!device || !context || !swapchain) return false;
        DXGI_SWAP_CHAIN_DESC desc{};
        if (FAILED(swapchain->GetDesc(&desc)) || !desc.OutputWindow) return false;
        IMGUI_CHECKVERSION();
        s.context = ImGui::CreateContext();
        ImGui::SetCurrentContext(s.context);
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NoMouseCursorChange;
        io.MouseDrawCursor = false;
        // Two widgets sharing an ID is a programmer error. The screen tests hover every
        // page to catch one; players must never be shown Dear ImGui's diagnostic for it.
        io.ConfigDebugHighlightIdConflicts = false;
        // A frame whose drawing throws is unwound and closes Tailor (DrawFrame). Players never see Dear ImGui's
        // diagnostics, so its assert and error tooltip are off, and its errors go to Tailor.log instead: each distinct
        // one once per open, since an error Dear ImGui recovers from on its own comes back every frame.
        io.ConfigErrorRecoveryEnableAssert = false;
        io.ConfigErrorRecoveryEnableTooltip = false;
        s.context->ErrorCallbackUserData = &s.imguiErrors;
        s.context->ErrorCallback = [](ImGuiContext*, void* seen, const char* message) {
            if (static_cast<std::set<std::string>*>(seen)->insert(message).second) logger::warn("Dear ImGui: {}", message);
        };
        s.context->ConfigNavWindowingWithGamepad = false; // X belongs to Assign.
        if (!LoadFonts(io, "Data/SKSE/Plugins/Tailor/fonts", s.fonts))
            logger::warn("Tailor native fonts missing; using available fallback fonts");
        if (!ImGui_ImplWin32_Init(desc.OutputWindow)) {
            ImGui::DestroyContext(s.context); s.context = nullptr; s.fonts = {}; return false;
        }
        if (!ImGui_ImplDX11_Init(device, context)) {
            ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext(s.context); s.context = nullptr; s.fonts = {}; return false;
        }
        s.window = desc.OutputWindow;
        s.ready = true;
        logger::info("Tailor native ImGui {} initialized", IMGUI_VERSION);
        return true;
    }

    void ImGuiHost::DrawFrame()
    {
        auto& s = *_state;
        if (!s.active || !s.facadeReady) return;
        ContextScope scope;
        if (!HasFocus()) { QueueLifecycleClose(); return; }
        if (!InitializeRenderer()) {
            logger::error("Tailor native renderer unavailable; closing menu and restoring preview/input");
            QueueLifecycleClose();
            return;
        }
        ImGui::SetCurrentContext(s.context);
        auto& io = ImGui::GetIO();
        Model model;
        { std::scoped_lock lock(s.modelLock); model = s.model; }
        const bool usingGamepad = s.controllerEnabled && s.usingGamepad.load();
        const bool cursorMode = usingGamepad && s.cursorMode.load();
        model["_usingGamepad"] = usingGamepad;
        model["_controllerCursor"] = cursorMode;
        model["_controllerGlyphs"] = s.glyphs;
        model["_controllerBindings"] = s.bindingNames;
        const auto generation = model.value("tailorSetPreviewOpenGeneration", std::uint64_t{0});
        const auto token = s.token.load();
        if (s.renderToken != token || s.generation != generation) {
            s.screen = {};
            s.renderToken = token;
            s.generation = generation;
            s.sequence = 0;
            s.yaw = 0;
            s.sentViewport = false;
            s.controllerArmed = false;
            s.clearInput = true;
            s.imguiErrors.clear();
        }
        if (s.clearInput.exchange(false)) {
            io.ClearInputKeys(); io.ClearInputMouse();
            io.ClearEventsQueue();
            s.cursorClickHeld = false;
            std::scoped_lock lock(s.inputLock); s.pendingInput.clear();
        }
        // Nothing still held from opening Tailor (the button that cast the power, a Favorites press) or
        // from switching target reaches the newly focused widgets: the controller waits until every
        // button is released.
        if (!s.gamepadHeld.load()) s.controllerArmed = true;
        const bool controllerInput = usingGamepad && s.controllerArmed.load();
        // Cursor mode still needs native gamepad Cancel for editors/popups.
        // SubmitControllerKeys suppresses focus movement and duplicate Accept.
        if (controllerInput) io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
        else io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
        if (usingGamepad && !cursorMode) io.ConfigFlags |= ImGuiConfigFlags_NoMouse;
        else io.ConfigFlags &= ~ImGuiConfigFlags_NoMouse;
        ImGui_ImplDX11_NewFrame();
        const int firstBackendEvent = s.context->InputEventsQueue.Size;
        ImGui_ImplWin32_NewFrame();
        input::DiscardBackendControllerEvents(*s.context, firstBackendEvent);
        if (controllerInput) io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
        else io.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;
        const float leftX = input::ControllerAxis(s.leftX.load()), leftY = input::ControllerAxis(s.leftY.load());
        const float rightX = input::ControllerAxis(s.rightX.load()), rightY = input::ControllerAxis(s.rightY.load());
        model["_controllerRightX"] = controllerInput && !cursorMode ? rightX : 0.0f;
        model["_controllerRightY"] = controllerInput && !cursorMode ? rightY : 0.0f;
        input::SubmitControllerKeys(s.gamepadHeld.load(), leftX, leftY, rightX, rightY, s.bindings,
            controllerInput, cursorMode, s.screen.controllerRotating, [&](ImGuiKey key, float value) {
                io.AddKeyAnalogEvent(key, value > 0, value);
            });
        if (auto* cursor = RE::MenuCursor::GetSingleton()) {
            auto& position = cursor->GetRuntimeData();
            if (controllerInput && cursorMode) {
                const float speed = 900.0f * io.DisplaySize.y / 1080.0f * std::min(io.DeltaTime, 0.05f);
                position.cursorPosX = std::clamp(position.cursorPosX + leftX * speed, 0.0f, std::max(0.0f, io.DisplaySize.x - 1));
                position.cursorPosY = std::clamp(position.cursorPosY - leftY * speed, 0.0f, std::max(0.0f, io.DisplaySize.y - 1));
            }
            io.AddMousePosEvent(position.cursorPosX, position.cursorPosY);
        }
        const bool cursorClick = controllerInput && cursorMode && (s.gamepadHeld.load() & s.bindings[0]);
        if (cursorClick != s.cursorClickHeld) {
            io.AddMouseButtonEvent(0, cursorClick);
            s.cursorClickHeld = cursorClick;
        }
        io.AddKeyEvent(ImGuiMod_Ctrl, (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0);
        io.AddKeyEvent(ImGuiMod_Shift, (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0);
        io.AddKeyEvent(ImGuiMod_Alt, (GetAsyncKeyState(VK_MENU) & 0x8000) != 0);
        std::vector<State::Input> inputs;
        { std::scoped_lock lock(s.inputLock); inputs.swap(s.pendingInput); }
        for (const auto& event : inputs) {
            switch (event.kind) {
            case State::Input::Kind::Key: io.AddKeyEvent(static_cast<ImGuiKey>(event.code), event.down); break;
            case State::Input::Kind::Button: io.AddMouseButtonEvent(event.code, event.down); break;
            case State::Input::Kind::Wheel: io.AddMouseWheelEvent(0, event.wheel); break;
            case State::Input::Kind::Char: io.AddInputCharacter(static_cast<unsigned>(event.code)); break;
            }
        }
        ImGui::NewFrame();
        FrameResult result;
        const auto failure = DrawRecovering([&] { result = DrawTailor(model, s.screen, s.fonts, generation != 0 && HasFocus()); });
        if (s.wantsText.exchange(io.WantTextInput) != io.WantTextInput)
            SKSE::GetTaskInterface()->AddTask([this, token] { SyncTextInput(token); });
        ImGui::Render();
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        if (failure) {
            // The screen threw part-way through the frame. Its windows and styles are unwound and the frame ended, so
            // nothing carries into the next open. Its actions, viewport and yaw never leave (a default yaw would snap
            // the preview). Tailor stops drawing and closes, then says so once the close has shown the HUD again.
            logger::error("Tailor: the screen failed while drawing, so Tailor is closing: {}", *failure);
            s.facadeReady = false;
            QueueLifecycleClose();
            SKSE::GetTaskInterface()->AddTask([] { RE::SendHUDMessage::ShowHUDMessage(TailorUI::kClosedAfterError); });
            return;
        }
        if (!generation || !HasFocus() || token != s.token.load()) {
            // The screen has already acted on these (a page may have changed), so say they never left.
            if (!result.actions.empty())
                logger::warn("ImGuiHost: dropped {} action(s) from '{}' (generation {}, focus {}, token {})",
                    result.actions.size(), result.actions.front().name, generation, HasFocus(), token == s.token.load());
            return;
        }
        auto send = [generation](std::string name, std::string data) {
            TailorUI::GetSingleton().Dispatch(std::move(name), std::move(data), generation);
        };
        for (auto& action : result.actions) send(std::move(action.name), std::move(action.data));
        const auto& r = result.viewport;
        if (std::isfinite(r.x) && std::isfinite(r.y) && std::isfinite(r.width) && std::isfinite(r.height) &&
            r.x >= 0 && r.y >= 0 && r.width > 0.01f && r.height > 0.01f &&
            r.x + r.width <= 1.001f && r.y + r.height <= 1.001f &&
            (!s.sentViewport || r != s.viewport || result.hairMode != s.hairMode)) {
            send("tailorPreviewViewport", Model{{"x", r.x}, {"y", r.y}, {"width", r.width},
                {"height", r.height}, {"hairMode", result.hairMode}, {"openGeneration", generation}}.dump());
            s.viewport = r; s.hairMode = result.hairMode; s.sentViewport = true;
        }
        if (std::isfinite(result.yaw) && result.yaw != s.yaw) {
            send("tailorPreviewRotate", Model{{"yaw", result.yaw}, {"sequence", ++s.sequence},
                {"openGeneration", generation}}.dump());
            s.yaw = result.yaw;
        }
    }

    RE::BSEventNotifyControl ImGuiHost::ProcessEvent(RE::InputEvent* const* events,
        RE::BSTEventSource<RE::InputEvent*>*)
    {
        auto& s = *_state;
        // The dispatch guard already delivered this batch in full.
        if (IsForwardingInput()) return RE::BSEventNotifyControl::kContinue;
        if (s.active && !HasFocus()) QueueLifecycleClose();
        if (!events) return RE::BSEventNotifyControl::kContinue;
        std::vector<State::Input> batch;
        for (auto* event = *events; event; event = event->next) {
            if (event->eventType == RE::INPUT_EVENT_TYPE::kDeviceConnect &&
                event->GetDevice() == RE::INPUT_DEVICE::kGamepad &&
                !static_cast<const RE::DeviceConnectEvent*>(event)->connected) {
                s.gamepadHeld = 0; s.usingGamepad = false; s.cursorMode = false; s.clearInput = true;
                s.leftX = 0; s.leftY = 0; s.rightX = 0; s.rightY = 0;
            }
            if (event->eventType == RE::INPUT_EVENT_TYPE::kMouseMove) {
                const auto* move = event->AsMouseMoveEvent();
                if (move && (move->mouseInputX || move->mouseInputY)) s.usingGamepad = false;
            }
            if (event->eventType == RE::INPUT_EVENT_TYPE::kThumbstick && s.controllerEnabled) {
                const auto* stick = event->AsThumbstickEvent();
                if (stick && stick->IsLeft()) { s.leftX = stick->xValue; s.leftY = stick->yValue; }
                if (stick && stick->IsRight()) { s.rightX = stick->xValue; s.rightY = stick->yValue; }
                if (stick && (std::abs(stick->xValue) > 0.25f || std::abs(stick->yValue) > 0.25f))
                    s.usingGamepad = true;
            }
            if (event->eventType == RE::INPUT_EVENT_TYPE::kChar && HasFocus()) {
                s.usingGamepad = false;
                const auto code = static_cast<const RE::CharEvent*>(event)->keyCode;
                if (input::IsTextCharacter(code)) batch.push_back({State::Input::Kind::Char, static_cast<int>(code)});
            }
            if (event->eventType != RE::INPUT_EVENT_TYPE::kButton) continue;
            const auto* button = event->AsButtonEvent();
            if (!button || (!button->IsDown() && !button->IsUp())) continue;
            const auto id = button->GetIDCode();
            const bool down = button->IsDown();
            if (button->GetDevice() == RE::INPUT_DEVICE::kGamepad) {
                if (down && s.controllerEnabled) s.usingGamepad = true;
                if (input::IsDigitalControllerButton(id)) {
                    if (down) s.gamepadHeld.fetch_or(id); else s.gamepadHeld.fetch_and(~id);
                }
                if (HasFocus() && s.controllerEnabled) {
                    if (s.controllerArmed && down && id == s.bindings[6]) {
                        s.cursorMode = !s.cursorMode.load();
                        s.clearInput = true;
                    }
                    if (s.cursorMode && down && (id & 0xF) && input::IsDigitalControllerButton(id)) {
                        s.cursorMode = false;
                        s.clearInput = true;
                    }
                }
                continue; // Render thread submits the engine's held state once.
            }
            if (down) s.usingGamepad = false;
            if (!HasFocus()) continue;
            if (button->GetDevice() == RE::INPUT_DEVICE::kKeyboard) {
                const auto key = input::ScanCodeToImGuiKey(id);
                if (key != ImGuiKey_None) batch.push_back({State::Input::Kind::Key, static_cast<int>(key), down});
            } else if (button->GetDevice() == RE::INPUT_DEVICE::kMouse) {
                if (id <= 2) batch.push_back({State::Input::Kind::Button, static_cast<int>(id), down});
                else if (down && (id == 8 || id == 9))
                    batch.push_back({State::Input::Kind::Wheel, 0, true, id == 8 ? 1.0f : -1.0f});
            }
        }
        if (!batch.empty()) {
            std::scoped_lock lock(s.inputLock);
            s.pendingInput.insert(s.pendingInput.end(), batch.begin(), batch.end());
        }
        return RE::BSEventNotifyControl::kContinue;
    }
}
