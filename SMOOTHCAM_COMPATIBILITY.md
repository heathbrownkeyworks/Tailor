# SmoothCam compatibility

Tailor's live preview takes over the camera. When SmoothCam is installed, Tailor asks SmoothCam for the camera
through its public API first, and hands it back when the preview ends.

## The thread SmoothCam requires

SmoothCam's V1 API reports the thread it accepts camera requests on (`GetSmoothCamThreadId()`). Some SmoothCam
builds check it before granting the camera and refuse a request from any other thread with `BadThread`; others
don't check. A request made from the wrong thread works with one build and fails with the next. Before 2.5.2,
Tailor made the request from its menu's worker thread, and one player's log showed every NPC preview refused.

Sources:

- https://github.com/mwilsnd/SkyrimSE-SmoothCam/blob/Beta1.5/SmoothCam/source/modapi.cpp
- https://github.com/mwilsnd/SkyrimSE-SmoothCam/blob/Beta1.7/SmoothCam/source/modapi.cpp
- https://github.com/mwilsnd/SkyrimSE-SmoothCam/blob/master/SmoothCam/include/SmoothCamAPI.h

## How Tailor asks

Tailor queues the request. A small dispatcher that runs in `Main::Update` makes it, but only when the current
thread is the one SmoothCam names. The preview waits for the answer before it moves the camera or sets up the
scene. If there is no dispatcher, the thread doesn't match, SmoothCam refuses, or no answer comes within two
seconds, the preview closes instead of taking the camera anyway.

Every way the preview can end cancels a request that hasn't been made yet and gives back a camera SmoothCam
granted. A release that has to wait for SmoothCam's thread is finished by the dispatcher, whether or not Tailor is
still open, and a new preview waits for it. Tailor never releases a camera another plugin holds.

## Native hook

The dispatcher hooks the `void()` call in `Main::Update` at Address Library IDs 35565 (SE) and 36564 (AE), offset
0x748 on SE, 0xC26 on AE before 1.7.99 and 0xC38 on AE 1.7.99 and later. Tailor checks that the instruction there
is a CALL and keeps the previous callee in the chain. If it isn't a CALL, the dispatcher stays off and SmoothCam
requests are not made. VR is left out.

Call site and calling convention, as used by Community Shaders:

- https://github.com/community-shaders/skyrim-community-shaders/blob/dev/src/Features/GrassCollision.h
- https://github.com/community-shaders/skyrim-community-shaders/blob/dev/src/Features/GrassCollision.cpp
- https://github.com/community-shaders/skyrim-community-shaders/blob/dev/src/Utils/VersionedRelocation.h

## If the preview won't open with SmoothCam

Check `Tailor.log` (in `Documents/My Games/Skyrim Special Edition/SKSE/`). With SmoothCam installed it should show
the dispatcher being installed, the camera being granted on matching thread IDs, and the camera being released after
each preview. A line saying SmoothCam did not grant control means another mod holds the camera. A dispatcher
timeout or thread mismatch is worth reporting with the log.
