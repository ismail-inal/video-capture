#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <condition_variable>

// --- OptiTrack SDK Includes ---
#include "camera.h"
#include "cameralibrary.h"
#include "cameramanager.h"
#include "cameratypes.h"
#include "frame.h"
#include "synchronizer.h"

#define SDL_MAIN_HANDLED
extern "C" {
#include <SDL2/SDL.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/time.h>
#include <libswscale/swscale.h>
}

struct CompressedPacket {
    std::vector<uint8_t> data;
    int64_t pts;

    CompressedPacket() : pts(0) { data.reserve(256 * 1024); }
    CompressedPacket(CompressedPacket &&other) noexcept
        : data(std::move(other.data)), pts(other.pts) {}
    CompressedPacket &operator=(CompressedPacket &&other) noexcept {
        if (this != &other) {
            data = std::move(other.data);
            pts = other.pts;
        }
        return *this;
    }
    CompressedPacket(const CompressedPacket &) = delete;
    CompressedPacket &operator=(const CompressedPacket &) = delete;
};

#define ARENA_DEPTH 256

template <typename T> class Arena {
    std::vector<T> buffer;
    std::atomic<uint64_t> producer_idx{0};
    std::atomic<uint64_t> consumer_idx{0};
    std::mutex mtx;
    std::condition_variable producer_cv;
    std::condition_variable consumer_cv;

    static constexpr uint64_t MASK = ARENA_DEPTH - 1;
    static_assert((ARENA_DEPTH & MASK) == 0,
                  "ARENA_DEPTH must be a power of 2");

  public:
    Arena() : buffer(ARENA_DEPTH) {}

    std::unique_ptr<T> get_for_producer(std::atomic<bool> &running) {
        uint64_t p_idx = producer_idx.load(std::memory_order_relaxed);
        uint64_t c_idx = consumer_idx.load(std::memory_order_acquire);

        if (p_idx - c_idx >= ARENA_DEPTH) {
            std::unique_lock<std::mutex> lock(mtx);
            producer_cv.wait(lock, [&] {
                return !running.load() ||
                       (producer_idx.load(std::memory_order_relaxed) -
                        consumer_idx.load(std::memory_order_acquire)) <
                           ARENA_DEPTH;
            });
        }

        if (!running)
            return nullptr;
        return std::make_unique<T>();
    }

    void push(std::unique_ptr<T> packet) {
        uint64_t p_idx = producer_idx.load(std::memory_order_relaxed);
        buffer[p_idx & MASK] = std::move(*packet);
        producer_idx.store(p_idx + 1, std::memory_order_release);
        consumer_cv.notify_one();
    }

    std::unique_ptr<T> pop(std::atomic<bool> &running) {
        uint64_t c_idx = consumer_idx.load(std::memory_order_relaxed);
        uint64_t p_idx = producer_idx.load(std::memory_order_acquire);

        if (c_idx >= p_idx) {
            std::unique_lock<std::mutex> lock(mtx);
            consumer_cv.wait(lock, [&] {
                return !running.load() ||
                       (consumer_idx.load(std::memory_order_relaxed) <
                        producer_idx.load(std::memory_order_acquire));
            });
        }

        if (!running)
            return nullptr;

        c_idx = consumer_idx.load(std::memory_order_relaxed);
        p_idx = producer_idx.load(std::memory_order_acquire);
        if (c_idx >= p_idx)
            return nullptr;

        auto packet = std::make_unique<T>(std::move(buffer[c_idx & MASK]));
        consumer_idx.store(c_idx + 1, std::memory_order_release);
        producer_cv.notify_one();
        return packet;
    }
};

// --- Muxer (Recording) Thread ---

struct MuxerParams {
    std::string filename;
    int width;
    int height;
    uint32_t time_freq;
};

void muxer_consumer(MuxerParams params, Arena<CompressedPacket> *arena,
                    std::atomic<bool> &running) {
    AVFormatContext *ofmt_ctx = nullptr;

    std::cout << "[Muxer " << params.filename << "] Starting..." << std::endl;

    do {
        AVStream *out_stream = nullptr;
        int ret;

        avformat_alloc_output_context2(&ofmt_ctx, NULL, "mkv",
                                       params.filename.c_str());
        if (!ofmt_ctx) {
            std::cerr << "Could not create output context for "
                      << params.filename << std::endl;
            running = false;
            break;
        }

        out_stream = avformat_new_stream(ofmt_ctx, NULL);
        if (!out_stream) {
            std::cerr << "Failed to allocate output stream for "
                      << params.filename << std::endl;
            running = false;
            break;
        }

        out_stream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
        out_stream->codecpar->codec_id = AV_CODEC_ID_H264;
        out_stream->codecpar->width = params.width;
        out_stream->codecpar->height = params.height;
        out_stream->time_base = {1, (int)params.time_freq};

        av_dump_format(ofmt_ctx, 0, params.filename.c_str(), 1);

        if (!(ofmt_ctx->oformat->flags & AVFMT_NOFILE)) {
            ret = avio_open(&ofmt_ctx->pb, params.filename.c_str(),
                            AVIO_FLAG_WRITE);
            if (ret < 0) {
                std::cerr << "Could not open output file '" << params.filename
                          << "'" << std::endl;
                running = false;
                break;
            }
        }

        ret = avformat_write_header(ofmt_ctx, NULL);
        if (ret < 0) {
            std::cerr << "Error occurred when opening output file: "
                      << params.filename << std::endl;
            running = false;
            break;
        }

        std::cout << "[Muxer " << params.filename << "] Recording..."
                  << std::endl;

        while (running) {
            auto packet = arena->pop(running);
            if (!packet || !running)
                break;

            AVPacket pkt = {0};
            pkt.data = packet->data.data();
            pkt.size = packet->data.size();
            pkt.stream_index = 0;
            pkt.pts = packet->pts;
            pkt.dts = packet->pts;

            ret = av_interleaved_write_frame(ofmt_ctx, &pkt);
            if (ret < 0) {
                std::cerr << "Error while writing frame to " << params.filename
                          << std::endl;
                break;
            }
        }

        std::cout << "[Muxer " << params.filename << "] Stopping..."
                  << std::endl;
        av_write_trailer(ofmt_ctx);

    } while (false);

    if (ofmt_ctx && !(ofmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        avio_closep(&ofmt_ctx->pb);
    }
    avformat_free_context(ofmt_ctx);
    std::cout << "[Muxer " << params.filename << "] Stopped." << std::endl;
}

// --- Frame Producer (OptiTrack) Thread ---

void frame_producer(CameraLibrary::Camera *cam, Arena<CompressedPacket> *arena,
                    std::atomic<bool> &running) {
    std::cout << "[Producer " << cam->Serial() << "] Starting..." << std::endl;

    while (running) {
        // cam->NextFrame() blocks until a frame is ready or cam->Stop() is
        // called
        auto frame = cam->NextFrame();
        if (!frame || !running) {
            break; // Exit thread
        }

        int size = cam->CompressedImageSize(frame.get());
        if (size <= 0)
            continue;

        auto packet = arena->get_for_producer(running);
        if (!packet || !running)
            break;

        packet->data.resize(size);
        cam->CompressedImage(frame.get(), packet->data.data(), size);
        packet->pts = frame->HardwareTimeStamp();
        arena->push(std::move(packet));
    }
    std::cout << "[Producer " << cam->Serial() << "] Stopped." << std::endl;
}

// --- Display Function (now run by main thread) ---

void display_loop(std::vector<std::shared_ptr<CameraLibrary::Camera>> &cameras,
                  std::atomic<int> &active_index, std::atomic<bool> &running,
                  int width, int height) {
    SDL_Window *window = nullptr;
    SDL_Renderer *renderer = nullptr;
    SDL_Texture *texture = nullptr;

    std::vector<AVCodecContext *> decoders;
    std::vector<SwsContext *> sws_contexts;
    std::vector<AVFrame *> decoded_frames;
    AVFrame *rgba_frame = nullptr;
    uint8_t *rgba_buffer = nullptr;

    do {
        if (SDL_Init(SDL_INIT_VIDEO) < 0) {
            std::cerr << "SDL could not initialize! SDL_Error: "
                      << SDL_GetError() << std::endl;
            running = false;
            break;
        }
        window = SDL_CreateWindow(
            "OptiTrack H.264 Viewer", SDL_WINDOWPOS_UNDEFINED,
            SDL_WINDOWPOS_UNDEFINED, width, height, SDL_WINDOW_SHOWN);
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
        texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
                                    SDL_TEXTUREACCESS_STREAMING, width, height);

        for (size_t i = 0; i < cameras.size(); ++i) {
            const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
            if (!codec) {
                std::cerr << "H.264 decoder not found" << std::endl;
                running = false;
                break;
            }
            AVCodecContext *ctx = avcodec_alloc_context3(codec);
            if (avcodec_open2(ctx, codec, NULL) < 0) {
                std::cerr << "Could not open H.264 decoder for camera " << i
                          << std::endl;
                running = false;
                break;
            }
            decoders.push_back(ctx);

            SwsContext *sws_ctx =
                sws_getContext(width, height, AV_PIX_FMT_YUV420P, width, height,
                               AV_PIX_FMT_RGBA, SWS_BILINEAR, NULL, NULL, NULL);
            sws_contexts.push_back(sws_ctx);
            decoded_frames.push_back(av_frame_alloc());
        }
        if (!running)
            break;

        rgba_frame = av_frame_alloc();
        if (!rgba_frame) {
            std::cerr << "Could not allocate rgba_frame" << std::endl;
            running = false;
            break;
        }
        int num_bytes =
            av_image_get_buffer_size(AV_PIX_FMT_RGBA, width, height, 32);
        rgba_buffer = (uint8_t *)av_malloc(num_bytes);
        if (!rgba_buffer) {
            std::cerr << "Could not allocate rgba_buffer" << std::endl;
            running = false;
            break;
        }
        av_image_fill_arrays(rgba_frame->data, rgba_frame->linesize,
                             rgba_buffer, AV_PIX_FMT_RGBA, width, height, 32);

        std::cout
            << "[Display] Starting... Press 1, 2, 3 to switch. Press Q to quit."
            << std::endl;

        while (running) {
            SDL_Event e;
            while (SDL_PollEvent(&e) != 0) {
                if (e.type == SDL_QUIT) {
                    running = false;
                } else if (e.type == SDL_KEYDOWN) {
                    switch (e.key.keysym.sym) {
                    case SDLK_1:
                        if (cameras.size() >= 1)
                            active_index = 0;
                        break;
                    case SDLK_2:
                        if (cameras.size() >= 2)
                            active_index = 1;
                        break;
                    case SDLK_3:
                        if (cameras.size() >= 3)
                            active_index = 2;
                        break;
                    case SDLK_q:
                        running = false;
                        break;
                    }
                }
            }
            if (!running)
                break;

            int idx = active_index.load(std::memory_order_relaxed);
            auto cam = cameras[idx];
            auto decoder_ctx = decoders[idx];
            auto sws_ctx = sws_contexts[idx];
            auto yuv_frame = decoded_frames[idx];

            auto frame = cam->LatestFrame();
            if (!frame) {
                SDL_Delay(2);
                continue;
            }

            int size = cam->CompressedImageSize(frame.get());
            if (size <= 0)
                continue;

            AVPacket pkt = {0};
            std::vector<uint8_t> h264_buffer(size);
            cam->CompressedImage(frame.get(), h264_buffer.data(), size);
            pkt.data = h264_buffer.data();
            pkt.size = size;

            int ret = avcodec_send_packet(decoder_ctx, &pkt);
            if (ret < 0)
                continue;

            ret = avcodec_receive_frame(decoder_ctx, yuv_frame);
            if (ret == 0) {
                sws_scale(sws_ctx, (const uint8_t *const *)yuv_frame->data,
                          yuv_frame->linesize, 0, yuv_frame->height,
                          rgba_frame->data, rgba_frame->linesize);
                SDL_UpdateTexture(texture, NULL, rgba_frame->data[0],
                                  rgba_frame->linesize[0]);
                SDL_RenderClear(renderer);
                SDL_RenderCopy(renderer, texture, NULL, NULL);
                SDL_RenderPresent(renderer);
            }
            SDL_Delay(2);
        }

    } while (false);

    std::cout << "[Display] Stopping..." << std::endl;
    for (auto &frame : decoded_frames)
        av_frame_free(&frame);
    for (auto &ctx : sws_contexts)
        sws_freeContext(ctx);
    for (auto &ctx : decoders)
        avcodec_free_context(&ctx);

    av_free(rgba_buffer);
    av_frame_free(&rgba_frame);

    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    std::cout << "[Display] Stopped." << std::endl;
}

// --- Main Function ---

int main() {
    std::cout << "Starting OptiTrack Capture..." << std::endl;

    std::vector<std::shared_ptr<CameraLibrary::Camera>> cameras;
    std::vector<std::unique_ptr<Arena<CompressedPacket>>> arenas;
    std::vector<std::thread> threads;
    std::atomic<bool> running = true;
    std::atomic<int> g_active_display_index = 0;

    uint32_t hardware_time_freq = 0;
    const int CAM_WIDTH = 1920;
    const int CAM_HEIGHT = 1080;

    // 1. Initialize OptiTrack SDK
    CameraLibraryStartup();
    auto &manager = CameraLibrary::CameraManager::X();
    manager.ScanForCameras();

    // *** Scan Time Fix: Increased to 5 seconds ***
    std::cout << "Scanning for cameras... (waiting 5s)" << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(5));

    CameraLibrary::CameraList list;
    manager.GetCameraList(list);

    if (list.Count() < 3) {
        std::cerr << "Error: Found " << list.Count()
                  << " cameras, but 3 are required." << std::endl;
        CameraLibraryShutdown();
        return -1;
    }

    std::cout << "Found " << list.Count() << " cameras." << std::endl;

    // 2. Apply Hardware Synchronization
    try {
        CameraLibrary::sSyncSettings settings;
        manager.GetSyncSettings(settings);
        settings.Mode = CameraLibrary::SyncModeCustom;
        settings.SyncType = CameraLibrary::SyncTypeWiredSync;
        settings.SyncInputSource =
            CameraLibrary::SyncInputSourcePTPInput; // PTP for Ethernet sync
        manager.ApplySyncSettings(settings);
        std::cout << "Hardware synchronization set to PTP." << std::endl;
    } catch (...) {
        std::cerr << "Warning: Could not apply hardware sync settings."
                  << std::endl;
    }

    // 3. Configure and Start Cameras
    for (int i = 0; i < 3; ++i) {
        auto cam = manager.GetCameraBySerial(list[i].Serial());
        if (!cam) {
            std::cerr << "Error: Could not get camera " << list[i].Serial()
                      << std::endl;
            running = false;
            break;
        }

        std::cout << "Configuring camera " << cam->Serial() << "..."
                  << std::endl;

        // *** E4 Error Fix: Set H.264 mode AND add compression/exposure
        // settings ***
        cam->SetVideoType(Core::VideoMode);
        // These values are from the SDK sample `QtCameraViewer.cpp`
        cam->SetColorCompression(1, 0.4F, 0.30F); // Mode, Quality, BitRate
        cam->SetExposure(
            3000); // 250 FPS is ~4000us max exposure. 3000 is a safe value.
        cam->SetImagerGain(static_cast<CameraLibrary::eImagerGain>(3));

        cam->Start();

        cameras.push_back(cam);
        arenas.push_back(std::make_unique<Arena<CompressedPacket>>());

        if (i == 0) {
            std::shared_ptr<const CameraLibrary::Frame> frame;
            std::cout << "Waiting for first frame to get hardware clock..."
                      << std::endl;
            auto start_time = std::chrono::steady_clock::now();
            while (!frame && running &&
                   std::chrono::steady_clock::now() - start_time <
                       std::chrono::seconds(5)) {
                frame = cam->LatestFrame();
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            if (frame) {
                hardware_time_freq = frame->HardwareTimeFreq();
                if (hardware_time_freq == 0) {
                    std::cerr << "Warning: Hardware time frequency is 0. "
                                 "Assuming 90000 Hz."
                              << std::endl;
                    hardware_time_freq = 90000;
                } else {
                    std::cout
                        << "Hardware time frequency: " << hardware_time_freq
                        << " Hz" << std::endl;
                }
            } else {
                std::cerr << "Failed to get initial frame. Assuming 90000 Hz."
                          << std::endl;
                hardware_time_freq = 90000;
            }
        }
    }

    if (!running) {
        for (auto &cam : cameras)
            cam->Stop();
        CameraLibraryShutdown();
        return -1;
    }

    // 4. Launch Producer and Muxer threads
    for (int i = 0; i < 3; ++i) {
        MuxerParams params;
        params.filename = std::to_string(cameras[i]->Serial()) + ".mkv";
        params.width = CAM_WIDTH;
        params.height = CAM_HEIGHT;
        params.time_freq = hardware_time_freq;

        threads.emplace_back(frame_producer, cameras[i].get(), arenas[i].get(),
                             std::ref(running));
        threads.emplace_back(muxer_consumer, params, arenas[i].get(),
                             std::ref(running));
    }

    // 5. Run Display Loop in Main Thread
    display_loop(cameras, g_active_display_index, running, CAM_WIDTH,
                 CAM_HEIGHT);

    // 6. Stop cameras to unblock producer threads
    std::cout << "Shutting down all cameras..." << std::endl;
    for (auto &cam : cameras) {
        cam->Stop();
    }

    // 7. Wait for all threads to finish
    std::cout << "Waiting for threads to join..." << std::endl;
    for (auto &t : threads) {
        t.join();
    }

    // 8. Final Cleanup
    CameraLibraryShutdown();
    std::cout << "Shutdown complete. Exiting." << std::endl;

    return 0;
}
