#pragma once

namespace Tailor::Api
{
    // Binds the Tailor script's Global Native functions (papyrus/Source/Scripts/Tailor.psc) to the core in
    // ModApi.h. Call once from SKSEPlugin_Load, after SKSE::Init; SKSE binds them when the VM exists.
    void RegisterPapyrus();
}
