#pragma once

namespace RE { class Actor; }

namespace PapyrusBridge
{
    // Actor.SetRestrained(bool) dispatched via the Papyrus VM.
    //
    // Drop-in replacement for EnableAI(false)/(true) that does NOT halt
    // the actor's update tick. skee/NiOverride/MorphInterface keep ticking,
    // so any material or scenegraph commit lands the same frame instead of
    // being deferred until UI close. The NPC can't move or act, but still
    // idle-animates (breathing, blinking).
    bool SetActorRestrained(RE::Actor* actor, bool restrained);

    // Actor.SetDontMove(bool) dispatched via the Papyrus VM.
    //
    // Used with SetRestrained while the Tailor UI is open to stop behavior packages
    // from continuing pathing movement during outfit/wig work.
    bool SetActorDontMove(RE::Actor* actor, bool dontMove);
}
