#pragma once

// Install once during the PostPostLoad API handshake, only with SmoothCam present.
[[nodiscard]] bool InstallSmoothCamCameraHook();
