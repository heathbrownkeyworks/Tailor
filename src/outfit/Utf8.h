#pragma once

#include <cstddef>
#include <string>

// Sanitize a C string that may contain invalid UTF-8 bytes (e.g. from mod data).
// Keeps exactly the UTF-8 nlohmann::json writes, so serializing the result never
// throws type_error.316: each byte that doesn't start such a sequence becomes '?'
// and the next byte is read afresh. Rejected: leads that are never valid (80-C1,
// F5-FF), overlong forms (E0 80-9F, F0 80-8F), surrogates (ED A0-BF), anything
// above U+10FFFF (F4 90-BF), and a sequence cut short.
inline std::string SanitizeUtf8(const char* input)
{
    if (!input || input[0] == '\0') return {};

    std::string result;
    const auto* p = reinterpret_cast<const unsigned char*>(input);

    while (*p) {
        const unsigned char lead = *p;
        // The sequence's length, and the range its second byte must fall in.
        std::size_t length = 0;
        unsigned char low = 0x80;
        unsigned char high = 0xBF;
        if (lead < 0x80) {
            length = 1;
        } else if (lead >= 0xC2 && lead <= 0xDF) {
            length = 2;
        } else if (lead >= 0xE0 && lead <= 0xEF) {
            length = 3;
            if (lead == 0xE0) low = 0xA0;
            if (lead == 0xED) high = 0x9F;
        } else if (lead >= 0xF0 && lead <= 0xF4) {
            length = 4;
            if (lead == 0xF0) low = 0x90;
            if (lead == 0xF4) high = 0x8F;
        }

        // A NUL fails every test, so nothing past the end is read.
        bool valid = length > 0;
        if (valid && length > 1) valid = p[1] >= low && p[1] <= high;
        for (std::size_t i = 2; valid && i < length; ++i) valid = (p[i] & 0xC0) == 0x80;

        if (!valid) {
            result += '?';
            ++p;
            continue;
        }
        result.append(reinterpret_cast<const char*>(p), length);
        p += length;
    }

    return result;
}
