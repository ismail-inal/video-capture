#include <array>
#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <memory>
#include <print>
#include <string>
#include <thread>

#include <boost/lockfree/spsc_queue.hpp>

#ifdef _WIN32
#include <conio.h>
#else
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#endif

extern "C" {
#include "lib/types.h"
#include <SDL2/SDL.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

#include "Core/Frame.h"
#include "bitmap.h"
#include "camera.h"
#include "cameralibrary.h"
#include "cameramanager.h"
#include "frame.h"

constexpr const u32 WIDTH = 1920;
constexpr const u32 HEIGHT = 1080;
constexpr const u32 CHANNEL = 1;
constexpr const u32 FRAME_SIZE = WIDTH * HEIGHT * CHANNEL;

constexpr const i32 FPS = 120;
constexpr const u32 SECONDS = 3;
constexpr const u32 FRAME_NUM = FPS * SECONDS;
constexpr const u64 ARENA_SIZE = static_cast<u64>(FRAME_NUM) * FRAME_SIZE;

constexpr const u32 NUM_CAMERAS = 3;
constexpr const u32 DISPLAY_HERTZ = 30;

struct FrameMetadata {
    u64 pts;
    u64 dts;
    u32 flags;
    u32 reserved;
};

struct alignas(32) Arena {
    u8 data[ARENA_SIZE];
};

struct ControlState {
    std::atomic<bool> running{false};
    std::atomic<bool> paused{false};
    std::atomic<int> active_display_idx{0};
};

#ifndef _WIN32
struct RawTerm {
    termios orig_termios;
    RawTerm() {
        tcgetattr(STDIN_FILENO, &orig_termios);
        termios raw = orig_termios;
        raw.c_lflag &= ~(ECHO | ICANON);
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    }
    ~RawTerm() { tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios); }
};
#endif

struct CameraPipeline {
    // SDK
    std::shared_ptr<CameraLibrary::Camera> camera;
    std::string serial_str;
    CameraLibrary::Bitmap bmp;
    u32 hardware_freq = 0;

    // Buffers
    std::unique_ptr<Arena> arena;
    std::array<FrameMetadata, FRAME_NUM> frame_meta;

    // Queues
    boost::lockfree::spsc_queue<u16, boost::lockfree::capacity<FRAME_NUM>>
        free_q;
    boost::lockfree::spsc_queue<u16, boost::lockfree::capacity<FRAME_NUM>>
        ready_q;

    // Display
    std::atomic<u16> g_latest_display_frame_id{FRAME_NUM};

    // FFmpeg
    AVFormatContext *out_ctx = nullptr;
    AVStream *stream = nullptr;
    AVCodecContext *enc_ctx = nullptr;
    SwsContext *sws_ctx = nullptr;
    AVFrame *yuv_frame = nullptr;
    AVPacket *pkt = nullptr;

    CameraPipeline()
        : bmp(WIDTH, HEIGHT, WIDTH * CHANNEL, CameraLibrary::Bitmap::EightBit),
          arena(std::make_unique<Arena>()) {
        for (usize i = 0; i < FRAME_NUM; ++i) {
            free_q.push(static_cast<u16>(i));
        }
    }

    ~CameraPipeline() {
        if (out_ctx) {
            if (out_ctx->pb) {
                av_write_trailer(out_ctx);
                if (!(out_ctx->oformat->flags & AVFMT_NOFILE)) {
                    avio_closep(&out_ctx->pb);
                }
            }
            avformat_free_context(out_ctx);
        }
        if (pkt)
            av_packet_free(&pkt);
        if (yuv_frame)
            av_frame_free(&yuv_frame);
        if (sws_ctx)
            sws_freeContext(sws_ctx);
        // *** FIX 1: Pass address-of (&) ***
        if (enc_ctx)
            avcodec_free_context(&enc_ctx);
    }
};

// --- Thread Functions ---

void producer_loop(CameraPipeline *p, ControlState *controls) {
    while (!controls->running.load(std::memory_order_acquire)) {
        if (std::this_thread::get_id() == std::thread::id())
            return;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::println("[PROD-{}] Producer thread starting capture.", p->serial_str);

    u16 id;
    u8 *src = nullptr;
    auto cam = p->camera;
    auto &bmp = p->bmp;

    while (controls->running) {
        if (controls->paused) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        if (!p->free_q.pop(id)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        auto frame = cam->LatestFrame();
        if (!frame || frame->IsEmpty() || frame->IsInvalid()) {
            p->free_q.push(id);
            std::this_thread::sleep_for(
                std::chrono::microseconds(500)); // Polite yield
            continue;
        }

        u64 hw_ts = frame->HardwareTimeStamp();

        src = p->arena->data + static_cast<usize>(id) * FRAME_SIZE;
        frame->Rasterize(*cam, &bmp);
        std::memcpy(src, bmp.GetBits(), FRAME_SIZE);

        p->frame_meta[id].pts = hw_ts;
        p->frame_meta[id].dts = hw_ts;
        p->frame_meta[id].flags = 0;

        while (!p->ready_q.push(id)) {
            if (!controls->running)
                break;
        }
    }
    std::println("[PROD-{}] Producer thread stopping.", p->serial_str);
}

void consumer_loop(CameraPipeline *p, ControlState *controls,
                   std::promise<bool> ready_promise) {
    std::string outfile = p->serial_str + ".mkv";

    try {
        const AVCodec *codec = avcodec_find_encoder_by_name("hevc_nvenc");
        if (!codec)
            throw std::runtime_error("NVENC encoder 'hevc_nvenc' not found.");

        avformat_alloc_output_context2(&p->out_ctx, nullptr, "matroska",
                                       outfile.c_str());
        if (!p->out_ctx)
            throw std::runtime_error(
                "avformat_alloc_output_context2 (mkv) failed.");

        p->stream = avformat_new_stream(p->out_ctx, codec);
        if (!p->stream)
            throw std::runtime_error("avformat_new_stream failed.");

        p->enc_ctx = avcodec_alloc_context3(codec);
        if (!p->enc_ctx)
            throw std::runtime_error("avcodec_alloc_context3 failed.");

        p->enc_ctx->bit_rate = 8000000;
        p->enc_ctx->width = WIDTH;
        p->enc_ctx->height = HEIGHT;
        p->enc_ctx->gop_size = 60;
        p->enc_ctx->max_b_frames = 0;
        p->enc_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
        av_opt_set(p->enc_ctx->priv_data, "preset", "fast", 0);

        AVRational hw_time_base = {1, static_cast<int>(p->hardware_freq)};
        p->enc_ctx->time_base = hw_time_base;
        p->enc_ctx->framerate = AVRational{FPS, 1};
        p->stream->time_base = hw_time_base;

        if (avcodec_open2(p->enc_ctx, codec, nullptr) < 0)
            throw std::runtime_error("Could not open encoder.");
        if (avcodec_parameters_from_context(p->stream->codecpar, p->enc_ctx) <
            0)
            throw std::runtime_error("avcodec_parameters_from_context failed.");

        p->sws_ctx = sws_getContext(WIDTH, HEIGHT, AV_PIX_FMT_GRAY8, WIDTH,
                                    HEIGHT, AV_PIX_FMT_YUV420P, SWS_BILINEAR,
                                    nullptr, nullptr, nullptr);
        if (!p->sws_ctx)
            throw std::runtime_error("sws_getContext failed.");

        p->yuv_frame = av_frame_alloc();
        if (!p->yuv_frame)
            throw std::runtime_error("av_frame_alloc failed.");
        p->yuv_frame->format = p->enc_ctx->pix_fmt;
        p->yuv_frame->width = p->enc_ctx->width;
        p->yuv_frame->height = p->enc_ctx->height;
        if (av_frame_get_buffer(p->yuv_frame, 32) < 0)
            throw std::runtime_error("av_frame_get_buffer failed.");

        p->pkt = av_packet_alloc();
        if (!p->pkt)
            throw std::runtime_error("av_packet_alloc failed.");

        if (!(p->out_ctx->oformat->flags & AVFMT_NOFILE)) {
            if (avio_open(&p->out_ctx->pb, outfile.c_str(), AVIO_FLAG_WRITE) <
                0) {
                throw std::runtime_error("avio_open failed.");
            }
        }
        if (avformat_write_header(p->out_ctx, nullptr) < 0)
            throw std::runtime_error("avformat_write_header failed.");

        std::println("[CONS-{}] Consumer thread initialized. Output: {}",
                     p->serial_str, outfile);
        ready_promise.set_value(true);

    } catch (const std::exception &e) {
        std::println(std::cerr, "[CONS-{}] ERROR: {}", p->serial_str, e.what());
        ready_promise.set_value(false);
        return;
    }

    while (controls->running) {
        u16 id;
        if (!p->ready_q.pop(id)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        p->g_latest_display_frame_id.store(id, std::memory_order_release);

        const FrameMetadata meta = p->frame_meta[id];
        const u8 *src_gray =
            p->arena->data + static_cast<usize>(id) * FRAME_SIZE;

        const u8 *src_slice[1] = {src_gray};
        i32 src_stride[1] = {static_cast<i32>(WIDTH * CHANNEL)};

        sws_scale(p->sws_ctx, src_slice, src_stride, 0, HEIGHT,
                  p->yuv_frame->data, p->yuv_frame->linesize);

        p->yuv_frame->pts = static_cast<i64>(meta.pts);

        if (avcodec_send_frame(p->enc_ctx, p->yuv_frame) < 0) {
            std::println(std::cerr,
                         "WARNING: avcodec_send_frame failed for pts={}",
                         meta.pts);
        } else {
            while (avcodec_receive_packet(p->enc_ctx, p->pkt) == 0) {
                av_packet_rescale_ts(p->pkt, p->enc_ctx->time_base,
                                     p->stream->time_base);
                p->pkt->stream_index = p->stream->index;
                av_interleaved_write_frame(p->out_ctx, p->pkt);
                av_packet_unref(p->pkt);
            }
        }

        while (!p->free_q.push(id)) {
            if (!controls->running)
                break;
        }
    }

    avcodec_send_frame(p->enc_ctx, nullptr);
    while (avcodec_receive_packet(p->enc_ctx, p->pkt) == 0) {
        av_packet_rescale_ts(p->pkt, p->enc_ctx->time_base,
                             p->stream->time_base);
        p->pkt->stream_index = p->stream->index;
        av_interleaved_write_frame(p->out_ctx, p->pkt);
        av_packet_unref(p->pkt);
    }

    std::println("[CONS-{}] Consumer thread stopping.", p->serial_str);
}

void display_loop(std::array<CameraPipeline *, NUM_CAMERAS> pipelines,
                  ControlState *controls, std::promise<bool> ready_promise) {
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        std::println(std::cerr, "SDL could not initialize! SDL_Error: {}",
                     SDL_GetError());
        ready_promise.set_value(false);
        return;
    }

    SDL_Window *window = SDL_CreateWindow(
        "Live Preview (1, 2, 3 to switch)", SDL_WINDOWPOS_UNDEFINED,
        SDL_WINDOWPOS_UNDEFINED, WIDTH / 2, HEIGHT / 2, SDL_WINDOW_SHOWN);
    if (!window) {
        std::println(std::cerr, "Window cound not be created!");
        ready_promise.set_value(false);
        SDL_Quit();
        return;
    }

    SDL_Renderer *renderer =
        SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if (!renderer) {
        std::println(std::cerr, "Renderer cound not be created!");
        ready_promise.set_value(false);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return;
    }

    // *** FIX 2 & 3: Use RGBA32 texture, no palette ***
    SDL_Texture *texture =
        SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
                          SDL_TEXTUREACCESS_STREAMING, WIDTH, HEIGHT);
    if (!texture) {
        std::println(std::cerr, "Texture cound not be created!");
        ready_promise.set_value(false);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return;
    }

    // Temp buffer for CPU-side grayscale-to-RGBA conversion
    std::vector<u8> display_buffer(WIDTH * HEIGHT * 4);
    // *** END FIX 2 & 3 ***

    std::println("Display thread initialized successfully.");
    ready_promise.set_value(true);

    while (controls->running) {
        int idx = controls->active_display_idx.load(std::memory_order_relaxed);
        CameraPipeline *p = pipelines[idx];

        u16 frame_id_to_show =
            p->g_latest_display_frame_id.load(std::memory_order_acquire);

        if (frame_id_to_show < FRAME_NUM) {
            const u8 *gray_pixels =
                p->arena->data +
                static_cast<usize>(frame_id_to_show) * FRAME_SIZE;

            // *** FIX 2 & 3: CPU-side GRAY8 -> RGBA32 conversion ***
            u8 *rgba_pixels = display_buffer.data();
            for (int i = 0; i < WIDTH * HEIGHT; ++i) {
                *rgba_pixels++ = *gray_pixels; // R
                *rgba_pixels++ = *gray_pixels; // G
                *rgba_pixels++ = *gray_pixels; // B
                *rgba_pixels++ = 255;          // A
                gray_pixels++;
            }
            SDL_UpdateTexture(texture, nullptr, display_buffer.data(),
                              WIDTH * 4); // Use 4-channel pitch
            // *** END FIX 2 & 3 ***

            SDL_RenderCopy(renderer, texture, nullptr, nullptr);
            SDL_RenderPresent(renderer);
        }

        SDL_Event e;
        while (SDL_PollEvent(&e) != 0) {
            if (e.type == SDL_QUIT) {
                controls->running = false;
            }
        }

        std::this_thread::sleep_for(
            std::chrono::milliseconds(1000 / DISPLAY_HERTZ));
    }

    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    std::println("[DISPLAY] Display thread stopping.");
}

// --- Main Function ---

int main() {
    ControlState control_state;
    std::array<std::unique_ptr<CameraPipeline>, NUM_CAMERAS> pipelines;

    std::println("[MAIN] Initializing CameraLibrary...");
    CameraLibraryStartup();
    CameraLibrary::CameraManager::X().ScanForCameras();
    CameraLibrary::CameraList cam_list;
    CameraLibrary::CameraManager::X().GetCameraList(cam_list);

    if (cam_list.Count() < NUM_CAMERAS) {
        std::println(std::cerr, "ERROR: Found {} cameras, but {} are required.",
                     cam_list.Count(), NUM_CAMERAS);
        CameraLibraryShutdown();
        return -1;
    }
    std::println("[MAIN] Found {} cameras. Initializing first {}.",
                 cam_list.Count(), NUM_CAMERAS);

    std::vector<std::promise<bool>> consumer_ready_promises(NUM_CAMERAS);
    std::vector<std::future<bool>> consumer_ready_futures;
    std::promise<bool> display_ready_promise;
    std::future<bool> display_ready_future = display_ready_promise.get_future();

    std::vector<std::thread> threads;
    std::array<CameraPipeline *, NUM_CAMERAS> pipeline_pointers;

    try {
        for (int i = 0; i < NUM_CAMERAS; ++i) {
            pipelines[i] = std::make_unique<CameraPipeline>();
            CameraPipeline *p_raw = pipelines[i].get();
            pipeline_pointers[i] = p_raw;

            auto entry = cam_list[i];

            // *** FIX 4: Use GetCameraBySerial ***
            p_raw->camera = CameraLibrary::CameraManager::X().GetCameraBySerial(
                entry.Serial());
            if (!p_raw->camera) {
                throw std::runtime_error("Failed to get camera by serial: " +
                                         std::to_string(entry.Serial()));
            }
            p_raw->serial_str = std::to_string(p_raw->camera->Serial());

            std::println("[MAIN] Setting up camera: {}", p_raw->serial_str);

            p_raw->camera->SetVideoType(Core::GrayscaleMode);
            p_raw->camera->SetFrameRate(FPS);

            // *** FIX 5: Pre-roll to get HW Freq ***
            std::println("[MAIN] Starting camera {} for HW Freq check...",
                         p_raw->serial_str);
            p_raw->camera->Start();
            std::shared_ptr<const CameraLibrary::Frame> frame;
            auto start_time = std::chrono::steady_clock::now();
            while (true) {
                frame = p_raw->camera->LatestFrame();
                if (frame && !frame->IsEmpty() &&
                    frame->IsHardwareTimeStamp()) {
                    p_raw->hardware_freq = frame->HardwareTimeFreq();
                    break;
                }
                if (std::chrono::steady_clock::now() - start_time >
                    std::chrono::seconds(2)) {
                    throw std::runtime_error(
                        "Failed to get a valid frame from camera " +
                        p_raw->serial_str);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            p_raw->camera->Stop();
            std::println("[MAIN] Camera {} HW Freq: {}. Stopping camera.",
                         p_raw->serial_str, p_raw->hardware_freq);
            // *** END FIX 5 ***

            if (p_raw->hardware_freq == 0) {
                std::println(std::cerr,
                             "Warning: Camera {} does not support hardware "
                             "timestamps. Using FPS.",
                             p_raw->serial_str);
                p_raw->hardware_freq = FPS; // Fallback
            }

            consumer_ready_futures.push_back(
                consumer_ready_promises[i].get_future());
            threads.emplace_back(producer_loop, p_raw, &control_state);
            threads.emplace_back(consumer_loop, p_raw, &control_state,
                                 std::move(consumer_ready_promises[i]));
        }
    } catch (const std::exception &e) {
        std::println(std::cerr, "[MAIN] CRITICAL ERROR during camera init: {}",
                     e.what());
        control_state.running = false;
        CameraLibraryShutdown();
        return -1;
    }

    threads.emplace_back(display_loop, pipeline_pointers, &control_state,
                         std::move(display_ready_promise));

    bool all_consumers_ok = true;
    for (auto &fut : consumer_ready_futures) {
        if (!fut.get()) {
            all_consumers_ok = false;
        }
    }
    bool display_ok = display_ready_future.get();

    if (all_consumers_ok && display_ok) {
        std::println("All threads initialized. Starting main loop.");
        control_state.running = true;
        for (auto &p : pipelines)
            p->camera->Start();
    } else {
        std::println(std::cerr, "ERROR: Thread initialization failed.");
        control_state.running = false;
    }

#ifndef _WIN32
    RawTerm term_guard;
    fcntl(STDIN_FILENO, F_SETFL, fcntl(STDIN_FILENO, F_GETFL) | O_NONBLOCK);
#endif

    if (control_state.running) {
        std::println(
            "Press '1'-'3' to switch view, 'p' to pause/resume, 'q' to quit.");
    }

    while (control_state.running) {
        char ch = 0;
#ifdef _WIN32
        if (_kbhit()) {
            ch = _getch();
        }
#else
        if (read(STDIN_FILENO, &ch, 1) <= 0) {
            ch = 0;
        }
#endif

        if (ch == 'p') {
            bool paused_val = !control_state.paused.load();
            control_state.paused = paused_val;
            std::println("{}", paused_val ? "Paused" : "Resumed");
            if (paused_val) {
                for (auto &p : pipelines)
                    p->camera->Stop();
            } else {
                for (auto &p : pipelines)
                    p->camera->Start();
            }
        } else if (ch == 'q') {
            control_state.running = false;
        } else if (ch == '1') {
            control_state.active_display_idx = 0;
            std::println("Switched to camera 1");
        } else if (ch == '2') {
            control_state.active_display_idx = 1;
            std::println("Switched to camera 2");
        } else if (ch == '3') {
            control_state.active_display_idx = 2;
            std::println("Switched to camera 3");
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    std::println("[MAIN] Shutdown signaled. Waiting for threads to join...");

    for (auto &p : pipelines) {
        if (p && p->camera && p->camera->IsCameraRunning()) {
            p->camera->Stop();
        }
    }

    for (auto &t : threads)
        t.join();

    CameraLibrary::CameraManager::X().Shutdown();
    CameraLibraryShutdown();

    std::println("Finished.");
    return 0;
}
