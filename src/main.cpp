#include <array>
#include <atomic>
#include <boost/lockfree/spsc_queue.hpp>
#include <chrono>
#include <future>
#include <iostream>
#include <memory>
#include <print>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <conio.h>
#else
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#endif

#define SDL_MAIN_HANDLED
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
constexpr const u32 MAX_FRAME_SIZE = 4 * 1024 * 1024;

constexpr const i32 FPS = 250;
constexpr const u32 SECONDS = 1;
constexpr const u32 FRAME_NUM = FPS * SECONDS;
constexpr const u64 ARENA_SIZE = static_cast<u64>(FRAME_NUM) * MAX_FRAME_SIZE;

constexpr const u32 NUM_CAMERAS = 3;

struct FrameMetadata {
    u64 pts;
    u32 compressed_size;
    u32 flags;
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
    std::shared_ptr<CameraLibrary::Camera> camera;
    std::string serial_str;
    i32 actual_fps = 0;

    std::unique_ptr<Arena> arena;
    std::array<FrameMetadata, FRAME_NUM> frame_meta;

    boost::lockfree::spsc_queue<u16, boost::lockfree::capacity<FRAME_NUM>>
        free_q;
    boost::lockfree::spsc_queue<u16, boost::lockfree::capacity<FRAME_NUM>>
        ready_q;

    std::atomic<u16> g_latest_display_frame_id{FRAME_NUM};

    AVFormatContext *out_ctx = nullptr;
    AVStream *stream = nullptr;

    CameraPipeline() : arena(std::make_unique<Arena>()) {
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
    }
};

static void producer(CameraPipeline *p, ControlState *controls) {
    while (!controls->running.load(std::memory_order_acquire)) {
        if (std::this_thread::get_id() == std::thread::id())
            return;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::println("[PROD-{}] Producer thread starting capture.", p->serial_str);

    u16 id;
    u8 *dest_buf = nullptr;
    auto &cam = p->camera;

    u64 pts_counter = 0;

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
            std::this_thread::sleep_for(std::chrono::microseconds(500));
            continue;
        }

        const CameraLibrary::Frame *frame_ptr = frame.get();
        int compressed_size = p->camera->CompressedImageSize(frame_ptr);

        if (compressed_size <= 0 || (u32)compressed_size > MAX_FRAME_SIZE) {
            std::println(std::cerr, "[PROD-{}] Invalid compressed size: {}",
                         p->serial_str, compressed_size);
            p->free_q.push(id);
            continue;
        }

        dest_buf = p->arena->data + static_cast<usize>(id) * MAX_FRAME_SIZE;
        p->camera->CompressedImage(frame_ptr, dest_buf, compressed_size);

        p->frame_meta[id].pts = pts_counter++;
        p->frame_meta[id].compressed_size = (u32)compressed_size;
        p->frame_meta[id].flags = 0;

        while (!p->ready_q.push(id)) {
            if (!controls->running)
                break;
        }

        p->g_latest_display_frame_id.store(id, std::memory_order_release);
    }
    std::println("[PROD-{}] Producer thread stopping.", p->serial_str);
}

static void consumer(CameraPipeline *p, ControlState *controls,
                     std::promise<bool> ready_promise) {
    std::string outfile = p->serial_str + ".mkv";
    AVPacket *pkt = nullptr;

    try {
        avformat_alloc_output_context2(&p->out_ctx, nullptr, "matroska",
                                       outfile.c_str());
        if (!p->out_ctx)
            throw std::runtime_error(
                "avformat_alloc_output_context2 (mkv) failed.");

        p->stream = avformat_new_stream(p->out_ctx, nullptr);
        if (!p->stream)
            throw std::runtime_error("avformat_new_stream failed.");

        p->stream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
        p->stream->codecpar->codec_id = AV_CODEC_ID_MJPEG;
        p->stream->codecpar->width = WIDTH;
        p->stream->codecpar->height = HEIGHT;
        p->stream->codecpar->format = AV_PIX_FMT_YUVJ420P;

        p->stream->time_base = {1, p->actual_fps};

        pkt = av_packet_alloc();
        if (!pkt)
            throw std::runtime_error("av_packet_alloc failed.");

        if (!(p->out_ctx->oformat->flags & AVFMT_NOFILE)) {
            if (avio_open(&p->out_ctx->pb, outfile.c_str(), AVIO_FLAG_WRITE) <
                0) {
                throw std::runtime_error("avio_open failed.");
            }
        }
        if (avformat_write_header(p->out_ctx, nullptr) < 0)
            throw std::runtime_error("avformat_write_header failed.");

        std::println(
            "[CONS-{}] Consumer (Remuxer) thread initialized. Output: {}",
            p->serial_str, outfile);
        ready_promise.set_value(true);

    } catch (const std::exception &e) {
        std::println(std::cerr, "[CONS-{}] ERROR: {}", p->serial_str, e.what());
        if (pkt)
            av_packet_free(&pkt);
        ready_promise.set_value(false);
        return;
    }

    while (controls->running) {
        u16 id;
        if (!p->ready_q.pop(id)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        const FrameMetadata meta = p->frame_meta[id];
        const u8 *compressed_data =
            p->arena->data + static_cast<usize>(id) * MAX_FRAME_SIZE;

        if (av_new_packet(pkt, (int)meta.compressed_size) < 0) {
            std::println(std::cerr, "[CONS-{}] av_new_packet failed",
                         p->serial_str);
            continue;
        }
        std::memcpy(pkt->data, compressed_data, meta.compressed_size);

        pkt->pts = static_cast<i64>(meta.pts);
        pkt->dts = static_cast<i64>(meta.pts);
        pkt->stream_index = p->stream->index;
        pkt->flags |= AV_PKT_FLAG_KEY;

        av_interleaved_write_frame(p->out_ctx, pkt);
        av_packet_unref(pkt);

        while (!p->free_q.push(id)) {
            if (!controls->running)
                break;
        }
    }

    std::println("[CONS-{}] Consumer thread stopping.", p->serial_str);
    av_packet_free(&pkt);
}

static void display(std::array<CameraPipeline *, NUM_CAMERAS> pipelines,
                    ControlState *controls, std::promise<bool> ready_promise) {
    AVCodecContext *dec_ctx = nullptr;
    const AVCodec *decoder = nullptr;
    AVFrame *raw_frame = nullptr;
    AVFrame *rgba_frame = nullptr;
    AVPacket *pkt = nullptr;
    SwsContext *sws_ctx = nullptr;
    std::vector<u8> rgba_buffer;

    SDL_Window *window = nullptr;
    SDL_Renderer *renderer = nullptr;
    SDL_Texture *texture = nullptr;

    try {
        if (SDL_Init(SDL_INIT_VIDEO) < 0)
            throw std::runtime_error(SDL_GetError());
        window = SDL_CreateWindow(
            "Live Preview (1, 2, 3 to switch)", SDL_WINDOWPOS_UNDEFINED,
            SDL_WINDOWPOS_UNDEFINED, WIDTH / 2, HEIGHT / 2, SDL_WINDOW_SHOWN);
        if (!window)
            throw std::runtime_error(SDL_GetError());
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
        if (!renderer)
            throw std::runtime_error(SDL_GetError());
        texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
                                    SDL_TEXTUREACCESS_STREAMING, WIDTH, HEIGHT);
        if (!texture)
            throw std::runtime_error(SDL_GetError());

        decoder = avcodec_find_decoder(AV_CODEC_ID_MJPEG);
        if (!decoder)
            throw std::runtime_error("MJPEG decoder not found.");
        dec_ctx = avcodec_alloc_context3(decoder);
        if (!dec_ctx)
            throw std::runtime_error(
                "avcodec_alloc_context3 (decoder) failed.");
        if (avcodec_open2(dec_ctx, decoder, nullptr) < 0)
            throw std::runtime_error("avcodec_open2 (decoder) failed.");

        raw_frame = av_frame_alloc();
        rgba_frame = av_frame_alloc();
        pkt = av_packet_alloc();
        if (!raw_frame || !rgba_frame || !pkt)
            throw std::runtime_error("FFmpeg alloc failed.");

        sws_ctx = sws_getContext(WIDTH, HEIGHT, AV_PIX_FMT_YUVJ420P, WIDTH,
                                 HEIGHT, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr,
                                 nullptr, nullptr);
        if (!sws_ctx)
            throw std::runtime_error("sws_getContext (display) failed.");

        int num_bytes =
            av_image_get_buffer_size(AV_PIX_FMT_RGBA, WIDTH, HEIGHT, 32);
        rgba_buffer.resize(num_bytes);
        av_image_fill_arrays(rgba_frame->data, rgba_frame->linesize,
                             rgba_buffer.data(), AV_PIX_FMT_RGBA, WIDTH, HEIGHT,
                             32);

        std::println("Display thread initialized successfully.");
        ready_promise.set_value(true);
    } catch (const std::exception &e) {
        std::println(std::cerr, "[DISPLAY] ERROR: {}", e.what());
        if (sws_ctx)
            sws_freeContext(sws_ctx);
        if (pkt)
            av_packet_free(&pkt);
        if (raw_frame)
            av_frame_free(&raw_frame);
        if (rgba_frame)
            av_frame_free(&rgba_frame);
        if (dec_ctx)
            avcodec_free_context(&dec_ctx);
        if (texture)
            SDL_DestroyTexture(texture);
        if (renderer)
            SDL_DestroyRenderer(renderer);
        if (window)
            SDL_DestroyWindow(window);
        if (SDL_WasInit(SDL_INIT_VIDEO))
            SDL_Quit();
        ready_promise.set_value(false);
        return;
    }

    u16 last_displayed_frame_id = FRAME_NUM;

    while (controls->running) {
        int idx = controls->active_display_idx.load(std::memory_order_relaxed);
        CameraPipeline *p = pipelines[idx];

        u16 frame_id_to_show =
            p->g_latest_display_frame_id.load(std::memory_order_acquire);
        if (frame_id_to_show == last_displayed_frame_id) {
            std::this_thread::sleep_for(std::chrono::microseconds(500));
            continue;
        }
        last_displayed_frame_id = frame_id_to_show;

        if (frame_id_to_show < FRAME_NUM) {
            const FrameMetadata meta = p->frame_meta[frame_id_to_show];
            const u8 *compressed_data =
                p->arena->data +
                static_cast<usize>(frame_id_to_show) * MAX_FRAME_SIZE;

            if (av_new_packet(pkt, (int)meta.compressed_size) < 0)
                continue;
            std::memcpy(pkt->data, compressed_data, meta.compressed_size);

            if (avcodec_send_packet(dec_ctx, pkt) == 0) {
                if (avcodec_receive_frame(dec_ctx, raw_frame) == 0) {
                    sws_scale(sws_ctx, (const u8 *const *)raw_frame->data,
                              raw_frame->linesize, 0, HEIGHT, rgba_frame->data,
                              rgba_frame->linesize);

                    SDL_UpdateTexture(texture, nullptr, rgba_frame->data[0],
                                      rgba_frame->linesize[0]);
                }
            }
            av_packet_unref(pkt);

            SDL_RenderClear(renderer);
            SDL_RenderCopy(renderer, texture, nullptr, nullptr);
            SDL_RenderPresent(renderer);
        }

        SDL_Event e;
        while (SDL_PollEvent(&e) != 0) {
            if (e.type == SDL_QUIT) {
                controls->running = false;
            }
        }
    }

    sws_freeContext(sws_ctx);
    av_packet_free(&pkt);
    av_frame_free(&raw_frame);
    av_frame_free(&rgba_frame);
    avcodec_free_context(&dec_ctx);
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    std::println("[DISPLAY] Display thread stopping.");
}

int main() {
    ControlState control_state;
    std::array<std::unique_ptr<CameraPipeline>, NUM_CAMERAS> pipelines;

    std::println("[MAIN] Initializing CameraLibrary...");
    CameraLibraryStartup();
    CameraLibrary::CameraManager::X().ScanForCameras();

    std::println("[MAIN] Waiting for cameras to initialize...");
    auto start_time = std::chrono::steady_clock::now();
    CameraLibrary::CameraList cam_list;
    while (true) {
        CameraLibrary::CameraManager::X().GetCameraList(cam_list);
        if (cam_list.Count() >= NUM_CAMERAS) {
            std::println("[MAIN] Found {} cameras.", cam_list.Count());
            break;
        }
        if (std::chrono::steady_clock::now() - start_time >
            std::chrono::seconds(5)) {
            std::println(std::cerr,
                         "ERROR: Timed out waiting for cameras. Found {}.",
                         cam_list.Count());
            CameraLibraryShutdown();
            return -1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

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

            auto &entry = cam_list[i];

            p_raw->camera = CameraLibrary::CameraManager::X().GetCameraBySerial(
                entry.Serial());
            if (!p_raw->camera) {
                throw std::runtime_error("Failed to get camera by serial: " +
                                         std::to_string(entry.Serial()));
            }
            p_raw->serial_str = std::to_string(p_raw->camera->Serial());

            std::println("[MAIN] Setting up camera: {}", p_raw->serial_str);

            p_raw->camera->SetVideoType(Core::VideoMode);
            p_raw->camera->SetColorCompression(1, 0.4F, 0.30F);

            p_raw->camera->SetFrameRate(FPS);
            p_raw->actual_fps = p_raw->camera->FrameRate();
            std::println("[MAIN] Requested FPS {}, Camera {} set to {}", FPS,
                         p_raw->serial_str, p_raw->actual_fps);

            if (p_raw->actual_fps == 0) {
                throw std::runtime_error(
                    "Camera reported 0 FPS. Is it configured for VideoMode?");
            }

            consumer_ready_futures.push_back(
                consumer_ready_promises[i].get_future());
            threads.emplace_back(producer, p_raw, &control_state);
            threads.emplace_back(consumer, p_raw, &control_state,
                                 std::move(consumer_ready_promises[i]));
        }
    } catch (const std::exception &e) {
        std::println(std::cerr, "[MAIN] CRITICAL ERROR during camera init: {}",
                     e.what());
        control_state.running = false;
        CameraLibraryShutdown();
        return -1;
    }

    threads.emplace_back(display, pipeline_pointers, &control_state,
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
