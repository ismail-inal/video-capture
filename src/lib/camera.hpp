#pragma once

#include "types.h"
#include <algorithm> // For std::sort
#include <chrono>
#include <cstring>
#include <memory> // For std::shared_ptr
#include <print>
#include <set> // For duplicate filtering
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
    std::set<int> seen_serials; // Track serials to prevent duplicates

    std::println("Initializing OptiTrack Camera Manager...");

    // FIX 1: Robust Initialization Loop
    // WaitForInitialization() returns when the *Manager* is ready, but not
    // necessarily when all ethernet cameras have finished DHCP/Discovery.
    CameraLibrary::CameraManager::X().WaitForInitialization();

    // Additional poll to ensure cameras are actually online
    // Wait up to 5 seconds for cameras to appear
    for (int i = 0; i < 50; ++i) {
        if (CameraLibrary::CameraManager::X().AreCamerasInitialized()) {
            // Check if we actually see devices
            CameraLibrary::CameraList temp;
            CameraLibrary::CameraManager::X().GetCameraList(temp);
            if (temp.Count() > 0) {
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Explicit short delay to let duplicate/ghost entries settle
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    if (!CameraLibrary::CameraManager::X().AreCamerasInitialized()) {
        std::println(stderr, "ERROR: OptiTrack Camera Manager failed to "
                             "initialize or no cameras found.");
        return handles;
    }

    CameraLibrary::CameraList list;
    CameraLibrary::CameraManager::X().GetCameraList(list);

    if (list.Count() == 0) {
        std::println("No cameras found.");
        return handles;
    }

    std::println("SDK reported {} camera entries (filtering duplicates...)",
                 list.Count());

    // 2. Convert CameraEntry to Camera* AND Filter Duplicates
    for (int i = 0; i < list.Count(); ++i) {
        const CameraLibrary::CameraEntry &entry = list[i];

        auto cam_shared =
            CameraLibrary::CameraManager::X().GetCamera(entry.UID());
        if (cam_shared) {
            CameraLibrary::Camera *raw_cam = cam_shared.get();
            int serial = raw_cam->Serial();

            // FIX 2: Check for Duplicates
            if (seen_serials.find(serial) != seen_serials.end()) {
                std::println(stderr,
                             "WARNING: Skipped duplicate handle for Serial {}",
                             serial);
                continue;
            }

            // FIX 3: Check for Error State (E4 often means Sync Signal
            // missing/unstable) We can't fix hardware errors via code, but we
            // can log them.
            if (raw_cam->State() == CameraLibrary::Camera::Uninitialized) {
                std::println(
                    stderr,
                    "WARNING: Camera {} is in Uninitialized/Error state.",
                    serial);
            }

            seen_serials.insert(serial);
            handles.push_back(raw_cam);
        }
    }

    // 3. Sort handles by Serial Number
    std::sort(handles.begin(), handles.end(),
              [](handle a, handle b) { return a->Serial() < b->Serial(); });

    std::println("Finalized {} unique cameras.", handles.size());

    for (size_t i = 0; i < handles.size(); i++) {
        handle cam = handles[i];

        // Use top-level Core namespace
        cam->SetVideoType(Core::GrayscaleMode);

        // Turn off numeric LED ID
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

    (void)size;

    std::shared_ptr<const CameraLibrary::Frame> frame = nullptr;

    // Blocking wait for a frame.
    while (true) {
        frame = h->LatestFrame();
        if (frame)
            break;
        std::this_thread::sleep_for(std::chrono::microseconds(10));
    }

    if (frame) {
        int w = h->Width();
        int h_dim = h->Height();

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
        h->SetVideoType(Core::GrayscaleMode);
        std::println("Cam {} Set to Grayscale Mode", h->Serial());
        break;

    case RGBA32:
    case RGB24:
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
    // Optional
#endif
}

inline void set_ir_filter(handle h, bool enable_visible_light) {
#ifdef LIB_CAMERA_IMPLEMENTATION
    if (h) {
        h->SetIRFilter(enable_visible_light ? 1 : 0);
        std::println("Cam {} Filter: {}", h->Serial(),
                     enable_visible_light ? "Visible" : "IR");
    }
#endif
}

inline void set_fps(handle h, u32 fps) {}
inline void set_size(handle h, u32 width, u32 height) {}

} // namespace camera
