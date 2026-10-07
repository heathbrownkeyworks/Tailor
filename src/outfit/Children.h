#pragma once

// Tailor never handles children, for outfits or wigs.
namespace Tailor::Children
{
    inline constexpr const char* kRefusalMessage = "Tailor can't be used on children.";

    // True for a child: Tailor never dresses one. A loaded, living child an earlier build dressed is released first,
    // once: helmets and weapons back, Tailor's wig off, the hair color and their own outfit back, and every Tailor
    // record of them removed. A release that can't finish keeps the records and is tried again at the next call.
    // Game thread only; never while holding WigManager's mutex.
    bool Skip(RE::Actor* actor);

    // Whether the first actor under the crosshair is a child, so Tailor doesn't open on them.
    bool CrosshairIsChild();
}
