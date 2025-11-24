#pragma once

#include "types.h"
#include <cstring>
#include <print>
#include <vector>

namespace camera {

enum FrameFormat { RGB24, RGBA32, YUV420P, YUV422P, GRAY8 };
typedef usize *handle;

inline void get_frame(handle handle, u8 *buffer, u32 size) {
#ifdef LIB_CAMERA_IMPLEMENTATION
    std::memset(buffer, 0x0, size);
#endif
}

inline void start(handle handle) {
#ifdef LIB_CAMERA_IMPLEMENTATION
    std::println("Started camera. {}", (void *)handle);
#endif
}

inline void stop(handle handle) {
#ifdef LIB_CAMERA_IMPLEMENTATION
    std::println("Stopped camera. {}", (void *)handle);
#endif
}

inline std::vector<handle> init() {
#ifdef LIB_CAMERA_IMPLEMENTATION
    std::vector<handle> camera_handles = {nullptr, nullptr, nullptr};
    return camera_handles;
#endif
}

inline void deinit(handle handle) {
#ifdef LIB_CAMERA_IMPLEMENTATION
    std::println("camera. {}", (void *)handle);
#endif
}

inline void set_format(handle handle, FrameFormat format) {
#ifdef LIB_CAMERA_IMPLEMENTATION
    std::println("Camera format set. {}, {}", (void *)handle, (i32)format);
#endif
}

inline void set_fps(handle handle, u32 fps) {
#ifdef LIB_CAMERA_IMPLEMENTATION
    std::println("Camera fps set. {}, {}", (void *)handle, fps);
#endif
}

inline void set_size(handle handle, u32 width, u32 height) {
#ifdef LIB_CAMERA_IMPLEMENTATION
    std::println("Camera size set. {}, {}x{}", (void *)handle, width, height);
#endif
}

} // namespace camera
