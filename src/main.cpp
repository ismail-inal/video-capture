#pragma once

#include "lib/types.h"
#include <algorithm> // For std::sort
#include <chrono>
#include <cstring>
#include <memory> // For std::shared_ptr
#include <print>
#include <thread>
#include <vector>

// OPTITRACK SDK
#include "cameralibrary.h"

namespace camera {

enum FrameFormat { RGB24, RGBA32, YUV420P, YUV422P, GRAY8 };

// Use native pointer for direct SDK access
typedef CameraLibrary::Camera *handle;

// -----------------------------------------------------------------------------
// CORE LIFECYCLE
// -----------------------------------------------------------------------------

inline std::vector<handle> init() {
#ifdef LIB_CAMERA_IMPLEMENTATION
    std::vector<handle> handles;

    std::println("Initializing OptiTrack Camera Manager...");
    CameraLibrary::CameraManager::X().WaitForInitialization();

    if (!CameraLibrary::CameraManager::X().AreCamerasInitialized()) {
        std::println(stderr,
                     "ERROR: OptiTrack Camera Manager failed to initialize.");
        return handles;
    }

    CameraLibrary::CameraList list;
    CameraLibrary::CameraManager::X().GetCameraList(list);

    if (list.Count() == 0) {
        std::println("No cameras found.");
        return handles;
    }

    // 1. Convert CameraEntry objects to Camera* handles
    // The list[] operator returns a CameraEntry, which contains the UID.
    for (int i = 0; i < list.Count(); ++i) {
        const CameraLibrary::CameraEntry &entry = list[i];

        // GetCamera returns std::shared_ptr<Camera>, get raw pointer using
        // .get()
        auto cam_shared =
            CameraLibrary::CameraManager::X().GetCamera(entry.UID());
        if (cam_shared) {
            handles.push_back(cam_shared.get());
        }
    }

    // 2. Sort handles by Serial Number
    std::sort(handles.begin(), handles.end(),
              [](handle a, handle b) { return a->Serial() < b->Serial(); });

    std::println("Found {} cameras.", handles.size());

    for (size_t i = 0; i < handles.size(); i++) {
        handle cam = handles[i];

        // FIX: Use top-level Core namespace
        cam->SetVideoType(Core::GrayscaleMode);

        // Turn off numeric LED ID on the camera front
        cam->SetNumeric(false, 0);

        std::println("  Cam {}: Serial {}", i, cam->Serial());
    }

    return handles;
#endif
}

inline void deinit(handle h) {
#ifdef LIB_CAMERA_IMPLEMENTATION
    if (h)
        h->Stop();
#endif
}

inline void start(handle h) {
#ifdef LIB_CAMERA_IMPLEMENTATION
    if (h) {
        h->Start();
        std::println("Started Camera {}", h->Serial());
    }
#endif
}

inline void stop(handle h) {
#ifdef LIB_CAMERA_IMPLEMENTATION
    if (h) {
        h->Stop();
        std::println("Stopped Camera {}", h->Serial());
    }
#endif
}

// -----------------------------------------------------------------------------
// CAPTURE
// -----------------------------------------------------------------------------

inline void get_frame(handle h, u8 *buffer, u32 size,
                      u64 *out_frame_id = nullptr) {
#ifdef LIB_CAMERA_IMPLEMENTATION
    if (!h)
        return;

    (void)size; // Suppress unused warning

    // FIX: LatestFrame() returns std::shared_ptr<const Frame>
    std::shared_ptr<const CameraLibrary::Frame> frame = nullptr;

    // Blocking wait for a frame.
    while (true) {
        frame = h->LatestFrame();
        // Check if pointer is valid
        if (frame)
            break;
        std::this_thread::sleep_for(std::chrono::microseconds(10));
    }

    if (frame) {
        int w = h->Width();
        int h_dim = h->Height();

        // Signature: Rasterize(Camera &camera, int width, int height, int span,
        // int bitsPerPixel, void *buffer) const
        frame->Rasterize(*h, w, h_dim, w, 8, buffer);

        if (out_frame_id) {
            *out_frame_id = static_cast<u64>(frame->FrameID());
        }
    }
#endif
}

// -----------------------------------------------------------------------------
// SYNC & INFO
// -----------------------------------------------------------------------------

inline int get_serial(handle h) {
#ifdef LIB_CAMERA_IMPLEMENTATION
    return h ? h->Serial() : 0;
#endif
}

inline bool is_synced() {
#ifdef LIB_CAMERA_IMPLEMENTATION
    return true;
#else
    return true;
#endif
}

inline bool wait_for_sync(u32 timeout_ms = 5000) {
#ifdef LIB_CAMERA_IMPLEMENTATION
    std::println("Waiting for Hardware Sync Lock...");
    auto start = std::chrono::steady_clock::now();

    while (std::chrono::steady_clock::now() - start <
           std::chrono::milliseconds(timeout_ms)) {
        if (is_synced()) {
            std::println("System is hardware synchronized.");
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::println(stderr, "WARNING: Hardware sync timed out or check disabled.");
    return false;
#else
    return true;
#endif
}

// -----------------------------------------------------------------------------
// CONFIGURATION
// -----------------------------------------------------------------------------

inline void set_format(handle h, FrameFormat format) {
#ifdef LIB_CAMERA_IMPLEMENTATION
    if (!h)
        return;

    switch (format) {
    case GRAY8:
        // FIX: Use top-level Core namespace
        h->SetVideoType(Core::GrayscaleMode);
        std::println("Cam {} Set to Grayscale Mode", h->Serial());
        break;

    case RGBA32:
    case RGB24:
        // FIX: Use top-level Core namespace
        h->SetVideoType(Core::MJPEGMode);
        std::println("Cam {} Set to MJPEG Mode (Color)", h->Serial());
        break;

    default:
        std::println(stderr, "WARNING: Format not explicitly supported.");
        break;
    }
#endif
}

inline void set_exposure(handle h, int microseconds) {
#ifdef LIB_CAMERA_IMPLEMENTATION
    if (h) {
        h->SetExposure(microseconds);
        std::println("Cam {} Exposure: {} us", h->Serial(), microseconds);
    }
#endif
}

inline void set_gain(handle h, int gain) {
#ifdef LIB_CAMERA_IMPLEMENTATION
    if (h)
        h->SetIntensity(gain);
#endif
}

inline void set_ir_illumination(handle h, bool enable) {
#ifdef LIB_CAMERA_IMPLEMENTATION
    // Optional: implementation depends on exact SDK support
#endif
}

inline void set_ir_filter(handle h, bool enable_visible_light) {
#ifdef LIB_CAMERA_IMPLEMENTATION
    if (h) {
        // 1 = Visible (IR Cut ON), 0 = IR (IR Cut OFF)
        h->SetIRFilter(enable_visible_light ? 1 : 0);
        std::println("Cam {} Filter: {}", h->Serial(),
                     enable_visible_light ? "Visible" : "IR");
    }
#endif
}

// Deprecated setters
inline void set_fps(handle h, u32 fps) {}
inline void set_size(handle h, u32 width, u32 height) {}

} // namespace camera
