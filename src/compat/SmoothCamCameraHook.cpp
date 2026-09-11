#include "compat/SmoothCamCameraHook.h"
#include "compat/SmoothCamCompat.h"

#include <mutex>

namespace
{
    struct MainUpdateCameraRequests
    {
        static void thunk()
        {
            func();
            SmoothCamCompat::GetSingleton().ProcessCameraRequests();
        }
        static inline REL::Relocation<decltype(thunk)> func;
    };
}

bool InstallSmoothCamCameraHook()
{
    static std::once_flag once;
    static bool installed = false;
    std::call_once(once, [] {
        if (REL::Module::IsVR()) return;
        // Main::Update's void() call after the engine update. Keep any existing
        // call hook in the chain. Offset provenance is in SMOOTHCAM_COMPATIBILITY.md.
        const auto offset = REL::Module::IsAE() &&
            REL::Module::get().version() >= REL::Version{1, 7, 99, 0} ?
            0xC38 : REL::Relocate(0x748, 0xC26);
        const auto call = REL::RelocationID(35565, 36564).address() + offset;
        if (*reinterpret_cast<const std::uint8_t*>(call) != 0xE8) {
            logger::warn("SmoothCamCompat: main-update call is unsupported; camera requests disabled");
            return;
        }
        SKSE::AllocTrampoline(14);
        MainUpdateCameraRequests::func = SKSE::GetTrampoline().write_call<5>(
            call, MainUpdateCameraRequests::thunk);
        installed = true;
        logger::info("SmoothCamCompat: main-update camera dispatcher installed");
    });
    return installed;
}
