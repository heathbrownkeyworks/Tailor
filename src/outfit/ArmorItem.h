#pragma once

#include <cstdint>
#include <string>

// Sanitize a C string that may contain invalid UTF-8 bytes (e.g. from mod data).
// Replaces invalid bytes with '?' to prevent nlohmann::json from throwing
// type_error.316 during serialization.
inline std::string SanitizeUtf8(const char* input)
{
    if (!input || input[0] == '\0') return {};

    std::string result;
    const auto* p = reinterpret_cast<const unsigned char*>(input);

    while (*p) {
        if (*p < 0x80) {
            result += static_cast<char>(*p++);
        } else if ((*p & 0xE0) == 0xC0) {
            if ((p[1] & 0xC0) == 0x80) {
                result += static_cast<char>(p[0]);
                result += static_cast<char>(p[1]);
                p += 2;
            } else {
                result += '?';
                p++;
            }
        } else if ((*p & 0xF0) == 0xE0) {
            if ((p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
                result += static_cast<char>(p[0]);
                result += static_cast<char>(p[1]);
                result += static_cast<char>(p[2]);
                p += 3;
            } else {
                result += '?';
                p++;
            }
        } else if ((*p & 0xF8) == 0xF0) {
            if ((p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
                result += static_cast<char>(p[0]);
                result += static_cast<char>(p[1]);
                result += static_cast<char>(p[2]);
                result += static_cast<char>(p[3]);
                p += 4;
            } else {
                result += '?';
                p++;
            }
        } else {
            result += '?';
            p++;
        }
    }

    return result;
}

struct ArmorItem
{
    RE::FormID  formId = 0;
    std::string plugin;
    std::string name;

    bool operator==(const ArmorItem& other) const
    {
        return formId == other.formId && plugin == other.plugin;
    }

    RE::TESObjectARMO* Resolve() const
    {
        auto* dataHandler = RE::TESDataHandler::GetSingleton();
        if (!dataHandler) return nullptr;
        auto* form = dataHandler->LookupForm(formId, plugin);
        return form ? form->As<RE::TESObjectARMO>() : nullptr;
    }
};

struct ModArmorList
{
    std::string             modName;
    std::vector<ArmorItem>  armors;
};
