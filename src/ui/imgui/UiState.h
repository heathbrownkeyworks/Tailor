#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace Tailor::ImGuiUI
{
    // Immutable snapshot for each frame. Keys retain the existing state message
    // names; values are JSON data, never JavaScript or executable expressions.
    using Model = nlohmann::json;

    struct Action
    {
        std::string name;
        std::string data;
    };

    struct Rect
    {
        float x = 0, y = 0, width = 0, height = 0;
        bool operator==(const Rect&) const = default;
    };

    struct FrameResult
    {
        std::vector<Action> actions;
        Rect viewport;  // normalized display coordinates
        bool hairMode = false;
        float yaw = 0;  // radians, matching PreviewOrbit and the existing camera
    };
}
