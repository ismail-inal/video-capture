#include "lib/camera.hpp"
#include <array>
#include <atomic>
#include <boost/lockfree/spsc_queue.hpp>
#include <chrono>
#include <future>
#include <iostream>
#include <memory>
#include <print>
#include <thread>

#ifdef _WIN32
#include <conio.h>
#else
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#endif

#include <SDL2/SDL.h>

extern "C" {
#include "lib/types.h"
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

constexpr const u32 WIDTH = 1920;
constexpr const u32 HEIGHT = 1080;
constexpr const u32 CHANNEL = 4;
constexpr const u32 FRAME_SIZE = WIDTH * HEIGHT * CHANNEL;

constexpr const i32 FPS = 120;
constexpr const u32 SECONDS = 3;
constexpr const u32 FRAME_NUM = FPS * SECONDS;
constexpr const u32 ARENA_SIZE = FRAME_NUM * FRAME_SIZE;
constexpr const char *OUTFILE = "output/out.h265";

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

std::atomic<bool> running = false;
std::atomic<bool> paused = false;
std::atomic<u16> g_latest_display_frame_id{FRAME_NUM};

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

int main() {
    auto arena = std::make_unique<Arena>();
    alignas(64) static std::array<FrameMetadata, FRAME_NUM> frame_meta;

    namespace blf = boost::lockfree;
    blf::spsc_queue<u16, blf::capacity<FRAME_NUM>> free_q;
    blf::spsc_queue<u16, blf::capacity<FRAME_NUM>> ready_q;

    for (usize i = 0; i < FRAME_NUM; ++i) {
        free_q.push(static_cast<u16>(i));
    }

    camera::CameraHandle camera_handle = camera::init()[0];
    camera::set_size(camera_handle, WIDTH, HEIGHT);
    camera::set_format(camera_handle, camera::RGBA32);
    camera::set_fps(camera_handle, FPS);

    std::promise<bool> consumer_ready_promise;
    std::promise<bool> display_ready_promise;
    std::future<bool> consumer_ready_future =
        consumer_ready_promise.get_future();
    std::future<bool> display_ready_future = display_ready_promise.get_future();

    std::thread producer([&]() {
        while (!running.load(std::memory_order_acquire)) {
            if (std::this_thread::get_id() == std::thread::id())
                return;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        std::println("Producer thread starting capture.");

        u64 pts_counter = 0;
        u16 id;
        u8 *src = nullptr;
        while (running) {
            if (paused) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }

            if (!free_q.pop(id)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }

            src = arena->data + static_cast<usize>(id) * FRAME_SIZE;
            camera::get_frame(camera_handle, src, FRAME_SIZE);

            frame_meta[id].pts = pts_counter;
            frame_meta[id].dts = pts_counter;
            frame_meta[id].flags = 0;

            while (!ready_q.push(id)) {
                if (!running)
                    break;
            }

            ++pts_counter;
        }
    });

    std::thread consumer([&, ready_promise =
                                 std::move(consumer_ready_promise)]() mutable {
        const AVCodec *codec = avcodec_find_encoder_by_name("hevc_nvenc");
        if (!codec) {
            std::println(std::cerr,
                         "ERROR: NVENC encoder 'hevc_nvenc' not found.");
            ready_promise.set_value(false);
            return;
        }

        AVCodecContext *enc_ctx = avcodec_alloc_context3(codec);
        if (!enc_ctx) {
            std::println(std::cerr, "ERROR: avcodec_alloc_context3 failed");
            ready_promise.set_value(false);
            return;
        }

        enc_ctx->bit_rate = 8000000;
        enc_ctx->width = WIDTH;
        enc_ctx->height = HEIGHT;
        enc_ctx->time_base = AVRational{1, FPS};
        enc_ctx->framerate = AVRational{FPS, 1};
        enc_ctx->gop_size = 60;
        enc_ctx->max_b_frames = 0;
        enc_ctx->pix_fmt = AV_PIX_FMT_YUV420P;

        av_opt_set(enc_ctx->priv_data, "preset", "fast", 0);
        av_opt_set(enc_ctx->priv_data, "rc", "cbr", 0);
        av_opt_set(enc_ctx->priv_data, "profile", "main", 0);
        av_opt_set(enc_ctx->priv_data, "annexb", "1", 0);

        if (avcodec_open2(enc_ctx, codec, nullptr) < 0) {
            std::println(std::cerr, "ERROR: Could not open encoder");
            avcodec_free_context(&enc_ctx);
            ready_promise.set_value(false);
            return;
        }

        SwsContext *sws = sws_getContext(
            WIDTH, HEIGHT, AV_PIX_FMT_RGBA, WIDTH, HEIGHT, AV_PIX_FMT_YUV420P,
            SWS_BILINEAR, nullptr, nullptr, nullptr);

        if (!sws) {
            std::println(std::cerr, "ERROR: sws_getContext failed");
            avcodec_free_context(&enc_ctx);
            ready_promise.set_value(false);
            return;
        }

        AVFrame *yuv_frame = av_frame_alloc();
        if (!yuv_frame) {
            std::println(std::cerr, "ERROR: av_frame_alloc failed");
            sws_freeContext(sws);
            avcodec_free_context(&enc_ctx);
            ready_promise.set_value(false);
            return;
        }
        yuv_frame->format = enc_ctx->pix_fmt;
        yuv_frame->width = enc_ctx->width;
        yuv_frame->height = enc_ctx->height;
        if (av_frame_get_buffer(yuv_frame, 32) < 0) {
            std::println(std::cerr, "ERROR: av_frame_get_buffer failed");
            av_frame_free(&yuv_frame);
            sws_freeContext(sws);
            avcodec_free_context(&enc_ctx);
            ready_promise.set_value(false);
            return;
        }

        AVPacket *pkt = av_packet_alloc();
        if (!pkt) {
            std::println(std::cerr, "ERROR: av_packet_alloc failed");
            av_frame_free(&yuv_frame);
            sws_freeContext(sws);
            avcodec_free_context(&enc_ctx);
            ready_promise.set_value(false);
            return;
        }

        FILE *f = std::fopen(OUTFILE, "wb");
        if (!f) {
            std::println(std::cerr, "ERROR: Could not open output file '{}'",
                         OUTFILE);
            av_packet_free(&pkt);
            av_frame_free(&yuv_frame);
            sws_freeContext(sws);
            avcodec_free_context(&enc_ctx);
            ready_promise.set_value(false);
            return;
        }

        std::println("Consumer thread initialized successfully.");
        ready_promise.set_value(true);

        while (running) {
            u16 id;
            if (!ready_q.pop(id)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }

            g_latest_display_frame_id.store(id, std::memory_order_release);

            const FrameMetadata meta = frame_meta[id];
            const u8 *src_rgb =
                arena->data + static_cast<usize>(id) * FRAME_SIZE;

            const u8 *src_slice[1] = {src_rgb};
            i32 src_stride[1] = {static_cast<i32>(WIDTH * CHANNEL)};

            sws_scale(sws, src_slice, src_stride, 0, HEIGHT, yuv_frame->data,
                      yuv_frame->linesize);

            yuv_frame->pts = static_cast<i64>(meta.pts);

            if (avcodec_send_frame(enc_ctx, yuv_frame) < 0) {
                std::println(std::cerr,
                             "WARNING: avcodec_send_frame failed for pts={}",
                             meta.pts);
            } else {
                while (avcodec_receive_packet(enc_ctx, pkt) == 0) {
                    std::fwrite(pkt->data, 1, pkt->size, f);
                    av_packet_unref(pkt);
                }
            }

            while (!free_q.push(id)) {
                if (!running)
                    break;
            }
        }

        avcodec_send_frame(enc_ctx, nullptr);
        while (avcodec_receive_packet(enc_ctx, pkt) == 0) {
            std::fwrite(pkt->data, 1, pkt->size, f);
            av_packet_unref(pkt);
        }

        std::fclose(f);
        av_packet_free(&pkt);
        av_frame_free(&yuv_frame);
        sws_freeContext(sws);
        avcodec_free_context(&enc_ctx);
    });

    std::thread display([&, ready_promise =
                                std::move(display_ready_promise)]() mutable {
        if (SDL_Init(SDL_INIT_VIDEO) < 0) {
            std::println(std::cerr, "SDL could not initialize! SDL_Error: {}",
                         SDL_GetError());
            ready_promise.set_value(false);
            return;
        }

        SDL_Window *window = SDL_CreateWindow(
            "Live Preview", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
            WIDTH / 2, HEIGHT / 2, SDL_WINDOW_SHOWN);
        if (!window) {
            std::println(std::cerr,
                         "Window could not be created! SDL_Error: {}",
                         SDL_GetError());
            SDL_Quit();
            ready_promise.set_value(false);
            return;
        }

        SDL_Renderer *renderer =
            SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
        if (!renderer) {
            std::println(std::cerr,
                         "Renderer could not be created! SDL Error: {}",
                         SDL_GetError());
            SDL_DestroyWindow(window);
            SDL_Quit();
            ready_promise.set_value(false);
            return;
        }

        SDL_Texture *texture =
            SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
                              SDL_TEXTUREACCESS_STREAMING, WIDTH, HEIGHT);
        if (!texture) {
            std::println(std::cerr,
                         "Texture could not be created! SDL Error: {}",
                         SDL_GetError());
            SDL_DestroyRenderer(renderer);
            SDL_DestroyWindow(window);
            SDL_Quit();
            ready_promise.set_value(false);
            return;
        }

        std::println("Display thread initialized successfully.");
        ready_promise.set_value(true);

        while (running) {
            u16 frame_id_to_show =
                g_latest_display_frame_id.load(std::memory_order_acquire);

            if (frame_id_to_show < FRAME_NUM) {
                const u8 *frame_data =
                    arena->data +
                    static_cast<usize>(frame_id_to_show) * FRAME_SIZE;
                SDL_UpdateTexture(texture, nullptr, frame_data,
                                  WIDTH * CHANNEL);
                SDL_RenderCopy(renderer, texture, nullptr, nullptr);
                SDL_RenderPresent(renderer);
            }

            SDL_Event e;
            while (SDL_PollEvent(&e) != 0) {
                if (e.type == SDL_QUIT) {
                    running = false;
                }
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(
                static_cast<u32>(1000 / DISPLAY_HERTZ)));
        }

        SDL_DestroyTexture(texture);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
    });

    bool consumer_ok = consumer_ready_future.get();
    bool display_ok = display_ready_future.get();

    if (consumer_ok && display_ok) {
        std::println("All threads initialized. Starting main loop.");
        running = true;
        camera::start(camera_handle);
    } else {
        std::println(std::cerr, "ERROR: Thread initialization failed.");
    }

#ifndef _WIN32
    RawTerm term_guard;
    fcntl(STDIN_FILENO, F_SETFL, fcntl(STDIN_FILENO, F_GETFL) | O_NONBLOCK);
#endif

    if (running) {
        std::println("Press 'p' to pause/resume, 'q' to quit.");
    }

    while (running) {
        char ch = 0;
#ifdef _WIN32
        if (_kbhit()) {
            c = _getch();
        }
#else
        if (read(STDIN_FILENO, &ch, 1) <= 0) {
            ch = 0;
        }
#endif

        if (ch == 'p') {
            paused = !paused.load();
            std::println("{}", paused ? "Paused" : "Resumed");
            paused ? camera::stop(camera_handle) : camera::start(camera_handle);
        } else if (ch == 'q') {
            running = false;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    camera::shutdown(camera_handle);

    producer.join();
    consumer.join();
    display.join();

    if (consumer_ok && display_ok) {
        std::println("Finished. Output: {}", OUTFILE);
    } else {
        std::println("Finished with initialization errors.");
    }
    return 0;
}
