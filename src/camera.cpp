#include "lib/camera.hpp"
#include "lib/types.h"
#include <print>

inline void camera::get_frame(CameraHandle handle, u8 *buffer, u32 size) {
    u32 *src = reinterpret_cast<u32 *>(buffer);
    for (usize i = 0, k = size / 4; i < k; ++i) {
        src[i] = 0xFFC0CB;
    }
}

void camera::start(CameraHandle handle) {
    std::println("Stopped camera. {}", (void *)handle);
}

void camera::stop(CameraHandle handle) {
    std::println("Started camera. {}", (void *)handle);
}

std::vector<camera::CameraHandle> camera::init() {
    std::vector<CameraHandle> camera_handles = {nullptr, nullptr, nullptr};
    return camera_handles;
}

void camera::deinit(CameraHandle handle) {
    std::println("camera. {}", (void *)handle);
}

void camera::set_format(CameraHandle handle, FrameFormat format) {
    std::println("Camera format set. {}, {}", (void *)handle, (i32)format);
}

void camera::set_fps(CameraHandle handle, u32 fps) {
    std::println("Camera fps set. {}, {}", (void *)handle, fps);
}

void camera::set_size(CameraHandle handle, u32 width, u32 height) {
    std::println("Camera size set. {}, {}x{}", (void *)handle, width, height);
}
