#include "ui/imgui/PapyrusInputGuard.h"
#include "ui/imgui/InputDispatchHook.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <array>
#include <mutex>
#include <string_view>

namespace Tailor::ImGuiUI
{
    namespace
    {
        using Tag = RE::StaticFunctionTag;
        bool (*skseIsKeyPressed)(Tag*, std::uint32_t) = nullptr;
        std::uint32_t (*skseGetNumKeysPressed)(Tag*) = nullptr;
        std::int32_t (*skseGetNthKeyPressed)(Tag*, std::uint32_t) = nullptr;

        bool IsKeyPressed(Tag* tag, std::uint32_t key) { return !IsInputOwned() && skseIsKeyPressed(tag, key); }
        std::uint32_t GetNumKeysPressed(Tag* tag) { return IsInputOwned() ? 0 : skseGetNumKeysPressed(tag); }
        std::int32_t GetNthKeyPressed(Tag* tag, std::uint32_t index) { return IsInputOwned() ? -1 : skseGetNthKeyPressed(tag, index); }

        struct Native { std::string_view name; std::uint32_t params; void** original; };
        constexpr std::string_view scriptName = "Input";

        // SKSE's native function objects mirror the engine's: the raw C++ callback is
        // the first member after NativeFunctionBase.
        void* SkseCallback(RE::BSScript::IFunction* function)
        {
            return *reinterpret_cast<void**>(reinterpret_cast<std::uintptr_t>(function) + sizeof(RE::BSScript::NF_util::NativeFunctionBase));
        }
        // Only an address on an executable page of a loaded module is ever called. A
        // native rebound by another plugin keeps a std::function there instead, which
        // fails this test and leaves everything untouched.
        bool IsCode(void* address)
        {
            HMODULE module = nullptr;
            MEMORY_BASIC_INFORMATION page{};
            return address &&
                GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(address), &module) && module &&
                VirtualQuery(address, &page, sizeof(page)) == sizeof(page) && page.State == MEM_COMMIT &&
                (page.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
        }
        RE::BSScript::IFunction* Find(RE::BSScript::ObjectTypeInfo& type, const Native& native)
        {
            auto* functions = type.GetGlobalFuncIter();
            for (std::uint32_t i = 0; functions && i < type.GetNumGlobalFuncs(); ++i) {
                auto* function = functions[i].func.get();
                if (function && function->GetName() == native.name && function->GetIsNative() && function->GetIsStatic() &&
                    function->GetParamCount() == native.params) return function;
            }
            return nullptr;
        }
        bool Install()
        {
            auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
            RE::BSTSmartPointer<RE::BSScript::ObjectTypeInfo> type;
            if (!vm || !vm->GetScriptObjectType(scriptName, type) || !type) {
                logger::warn("Tailor Papyrus input guard: SKSE's Input script is unavailable; polling scripts stay live while Tailor is open");
                return false;
            }
            const std::array natives{
                Native{"IsKeyPressed", 1, reinterpret_cast<void**>(&skseIsKeyPressed)},
                Native{"GetNumKeysPressed", 0, reinterpret_cast<void**>(&skseGetNumKeysPressed)},
                Native{"GetNthKeyPressed", 1, reinterpret_cast<void**>(&skseGetNthKeyPressed)}};
            // All three or none: a script must never see one guarded and another live.
            std::array<RE::BSScript::IFunction*, 3> bound{};
            std::array<void*, 3> callbacks{};
            for (std::size_t i = 0; i < natives.size(); ++i) {
                bound[i] = Find(*type, natives[i]);
                callbacks[i] = bound[i] ? SkseCallback(bound[i]) : nullptr;
                if (!IsCode(callbacks[i])) {
                    logger::warn("Tailor Papyrus input guard: Input.{} is not SKSE's bound native; guard not installed", natives[i].name);
                    return false;
                }
            }
            for (std::size_t i = 0; i < natives.size(); ++i) *natives[i].original = callbacks[i];
            vm->RegisterFunction("IsKeyPressed", scriptName, IsKeyPressed);
            vm->RegisterFunction("GetNumKeysPressed", scriptName, GetNumKeysPressed);
            vm->RegisterFunction("GetNthKeyPressed", scriptName, GetNthKeyPressed);
            // The VM must now resolve each name to a different function object than SKSE's.
            for (std::size_t i = 0; i < natives.size(); ++i) {
                if (const auto* function = Find(*type, natives[i]); !function || function == bound[i]) {
                    logger::warn("Tailor Papyrus input guard: the VM kept SKSE's Input.{}; polling scripts stay live while Tailor is open", natives[i].name);
                    return false;
                }
            }
            logger::info("Tailor Papyrus input guard installed: Input polling reports no keys while Tailor owns input");
            return true;
        }
    }

    bool InstallPapyrusInputGuard()
    {
        static std::once_flag once;
        static bool installed = false;
        std::call_once(once, [] { installed = Install(); });
        return installed;
    }
}
