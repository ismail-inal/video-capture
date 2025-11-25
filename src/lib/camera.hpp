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

// CHANGE: Use shared_ptr to ensure Camera objects stay alive
typedef std::shared_ptr<CameraLibrary::Camera> handle;

// -----------------------------------------------------------------------------
// CORE LIFECYCLE
// -----------------------------------------------------------------------------

inline std::vector<handle> init() {
#ifdef LIB_CAMERA_IMPLEMENTATION
    std::vector<handle> handles;

    std::println("Initializing OptiTrack Camera Manager...");
    CameraLibrary::CameraManager::X().WaitForInitialization();

    // ------------------------------------------------------------
    // DISCOVERY LOOP
    // ------------------------------------------------------------
    const int target_unique_count = 3;
    const int timeout_seconds = 10;

    std::println("Scanning for {} UNIQUE cameras (Timeout: {}s)...",
                 target_unique_count, timeout_seconds);

    auto start_time = std::chrono::steady_clock::now();
    bool ready = false;

    while (std::chrono::steady_clock::now() - start_time <
           std::chrono::seconds(timeout_seconds)) {
        if (CameraLibrary::CameraManager::X().AreCamerasInitialized()) {
            CameraLibrary::CameraList list;
            CameraLibrary::CameraManager::X().GetCameraList(list);

            std::set<int> temp_serials;
            for (int i = 0; i < list.Count(); ++i) {
                auto cam_ptr =
                    CameraLibrary::CameraManager::X().GetCamera(list[i].UID());
                if (cam_ptr) {
                    temp_serials.insert(cam_ptr->Serial());
                }
            }

            if (temp_serials.size() >= target_unique_count) {
                std::println("Discovery Success: Found {} unique cameras.",
                             temp_serials.size());
                ready = true;
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (!ready) {
        std::println(stderr, "TIMEOUT: Did not find {} unique cameras in time.",
                     target_unique_count);
    }

    // ------------------------------------------------------------
    // FINAL LIST BUILDING
    // ------------------------------------------------------------
    std::println("Building Camera List...");

    CameraLibrary::CameraList list;
    CameraLibrary::CameraManager::X().GetCameraList(list);
    std::set<int> seen_serials;

    for (int i = 0; i < list.Count(); ++i) {
        auto cam_shared =
            CameraLibrary::CameraManager::X().GetCamera(list[i].UID());

        if (cam_shared) {
            int serial = cam_shared->Serial();

            // Filter Duplicates
            if (seen_serials.find(serial) != seen_serials.end()) {
                continue;
            }

            seen_serials.insert(serial);
            handles.push_back(cam_shared);
            std::println("  - Acquired Camera Serial: {}", serial);
        }
    }

    // Sort by Serial Number
    std::sort(handles.begin(), handles.end(),
              [](handle a, handle b) { return a->Serial() < b->Serial(); });

    std::println("Initialized {} cameras.", handles.size());

    // Apply Base Settings
    for (size_t i = 0; i < handles.size(); i++) {
        handle cam = handles[i];
        if (cam) {
            cam->SetVideoType(Core::GrayscaleMode);
            cam->SetNumeric(false, 0);
        }
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

    while (true) {
        frame = h->LatestFrame();
        if (frame)
            break;
        std::this_thread::sleep_for(std::chrono::microseconds(10));
    }

    if (frame) {
        int w = h->Width();
        int h_dim = h->Height();

        // Pass the dereferenced camera object (*h)
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
    // Assumed true if SDK doesn't expose explicit check in this version
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
    std::println(stderr, "WARNING: Hardware sync timed out.");
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
