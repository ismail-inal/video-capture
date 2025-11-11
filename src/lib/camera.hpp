#include "types.h"
#include <vector>

namespace camera {

enum FrameFormat { RGB24, RGBA32, YUV420P, YUV422P };

typedef usize *CameraHandle;

std::vector<CameraHandle> init();
void deinit(CameraHandle handle);
void get_frame(CameraHandle handle, u8 *buffer, u32 size);
void start(CameraHandle handle);
void stop(CameraHandle handle);
void set_fps(CameraHandle handle, u32 fps);
void set_size(CameraHandle handle, u32 width, u32 height);
void set_format(CameraHandle handle, FrameFormat format);

} // namespace camera
