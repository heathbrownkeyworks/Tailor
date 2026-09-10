#pragma once

namespace Tailor::Player
{
    class State
    {
    public:
        static void Register();
        static void Clear();
    private:
        static void Save(SKSE::SerializationInterface* api);
        static void Load(SKSE::SerializationInterface* api);
    };
}
