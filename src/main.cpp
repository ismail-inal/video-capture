#define LIB_CAMERA_IMPLEMENTATION
#include "lib/camera.hpp"

#include <array>
#include <atomic>
#include <boost/lockfree/spsc_queue.hpp>
#include <filesystem>
#include <future>
#include <iostream>
#include <memory>
#include <print>
#include <thread>
#include <vector>

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

// --- Configuration ---
constexpr const u32 WIDTH = 1920;
constexpr const u32 HEIGHT = 1080;
constexpr const u32 CHANNEL = 1;
constexpr const u32 FRAME_SIZE = WIDTH * HEIGHT * CHANNEL;

constexpr const i32 FPS = 250;
constexpr const u32 SECONDS = 1;
constexpr const u32 FRAME_NUM = FPS * SECONDS;
constexpr const u32 ARENA_SIZE = FRAME_NUM * FRAME_SIZE;
constexpr const u32 DISPLAY_HERTZ = 30;

constexpr const i32 CAMERA_EXPOSURE = 2500;
constexpr const i32 CAMERA_GAIN = 4;

// SEPARATE FLAGS FOR SAFETY
std::atomic<bool> g_app_running = true;     // Main loop control
std::atomic<bool> g_capture_active = false; // Start/Stop cameras
std::atomic<bool> g_paused = false;
std::atomic<u8 *> g_live_display_ptr{nullptr};

struct FrameMetadata {
    u64 pts;
    u64 dts;
    u32 flags;
    u32 reserved;
};

struct alignas(32) Arena {
    u8 data[ARENA_SIZE];
};

struct CameraContext {
    int id;
    int serial;
    camera::handle cam_handle;
    std::string output_filename;

    std::unique_ptr<Arena> arena;
    alignas(64) std::array<FrameMetadata, FRAME_NUM> frame_meta;

    boost::lockfree::spsc_queue<u16, boost::lockfree::capacity<FRAME_NUM>>
        free_q;
    boost::lockfree::spsc_queue<u16, boost::lockfree::capacity<FRAME_NUM>>
        ready_q;

    std::atomic<u16> latest_frame_id{FRAME_NUM};

    AVCodecContext *enc_ctx = nullptr;
    AVFrame *yuv_frame = nullptr;
    SwsContext *sws = nullptr;
    AVFormatContext *fmt_ctx = nullptr;
    AVStream *vid_stream = nullptr;

    CameraContext(int index, camera::handle h) : id(index), cam_handle(h) {
        serial = camera::get_serial(h);
        output_filename = std::format("output/cam_{}.mkv", serial);

        // ALLOCATE MEMORY (Safe)
        std::println("Allocating 500MB buffer for Cam {}...", serial);
        arena = std::make_unique<Arena>();

        for (usize i = 0; i < FRAME_NUM; ++i) {
            free_q.push(static_cast<u16>(i));
        }
    }
};

// --- Terminal Input ---
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
char get_key_nonblocking() {
    char ch = 0;
    if (read(STDIN_FILENO, &ch, 1) > 0)
        return ch;
    return 0;
}
#else
char get_key_nonblocking() {
    if (_kbhit())
        return _getch();
    return 0;
}
#endif

// --- Capture Thread ---
void run_capture(CameraContext *ctx) {
    // Wait for the "Go" signal OR the "Die" signal
    while (g_app_running && !g_capture_active) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (!g_app_running)
        return; // Exit if app is closing

    std::println("Cam {} ({}) Capture starting.", ctx->id, ctx->serial);
    camera::start(ctx->cam_handle);

    u64 start_hardware_id = 0;
    u64 last_hardware_id = 0;
    bool first_frame = true;
    u16 id;

    while (g_app_running.load(std::memory_order_relaxed)) {
        if (g_paused.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        if (!ctx->free_q.pop(id)) {
            std::this_thread::yield();
            continue;
        }

        u8 *src = ctx->arena->data + static_cast<usize>(id) * FRAME_SIZE;
        u64 hardware_id = 0;

        int retries = 0;
        while (g_app_running) {
            camera::get_frame(ctx->cam_handle, src, FRAME_SIZE, &hardware_id);

            if (first_frame) {
                start_hardware_id = hardware_id;
                last_hardware_id = hardware_id;
                first_frame = false;
                break;
            }

            if (hardware_id > last_hardware_id) {
                last_hardware_id = hardware_id;
                break;
            }

            std::this_thread::sleep_for(std::chrono::microseconds(100));
            retries++;
            if (retries > 2000)
                break;
        }

        u64 rel_pts = hardware_id - start_hardware_id;
        ctx->frame_meta[id].pts = rel_pts;
        ctx->frame_meta[id].dts = rel_pts;

        ctx->latest_frame_id.store(id, std::memory_order_release);

        while (!ctx->ready_q.push(id)) {
            if (!g_app_running)
                break;
        }
    }

    camera::stop(ctx->cam_handle);
}

// --- Encoding Thread ---
void run_encode(CameraContext *ctx, std::promise<bool> ready_promise) {
    const AVCodec *codec = avcodec_find_encoder_by_name("hevc_nvenc");
    if (!codec) {
        codec = avcodec_find_encoder(AV_CODEC_ID_HEVC);
        if (!codec) {
            std::println(std::cerr, "Cam {} Error: No HEVC encoder found.",
                         ctx->id);
            ready_promise.set_value(false);
            return;
        }
    }

    ctx->enc_ctx = avcodec_alloc_context3(codec);
    ctx->enc_ctx->bit_rate = 8000000;
    ctx->enc_ctx->width = WIDTH;
    ctx->enc_ctx->height = HEIGHT;
    ctx->enc_ctx->time_base = AVRational{1, FPS};
    ctx->enc_ctx->framerate = AVRational{FPS, 1};
    ctx->enc_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    ctx->enc_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    ctx->enc_ctx->gop_size = 60;
    ctx->enc_ctx->max_b_frames = 0;

    if (std::string(codec->name) == "hevc_nvenc") {
        av_opt_set(ctx->enc_ctx->priv_data, "preset", "fast", 0);
        av_opt_set(ctx->enc_ctx->priv_data, "rc", "cbr", 0);
    }

    if (avcodec_open2(ctx->enc_ctx, codec, nullptr) < 0) {
        std::println(std::cerr, "Cam {} Error: Could not open codec.", ctx->id);
        ready_promise.set_value(false);
        return;
    }

    avformat_alloc_output_context2(&ctx->fmt_ctx, nullptr, "matroska",
                                   ctx->output_filename.c_str());
    if (!ctx->fmt_ctx) {
        std::println(std::cerr, "Cam {} Error: Could not alloc format context.",
                     ctx->id);
        ready_promise.set_value(false);
        return;
    }

    ctx->vid_stream = avformat_new_stream(ctx->fmt_ctx, nullptr);
    ctx->vid_stream->id = ctx->fmt_ctx->nb_streams - 1;
    avcodec_parameters_from_context(ctx->vid_stream->codecpar, ctx->enc_ctx);
    ctx->vid_stream->time_base = ctx->enc_ctx->time_base;

    if (!(ctx->fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&ctx->fmt_ctx->pb, ctx->output_filename.c_str(),
                      AVIO_FLAG_WRITE) < 0) {
            std::println(std::cerr, "Cam {} Error: Could not open file '{}'.",
                         ctx->id, ctx->output_filename);
            ready_promise.set_value(false);
            return;
        }
    }

    if (avformat_write_header(ctx->fmt_ctx, nullptr) < 0) {
        ready_promise.set_value(false);
        return;
    }

    AVPixelFormat input_fmt =
        (CHANNEL == 1) ? AV_PIX_FMT_GRAY8 : AV_PIX_FMT_RGBA;
    ctx->sws = sws_getContext(WIDTH, HEIGHT, input_fmt, WIDTH, HEIGHT,
                              AV_PIX_FMT_YUV420P, SWS_BILINEAR, nullptr,
                              nullptr, nullptr);

    ctx->yuv_frame = av_frame_alloc();
    ctx->yuv_frame->format = ctx->enc_ctx->pix_fmt;
    ctx->yuv_frame->width = WIDTH;
    ctx->yuv_frame->height = HEIGHT;
    av_frame_get_buffer(ctx->yuv_frame, 32);

    AVPacket *pkt = av_packet_alloc();
    ready_promise.set_value(true);

    // Wait for start
    while (g_app_running && !g_capture_active) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    while (g_app_running.load(std::memory_order_acquire)) {
        u16 id;
        if (!ctx->ready_q.pop(id)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        const u8 *src_data =
            ctx->arena->data + static_cast<usize>(id) * FRAME_SIZE;
        const u8 *src_slice[1] = {src_data};
        i32 src_stride[1] = {static_cast<i32>(WIDTH * CHANNEL)};

        sws_scale(ctx->sws, src_slice, src_stride, 0, HEIGHT,
                  ctx->yuv_frame->data, ctx->yuv_frame->linesize);

        ctx->yuv_frame->pts = static_cast<i64>(ctx->frame_meta[id].pts);

        if (avcodec_send_frame(ctx->enc_ctx, ctx->yuv_frame) >= 0) {
            while (avcodec_receive_packet(ctx->enc_ctx, pkt) == 0) {
                av_packet_rescale_ts(pkt, ctx->enc_ctx->time_base,
                                     ctx->vid_stream->time_base);
                pkt->stream_index = ctx->vid_stream->index;
                av_interleaved_write_frame(ctx->fmt_ctx, pkt);
                av_packet_unref(pkt);
            }
        }

        while (!ctx->free_q.push(id)) {
            if (!g_app_running)
                break;
        }
    }

    // Flush
    avcodec_send_frame(ctx->enc_ctx, nullptr);
    while (avcodec_receive_packet(ctx->enc_ctx, pkt) == 0) {
        av_packet_rescale_ts(pkt, ctx->enc_ctx->time_base,
                             ctx->vid_stream->time_base);
        pkt->stream_index = ctx->vid_stream->index;
        av_interleaved_write_frame(ctx->fmt_ctx, pkt);
        av_packet_unref(pkt);
    }

    av_write_trailer(ctx->fmt_ctx);
    if (!(ctx->fmt_ctx->oformat->flags & AVFMT_NOFILE))
        avio_closep(&ctx->fmt_ctx->pb);
    avformat_free_context(ctx->fmt_ctx);
    av_packet_free(&pkt);
    av_frame_free(&ctx->yuv_frame);
    sws_freeContext(ctx->sws);
    avcodec_free_context(&ctx->enc_ctx);
}

// --- Display Thread ---
void run_display(std::promise<bool> ready_promise) {
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        std::println(std::cerr, "SDL Init Failed: {}", SDL_GetError());
        ready_promise.set_value(false);
        return;
    }
    SDL_Window *window = SDL_CreateWindow(
        "PrimeX Monitor", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        WIDTH / 2, HEIGHT / 2, SDL_WINDOW_SHOWN);
    SDL_Renderer *renderer =
        SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);

    SDL_Texture *texture =
        SDL_CreateTexture(renderer, SDL_PIXELFORMAT_YV12,
                          SDL_TEXTUREACCESS_STREAMING, WIDTH, HEIGHT);

    std::vector<u8> dummy_uv((WIDTH / 2) * (HEIGHT / 2), 0x80);

    ready_promise.set_value(true);

    while (g_app_running && !g_capture_active) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    while (g_app_running.load(std::memory_order_relaxed)) {
        u8 *ptr = g_live_display_ptr.load(std::memory_order_relaxed);

        if (ptr) {
            if (CHANNEL == 1) {
                SDL_UpdateYUVTexture(texture, nullptr, ptr, WIDTH,
                                     dummy_uv.data(), WIDTH / 2,
                                     dummy_uv.data(), WIDTH / 2);
            } else {
                SDL_UpdateTexture(texture, nullptr, ptr, WIDTH * 4);
            }
            SDL_RenderCopy(renderer, texture, nullptr, nullptr);
            SDL_RenderPresent(renderer);
        }

        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT)
                g_app_running = false;
        }

        std::this_thread::sleep_for(
            std::chrono::milliseconds(1000 / DISPLAY_HERTZ));
    }

    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
}

// --- Main Entry ---
int main() {
    try {
        std::filesystem::create_directories("output");
    } catch (const std::exception &e) {
        std::println(std::cerr, "ERROR: Output dir creation failed: {}",
                     e.what());
        return 1;
    }

    auto cam_handles = camera::init();
    if (cam_handles.size() < 3) {
        std::println(std::cerr, "ERROR: Need 3 cameras, found {}",
                     cam_handles.size());
        if (cam_handles.empty())
            return 1;
    }

    std::println("Configuring Cameras...");
    for (auto h : cam_handles) {
        camera::set_format(h, (CHANNEL == 1) ? camera::GRAY8 : camera::RGBA32);
        camera::set_ir_filter(h, true);
        camera::set_ir_illumination(h, false);
        camera::set_exposure(h, CAMERA_EXPOSURE);
        camera::set_gain(h, CAMERA_GAIN);
    }

    camera::wait_for_sync(5000);

    // Context Creation with Memory Safety Check
    std::vector<std::unique_ptr<CameraContext>> contexts;
    try {
        for (size_t i = 0; i < cam_handles.size(); ++i) {
            if (i >= 3)
                break;
            contexts.push_back(
                std::make_unique<CameraContext>(i, cam_handles[i]));
        }
    } catch (const std::bad_alloc &e) {
        std::println(std::cerr,
                     "CRITICAL ERROR: Memory allocation failed! (System OOM). "
                     "Reduce buffers/FPS. Error: {}",
                     e.what());
        return 1;
    }

    g_app_running = true;
    g_capture_active = false; // Hold threads

    std::vector<std::thread> pool;
    std::vector<std::future<bool>> display_futures;
    std::vector<std::future<bool>> encoder_futures;

    std::promise<bool> disp_prom;
    display_futures.push_back(disp_prom.get_future());
    pool.emplace_back(run_display, std::move(disp_prom));

    for (auto &ctx : contexts) {
        pool.emplace_back(run_capture, ctx.get());

        std::promise<bool> enc_prom;
        encoder_futures.push_back(enc_prom.get_future());
        pool.emplace_back(run_encode, ctx.get(), std::move(enc_prom));
    }

    // Verify Initialization
    bool all_ok = true;
    if (!display_futures[0].get()) {
        std::println(std::cerr, "Display init failed.");
        all_ok = false;
    }
    for (auto &fut : encoder_futures) {
        if (!fut.get()) {
            std::println(std::cerr, "Encoder init failed.");
            all_ok = false;
        }
    }

    if (!all_ok) {
        std::println(std::cerr, "Initialization failed. Shutting down...");
        g_app_running = false;
        for (auto &t : pool)
            if (t.joinable())
                t.join();
        return 1;
    }

    // --- START SIGNAL ---
    std::println("System Ready. Starting Capture...");
    g_capture_active = true; // Release threads to start

#ifndef _WIN32
    RawTerm term_guard;
    fcntl(STDIN_FILENO, F_SETFL, fcntl(STDIN_FILENO, F_GETFL) | O_NONBLOCK);
#endif

    std::println("Controls: [1-3] Switch View, [P] Pause, [Q] Quit");
    int active_idx = 0;

    while (g_app_running) {
        char c = get_key_nonblocking();

        if (c == 'q')
            g_app_running = false;
        if (c == 'p') {
            g_paused = !g_paused;
            std::println("Paused: {}", (bool)g_paused);
        }
        if (c >= '1' && c <= '3') {
            size_t idx = static_cast<size_t>(c - '1');
            if (idx < contexts.size()) {
                active_idx = static_cast<int>(idx);
                std::println("Active: Cam {} (Serial {})", idx,
                             contexts[idx]->serial);
            }
        }

        if (active_idx < contexts.size()) {
            u16 frame_id = contexts[active_idx]->latest_frame_id.load(
                std::memory_order_acquire);
            if (frame_id < FRAME_NUM) {
                u8 *ptr = contexts[active_idx]->arena->data +
                          (static_cast<usize>(frame_id) * FRAME_SIZE);
                g_live_display_ptr.store(ptr, std::memory_order_relaxed);
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    std::println("Stopping threads...");
    g_app_running = false;   // Ensure threads exit loop
    g_capture_active = true; // Ensure threads aren't stuck in wait state

    for (auto &t : pool)
        if (t.joinable())
            t.join();

    // Explicit deinit is usually safe here because shared_ptrs are still valid
    // but typically CameraManager handles shutdown. We can call Stop just to be
    // polite.
    for (auto &ctx : contexts)
        camera::deinit(ctx->cam_handle);

    std::println("Shutdown complete.");
    return 0;
}
