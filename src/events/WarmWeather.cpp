#include "events/WarmWeather.h"
#include "events/WarmPolicy.h"

#include <unordered_map>

namespace Tailor::Situations
{
    namespace
    {
        std::uint8_t WeatherFlags(const RE::TESWeather* weather)
        {
            return weather ? weather->data.flags.underlying() : 0;
        }

        // Snow weathers' share of the region's weather chance. Region weather lists don't change while the
        // game runs, so each region is worked out once, on first sight.
        bool RegionIsSnowy(RE::TESRegion* region)
        {
            static std::unordered_map<RE::FormID, bool> snowyRegions;
            const auto id = region->GetFormID();
            if (const auto it = snowyRegions.find(id); it != snowyRegions.end()) return it->second;
            std::uint64_t snow = 0, total = 0;
            if (region->dataList) {
                for (auto* data : region->dataList->regionDataList) {
                    if (!data || data->GetType() != RE::TESRegionData::Type::kWeather) continue;
                    for (auto* entry : static_cast<RE::TESRegionDataWeather*>(data)->weatherTypes) {
                        if (!entry || !entry->weather) continue;
                        total += entry->chance;
                        if (WeatherFlags(entry->weather) & kSnowWeatherFlag) snow += entry->chance;
                    }
                }
            }
            const bool snowy = Tailor::Situations::IsSnowyRegion(snow, total);
            logger::info("Warm: weather region 0x{:X} has snow {} of {}: {}", id, snow, total, snowy ? "snowy" : "not snowy");
            snowyRegions.emplace(id, snowy);
            return snowy;
        }
    }

    bool IsColdOutdoors(RE::Actor* actor)
    {
        if (!actor) return false;
        auto* cell = actor->GetParentCell();
        if (!cell || !cell->IsExteriorCell()) return false;
        auto* sky = RE::Sky::GetSingleton();
        if (!sky) return false;
        // The weather region the sky draws from follows the player; logged as it changes, for the in-game check.
        auto* region = sky->region;
        static RE::FormID seenRegion = 0;
        if (const auto id = region ? region->GetFormID() : 0; id != seenRegion) {
            seenRegion = id;
            logger::info("Warm: the sky's weather region is now 0x{:X}", id);
        }
        const bool changing = sky->lastWeather && sky->currentWeatherPct < 1.0f;
        if (IsColdWeather(WeatherFlags(sky->currentWeather), WeatherFlags(sky->lastWeather), changing, sky->currentWeatherPct)) return true;
        return region && RegionIsSnowy(region);
    }
}
