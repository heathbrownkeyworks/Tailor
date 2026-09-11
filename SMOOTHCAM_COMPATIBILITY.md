# SmoothCam camera thread compatibility

## Failure and scope

A submitted Tailor 2.4.3 log recorded ten NPC-preview opens rejected with
`camera-control request declined (bad-thread)`, followed by setup failure
(`reason=7`). API discovery succeeded. Opens without an NPC skipped the request.
The same acquisition path remained in 2.5.1.

SmoothCam's public V1 API supplies `GetSmoothCamThreadId()`. Its Beta1.5 source
checks that thread before acquiring the camera; Beta1.7 omits that check. This
explains how a worker-thread call can work with some builds and be refused by
others. The reporter's exact SmoothCam binary was not supplied.

Sources:
- https://github.com/mwilsnd/SkyrimSE-SmoothCam/blob/Beta1.5/SmoothCam/source/modapi.cpp
- https://github.com/mwilsnd/SkyrimSE-SmoothCam/blob/Beta1.7/SmoothCam/source/modapi.cpp
- https://github.com/mwilsnd/SkyrimSE-SmoothCam/blob/master/SmoothCam/include/SmoothCamAPI.h

## 2.5.2 change

The cursor-menu worker queues a camera request. A small dispatcher in
`Main::Update` processes it only when its current Windows thread ID matches
SmoothCam's required thread. The existing preview tick waits for the result
before entering free camera or isolating the scene. A missing dispatcher,
thread mismatch, refusal, or two-second wait timeout still fails closed.

All preview teardown paths cancel an unprocessed request. A lease granted before
camera setup is also released. A worker-thread close marks release pending;
the main-update dispatcher completes it independently of whether Tailor is open.
Lifecycle teardown already on the required thread releases immediately.
A reopen waits for the prior release. A failed release remains tracked; another
plugin's ownership is never released. No polling threads, sleeps, or ordinary
SKSE task retries are used for this handoff.

The scene update, actor movement hold, AI, orbit, equipment, Meridian rendering,
and DLSS behavior are unchanged. Production remains NPC-only.

### Native hook

The chained `void()` call in `Main::Update` uses Address Library IDs 35565 (SE)
and 36564 (AE), offsets 0x748 (SE), 0xC26 (AE before 1.7.99), and 0xC38 (AE
1.7.99 onward). Installation checks for CALL rel32 and preserves the previous
callee. An unsupported instruction disables the dispatcher. VR is excluded.

Call-site and ABI reference:
- https://github.com/community-shaders/skyrim-community-shaders/blob/dev/src/Features/GrassCollision.h
- https://github.com/community-shaders/skyrim-community-shaders/blob/dev/src/Features/GrassCollision.cpp
- https://github.com/community-shaders/skyrim-community-shaders/blob/dev/src/Utils/VersionedRelocation.h

This is a runtime hook: a successful native build is not an in-game validation.

## Automated verification

`TailorSmoothCamTests` compiles the production adapter with an SKSE boundary
double and a SmoothCam implementation that checks real Windows thread IDs.
Its initial worker-call assertion failed on the original adapter and passes
with the queue. Tests cover 100 reopen cycles, duplicate closes, cancellation
before dispatch, close after grant but before camera setup, missing API/hook,
wrong dispatcher thread, MustKeep/AlreadyTaken/BadThread, AlreadyGiven,
release retries, and external ownership. No test calls the API on the wrong
thread after the fix.

The live-preview contract checks pending/denied handling and unconditional
cancellation during teardown. The NPC release contract guards the branch split.

## In-game validation

On 2026-09-11 Heath confirmed that the signed 2.5.2 SmoothCam test package fixed
the reported issue and approved combining it with the controller legend update
for release. Treat the reported SmoothCam failure as user-confirmed fixed on the
tested setup. The exact Skyrim runtime, SmoothCam version, and individual results
for the broader matrix below were not supplied.

Fresh combined-build gameplay and the remaining runtime matrix: NOT RUN.

Test on SE 1.5.97 and AE separately, including a SmoothCam build exhibiting the
reported BadThread refusal and the previously working SmoothCam setup.

1. Open without an NPC, close, then open on an NPC.
2. Close and reopen on the same NPC, then several different NPCs.
3. Close immediately during opening; repeat and load a save during an open.
4. Rotate, change outfit/wig, close, and verify normal camera, movement, dialogue,
   saving, and actor animation. Reopen to confirm control was returned.
5. Repeat with SmoothCam disabled in its MCM and with its DLL absent.

Capture `Tailor.log`. With SmoothCam installed it should show dispatcher
installation, acquisition with matching `current` and `required` thread IDs,
and a matching release after each granted session. Another camera owner's
refusal must remain a refusal. A canceled pending request must never acquire
after closing/loading. If the dispatcher reports a mismatch or timeout, retain
the log; do not bypass the thread or ownership checks.
