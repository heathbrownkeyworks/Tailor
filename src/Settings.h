#pragma once

#include <cstdint>
#include <string>

class Settings
{
public:
    static Settings& GetSingleton();

    void Load();

    uint32_t GetModifierKey() const { return _modifierKey; }
    uint32_t GetActivateKey() const { return _activateKey; }
    bool GetRefreshMorphs() const { return _refreshMorphs; }
    bool GetGrantPower() const { return _grantPower; }
    bool GetAutoFavorite() const { return _autoFavorite; }

private:
    Settings() = default;

    void CreateDefaultINI(const std::string& path) const;

    uint32_t _modifierKey = 0x2A;  // LShift
    uint32_t _activateKey = 0x2C;  // Z
    bool _refreshMorphs = true;
    bool _grantPower = true;
    bool _autoFavorite = true;
};
