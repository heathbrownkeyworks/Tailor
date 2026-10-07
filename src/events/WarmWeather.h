#pragma once

namespace Tailor::Situations
{
    // Outdoors in cloudy, rainy or snowy weather, or anywhere outdoors in a snowy region: the Warm
    // situation. Game thread only.
    [[nodiscard]] bool IsColdOutdoors(RE::Actor* actor);
}
