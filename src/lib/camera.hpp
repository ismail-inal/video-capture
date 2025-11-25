#pragma once

#include "cameralibrary.h"
#include "types.h"
#include <chrono>
#include <cstring>
#include <print>
#include <thread>
#include <vector>

namespace camera {
enum FrameFormat { RGB24, RGBA32, YUV420P, YUV422P, GRAY8 };
typedef CameraLibrary::Camera *handle;

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

    // Sort cameras by Serial Number to ensure deterministic order
    // This ensures "Camera 0" is always the same physical unit.
    for (int i = 0; i < list.Count(); ++i) {
        for (int j = i + 1; j < list.Count(); ++j) {
            if (list[i]->Serial() > list[j]->Serial()) {
                CameraLibrary::Camera *temp = list[i];
                list[i] = list[j];
                list[j] = temp;
            }
        }
    }

    std::println("Found {} cameras.", list.Count());

    for (int i = 0; i < list.Count(); i++) {
        CameraLibrary::Camera *cam = list[i];

        // Default to Grayscale Video Mode (Fastest, matches your pipeline)
        cam->SetVideoType(CameraLibrary::Core::GrayscaleMode);

        // Turn off numeric LED ID on the camera front to save power/heat
        cam->SetNumeric(false, 0);

        handles.push_back(cam);
        std::println("  Cam {}: Serial {}", i, cam->Serial());
    }

    return handles;
#endif
}

inline void deinit(handle h) {
#ifdef LIB_CAMERA_IMPLEMENTATION
    if (h)
        h->Release();
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
inline void get_frame(handle h, u8 *buffer, u32 size,
                      u64 *out_frame_id = nullptr) {
#ifdef LIB_CAMERA_IMPLEMENTATION
    if (!h)
        return;

    CameraLibrary::Frame *frame = nullptr;

    // Blocking wait (poll)
    // In production, you might want to add a timeout break here.
    while (true) {
        frame = h->GetFrame();
        if (frame)
            break;
        // Yield to prevent 100% CPU usage
        std::this_thread::sleep_for(std::chrono::microseconds(10));
    }

    if (frame) {
        // Decode/copy to buffer
        // Note: Ensure 'size' matches (Width * Height * BytesPerPixel)
        frame->Rasterize(size, buffer);

        if (out_frame_id) {
            *out_frame_id = static_cast<u64>(frame->FrameID());
        }

        frame->Release();
    }
#endif
}

inline int get_serial(handle h) {
#ifdef LIB_CAMERA_IMPLEMENTATION
    return h ? h->Serial() : 0;
#endif
}

inline bool is_synced() {
#ifdef LIB_CAMERA_IMPLEMENTATION
    // 0 = Syncing, 1 = Synced, 2 = Lost/Freerun
    return (CameraLibrary::CameraManager::X().GetSynchronizationStatus() == 1);
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

    std::println(
        stderr,
        "WARNING: Hardware sync timed out. Cameras may not be aligned.");
    return false;
#else
    return true;
#endif
}

inline void set_format(handle h, FrameFormat format) {
#ifdef LIB_CAMERA_IMPLEMENTATION
    if (!h)
        return;

    switch (format) {
    case GRAY8:
        // Standard raw grayscale (1 byte per pixel)
        // Best for high FPS, low latency, tracking
        h->SetVideoType(CameraLibrary::Core::GrayscaleMode);
        std::println("Cam {} Set to Grayscale Mode", h->Serial());
        break;

    case RGBA32:
    case RGB24:
        // MJPEG Mode is required for Color on PrimeX cameras
        // (Sending raw color saturates ethernet instantly)
        h->SetVideoType(CameraLibrary::Core::MJPEGMode);
        std::println("Cam {} Set to MJPEG Mode (Color)", h->Serial());
        break;

    default:
        std::println(
            stderr,
            "WARNING: Format not explicitly supported by OptiTrack wrapper.");
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
    if (h)
        h->SetIRIllumination(enable);
#endif
}

// Set IR Cut Filter.
// true = Visible Light Mode (Blocks IR, use for Color/Video)
// false = IR Mode (Passes IR, use for Markers/Night Vision)
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
} // namespace camera
