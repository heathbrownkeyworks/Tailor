#pragma once

#include <Windows.h>

#include <string>
#include <string_view>

namespace Tailor::ImGuiUI
{
    // A name's place in a list, as the HTML screen's localeCompare put it: Windows' collation for the player's language,
    // ignoring capitals in any alphabet and keeping accented letters beside their base letter. Keys compare as plain
    // byte strings. The name's own bytes follow the collation key, so the same name in other capitals still has one
    // fixed order.
    [[nodiscard]] inline std::string NameSortKey(std::string_view name)
    {
        std::string key;
        const int length = static_cast<int>(name.size());
        const int wideLength = length ? MultiByteToWideChar(CP_UTF8, 0, name.data(), length, nullptr, 0) : 0;
        if (wideLength > 0) {
            std::wstring wide(static_cast<std::size_t>(wideLength), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, name.data(), length, wide.data(), wideLength);
            constexpr DWORD flags = LCMAP_SORTKEY | LINGUISTIC_IGNORECASE;
            const int size = LCMapStringEx(LOCALE_NAME_USER_DEFAULT, flags, wide.c_str(), wideLength, nullptr, 0, nullptr, nullptr, 0);
            if (size > 0) {
                key.resize(static_cast<std::size_t>(size));
                LCMapStringEx(LOCALE_NAME_USER_DEFAULT, flags, wide.c_str(), wideLength, reinterpret_cast<LPWSTR>(key.data()), size,
                    nullptr, nullptr, 0);
                while (!key.empty() && key.back() == '\0') key.pop_back();
            }
        }
        key.push_back('\0');
        key.append(name);
        return key;
    }
}
