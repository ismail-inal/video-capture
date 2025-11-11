#include "cameralibrary.h"
#include "cameramodulebase.h" // <-- Required for cCameraModule
#include "frame.h"        // <-- Required for Frame

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
#include <cstdint> 

// Non-blocking key-press detection
#ifdef _WIN32
#include <conio.h>
#else
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

// RAII struct for non-blocking, non-echoing terminal
struct RawTerm {
    termios orig_termios;
    RawTerm() {
        tcgetattr(STDIN_FILENO, &orig_termios);
        termios raw = orig_termios;
        raw.c_lflag &= ~(ECHO | ICANON); // Disable echo and canonical mode
        raw.c_cc[VMIN] = 0;  // Read non-blocking
        raw.c_cc[VTIME] = 0; // No wait time
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
        fcntl(STDIN_FILENO, F_SETFL, fcntl(STDIN_FILENO, F_GETFL) | O_NONBLOCK);
    }
    ~RawTerm() { 
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios); 
        fcntl(STDIN_FILENO, F_SETFL, fcntl(STDIN_FILENO, F_GETFL) & ~O_NONBLOCK);
    }
};
#endif

#include <SDL2/SDL.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

using namespace CameraLibrary;

//== Configuration =========================================================

constexpr const int NUM_CAMERAS = 3;
constexpr const uint32_t WIDTH = 1920;
constexpr const uint32_t HEIGHT = 1080;
constexpr const int32_t FPS = 250;
constexpr const uint32_t SECONDS = 1;
constexpr const uint32_t FRAME_NUM = FPS * SECONDS;
constexpr const char *HW_DECODER_NAME = "h264_nvdec"; // Use "h264_videotoolbox" on macOS, "h264_vaapi" on Linux/VAAPI

//== Globals ===============================================================

std::atomic<bool> g_running{false};
cModuleSync* g_sync_module = nullptr;
std::vector<std::shared_ptr<Camera>> g_cameras;
std::array<std::string, NUM_CAMERAS> g_camera_serials;

// --- Queues ---
// 1. Distributor -> Remuxer Threads (Carries H.264 packets for file)
std::array<boost::lockfree::spsc_queue<AVPacket*, boost::lockfree::capacity<FRAME_NUM>>, NUM_CAMERAS> g_file_packet_q;

// 2. Distributor -> Display Threads (Carries H.264 packets for display)
std::array<boost::lockfree::spsc_queue<AVPacket*, boost::lockfree::capacity<FRAME_NUM>>, NUM_CAMERAS> g_display_packet_q;

// --- Packet Pool ---
boost::lockfree::spsc_queue<AVPacket*, boost::lockfree::capacity<FRAME_NUM * 6 + 128>> g_packet_pool;

//== Packet Pool Helpers ===================================================

void init_packet_pool() {
    std::println("Initializing AVPacket pool...");
    for (size_t i = 0; i < FRAME_NUM * 6; ++i) {
        g_packet_pool.push(av_packet_alloc());
    }
}

AVPacket* acquire_packet() {
    AVPacket* pkt = nullptr;
    if (g_packet_pool.pop(pkt)) {
        return pkt;
    }
    return av_packet_alloc();
}

void release_packet(AVPacket* pkt) {
    av_packet_unref(pkt);
    g_packet_pool.push(pkt);
}

//== Camera Module (Producer) ==============================================
// This class is our new "producer". An instance will be attached to each camera.

class H264FanoutModule : public cCameraModule {
private:
    int m_cam_index;
    // FIX: This module will own its own counter.
    std::atomic<uint64_t> m_pts_counter{0};

public:
    // FIX: Removed pts_counter reference
    H264FanoutModule(int camera_index)
        : m_cam_index(camera_index) {}

    // This is the callback from the SDK that provides the H.264 data
    virtual bool PostVideoData(Camera*, const unsigned char* Buffer, long bufferSize, [[maybe_unused]] const Frame* frame,
                               [[maybe_unused]] int frameWidth, [[maybe_unused]] int frameHeight, 
                               [[maybe_unused]] unsigned char* alignedFrameBuffer, [[maybe_unused]] long alignedFrameBufferSize) override 
    {
        if (!g_running.load(std::memory_order_relaxed) || !Buffer || bufferSize <= 0) {
            return true; // Stop processing if not running
        }

        // FIX: Use our internal PTS counter, not frame->Timestamp()
        const uint64_t pts = m_pts_counter.fetch_add(1, std::memory_order_relaxed);

        // Get two packets from the pool
        AVPacket* file_pkt = acquire_packet();
        AVPacket* display_pkt = acquire_packet();

        // Create a new data buffer and copy the frame data into it
        av_new_packet(file_pkt, bufferSize);
        std::memcpy(file_pkt->data, Buffer, bufferSize);

        // Share the data buffer between the two packets
        av_packet_ref(display_pkt, file_pkt);

        // Set timestamps
        file_pkt->pts = display_pkt->pts = pts;
        file_pkt->dts = display_pkt->dts = pts;

        // Push to file queue
        if (!g_file_packet_q[m_cam_index].push(file_pkt)) {
            std::println(std::cerr, "WARNING: File queue {} full, dropping packet.", m_cam_index);
            release_packet(file_pkt);
        }

        // Push to display queue
        if (!g_display_packet_q[m_cam_index].push(display_pkt)) {
            std::println(std::cerr, "WARNING: Display queue {} full, dropping packet.", m_cam_index);
            release_packet(display_pkt);
        }

        return true;
    }

    // This is a "dummy" override. We're not decoding, so we just return true.
    virtual bool PostVideoData([[maybe_unused]] Camera* cam, [[maybe_unused]] const unsigned char* Buffer, [[maybe_unused]] long bufferSize, 
                               [[maybe_unused]] const Frame* frame,
                               [[maybe_unused]] int frameWidth, [[maybe_unused]] int frameHeight, 
                               [[maybe_unused]] unsigned char* alignedFrameBuffer, [[maybe_unused]] long alignedFrameBufferSize, 
                               [[maybe_unused]] int pixelFormat) override
    {
        // This overload is for decoded data, which we're not handling.
        // We just call the other PostVideoData, which has the H.264 packet.
        return PostVideoData(cam, Buffer, bufferSize, frame, frameWidth, frameHeight, alignedFrameBuffer, alignedFrameBufferSize);
    }
};

//== Threads 1, 2, 3: Remuxers =============================================

void remuxer_thread(int cam_index) {
    const std::string serial = g_camera_serials[cam_index];
    const std::string filename = serial + ".mkv";
    std::println("Remuxer thread {} ({}) started. Output: {}", cam_index, serial, filename);

    AVFormatContext* fmt_ctx = nullptr;
    AVStream* out_stream = nullptr;
    int ret = 0;

    // 1. Setup Output Context
    avformat_alloc_output_context2(&fmt_ctx, nullptr, "mkv", filename.c_str());
    if (!fmt_ctx) {
        std::println(std::cerr, "ERROR [Remux {}]: Could not create output context.", cam_index);
        return;
    }

    out_stream = avformat_new_stream(fmt_ctx, nullptr);
    if (!out_stream) {
        std::println(std::cerr, "ERROR [Remux {}]: Could not create output stream.", cam_index);
        avformat_free_context(fmt_ctx);
        return;
    }

    // 2. Set Codec Parameters
    out_stream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    out_stream->codecpar->codec_id = AV_CODEC_ID_H264;
    out_stream->codecpar->width = WIDTH;
    out_stream->codecpar->height = HEIGHT;
    out_stream->codecpar->format = AV_PIX_FMT_YUV420P; // Assumed format for H.264

    // Set timebase for the output file
    // We use the high-resolution timestamp from the camera
    out_stream->time_base = AVRational{1, 1000000}; // Microsecond precision
    fmt_ctx->streams[0]->time_base = AVRational{1, 1000000};

    // 3. Open Output File
    if (!(fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&fmt_ctx->pb, filename.c_str(), AVIO_FLAG_WRITE);
        if (ret < 0) {
            std::println(std::cerr, "ERROR [Remux {}]: Could not open output file '{}'", cam_index, filename);
            avformat_free_context(fmt_ctx);
            return;
        }
    }

    // 4. Write Header
    ret = avformat_write_header(fmt_ctx, nullptr);
    if (ret < 0) {
        std::println(std::cerr, "ERROR [Remux {}]: Could not write header.", cam_index);
        avio_closep(&fmt_ctx->pb);
        avformat_free_context(fmt_ctx);
        return;
    }

    AVPacket* pkt = nullptr;
    while (g_running.load(std::memory_order_relaxed)) {
        if (!g_file_packet_q[cam_index].pop(pkt)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        pkt->stream_index = out_stream->index;
        pkt->pos = -1;
        // No PTS rescale needed as we set the timebase to match the frame's timestamp

        ret = av_interleaved_write_frame(fmt_ctx, pkt);
        if (ret < 0) {
            std::println(std::cerr, "WARNING [Remux {}]: Error writing frame.", cam_index);
        }

        release_packet(pkt);
    }

    // 5. Write Trailer and Close
    std::println("Remuxer thread {} stopping...", cam_index);
    av_write_trailer(fmt_ctx);
    if (!(fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        avio_closep(&fmt_ctx->pb);
    }
    avformat_free_context(fmt_ctx);
}

//== Threads 4, 5, 6: Display ==============================================

void display_thread(int cam_index) {
    const std::string serial = g_camera_serials[cam_index];
    const std::string title = "Camera " + serial;
    std::println("Display thread {} ({}) started.", cam_index, serial);

    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    SDL_Texture* texture = nullptr;
    const AVCodec* decoder = nullptr;
    AVCodecContext* dec_ctx = nullptr;
    AVFrame* frame = nullptr;

    // 1. Init SDL
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        std::println(std::cerr, "ERROR [Display {}]: SDL could not initialize: {}", cam_index, SDL_GetError());
        return;
    }

    window = SDL_CreateWindow(title.c_str(),
        SDL_WINDOWPOS_CENTERED + cam_index * 30, SDL_WINDOWPOS_CENTERED + cam_index * 30,
        WIDTH / 2, HEIGHT / 2, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!window) {
        std::println(std::cerr, "ERROR [Display {}]: Window could not be created: {}", cam_index, SDL_GetError());
        return;
    }

    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if (!renderer) {
        std::println(std::cerr, "ERROR [Display {}]: Renderer could not be created: {}", cam_index, SDL_GetError());
        return;
    }

    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_YV12, // YV12 is planar YUV 4:2:0
                                SDL_TEXTUREACCESS_STREAMING, WIDTH, HEIGHT);
    if (!texture) {
        std::println(std::cerr, "ERROR [Display {}]: Texture could not be created: {}", cam_index, SDL_GetError());
        return;
    }

    // 2. Init FFmpeg Decoder
    decoder = avcodec_find_decoder_by_name(HW_DECODER_NAME);
    if (!decoder) {
        std::println(std::cerr, "ERROR [Display {}]: HW Decoder '{}' not found. Falling back to default H.264.", cam_index, HW_DECODER_NAME);
        decoder = avcodec_find_decoder(AV_CODEC_ID_H264);
        if (!decoder) {
            std::println(std::cerr, "ERROR [Display {}]: Could not find any H.264 decoder.", cam_index);
            return;
        }
    }

    dec_ctx = avcodec_alloc_context3(decoder);
    if (!dec_ctx) {
        std::println(std::cerr, "ERROR [Display {}]: Could not allocate decoder context.", cam_index);
        return;
    }
    
    // Set timebase for the decoder
    dec_ctx->time_base = AVRational{1, 1000000}; // Microsecond precision

    if (avcodec_open2(dec_ctx, decoder, nullptr) < 0) {
        std::println(std::cerr, "ERROR [Display {}]: Could not open decoder.", cam_index);
        return;
    }

    frame = av_frame_alloc();
    if (!frame) {
        std::println(std::cerr, "ERROR [Display {}]: Could not allocate frame.", cam_index);
        return;
    }

    AVPacket* pkt = nullptr;
    SDL_Event e;
    bool local_running = true;

    // 3. Main Loop
    while (local_running && g_running.load(std::memory_order_relaxed)) {
        // Handle window events
        while (SDL_PollEvent(&e) != 0) {
            if (e.type == SDL_QUIT) {
                std::println("Display {} received QUIT signal.", cam_index);
                local_running = false;
                g_running = false; // Signal all threads to stop
            }
        }

        // Pop packet from our dedicated queue
        if (!g_display_packet_q[cam_index].pop(pkt)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        // Send packet to decoder
        if (avcodec_send_packet(dec_ctx, pkt) < 0) {
            std::println(std::cerr, "WARNING [Display {}]: Error sending packet to decoder.", cam_index);
        }
        release_packet(pkt); // Release packet back to pool

        // Receive decoded frame
        while (avcodec_receive_frame(dec_ctx, frame) == 0) {
            // Render the YUV frame
            SDL_UpdateYUVTexture(
                texture,
                nullptr,
                frame->data[0], frame->linesize[0], // Y plane
                frame->data[1], frame->linesize[1], // U plane
                frame->data[2], frame->linesize[2]  // V plane
            );
            SDL_RenderClear(renderer);
            SDL_RenderCopy(renderer, texture, nullptr, nullptr);
            SDL_RenderPresent(renderer);
        }
    }

    // 4. Shutdown
    std::println("Display thread {} stopping...", cam_index);
    av_frame_free(&frame);
    avcodec_free_context(&dec_ctx);
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
}


//== Main Function =========================================================

int main() {
    std::println("--- OptiTrack 3-Camera Recorder ---");

#ifndef _WIN32
    RawTerm term_guard; // Set terminal to raw mode
#endif

    // 1. Init SDK
    std::println("Starting Camera Library...");
    CameraLibraryStartup();
    CameraManager::X().ScanForCameras();

    CameraList list;
    CameraManager::X().GetCameraList(list);
    if (list.Count() < NUM_CAMERAS) {
        std::println(std::cerr, "ERROR: Found {} cameras, but {} are required. Exiting.", list.Count(), NUM_CAMERAS);
        CameraLibraryShutdown();
        return -1;
    }

    // 2. Init Cameras and Sync Module
    std::println("Found {} cameras. Initializing first {}:", list.Count(), NUM_CAMERAS);
    g_sync_module = new cModuleSync();
    // FIX: Removed dummy pts_counter
    
    for (int i = 0; i < NUM_CAMERAS; ++i) {
        auto cam = CameraManager::X().GetCameraBySerial(list[i].Serial());
        if (!cam) {
            std::println(std::cerr, "ERROR: Could not get camera by serial {}.", list[i].Serial());
            return -1;
        }
        g_cameras.push_back(cam);
        g_camera_serials[i] = std::to_string(cam->Serial());

        std::println("- Cam {}: Serial {} ({})", i, g_camera_serials[i], cam->Name());
        
        // Create and attach our custom module to this camera
        // FIX: Pass only the camera index
        auto* module = new H264FanoutModule(i);
        cam->AttachModule(module);

        cam->SetVideoType(Core::VideoMode);
        
        // Add camera to the hardware sync group
        g_sync_module->AddCamera(cam);
    }

    std::println("Starting all cameras...");
    for (auto& cam : g_cameras) {
        cam->Start();
    }
    std::println("Hardware sync module and cameras started.");

    // 3. Init Packet Pool
    init_packet_pool();

    // 4. Spawn Threads
    std::println("Spawning 6 threads (3R, 3D)...");
    std::array<std::thread, NUM_CAMERAS> remux_threads;
    std::array<std::thread, NUM_CAMERAS> disp_threads;

    for (int i = 0; i < NUM_CAMERAS; ++i) {
        remux_threads[i] = std::thread(remuxer_thread, i);
        disp_threads[i] = std::thread(display_thread, i);
    }

    // 5. Start and Run
    std::println("--- All threads started. ---");
    std::println("--- Press 'q' to quit. ---");
    g_running = true;

    while (g_running.load()) {
        char ch = 0;
#ifdef _WIN32
        if (_kbhit()) {
            ch = _getch();
        }
#else
        read(STDIN_FILENO, &ch, 1);
#endif
        if (ch == 'q' || ch == 'Q') {
            std::println("'q' pressed. Initiating shutdown...");
            g_running = false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // 6. Shutdown
    std::println("Stopping all cameras...");
    for (auto& cam : g_cameras) {
        if (cam->IsCameraRunning()) {
            cam->Stop();
        }
        // FIX: Removed cam->ClearModules(). The Camera object owns
        // the module and will clean it up on destruction/shutdown.
    }

    std::println("Joining threads...");
    for (int i = 0; i < NUM_CAMERAS; ++i) {
        remux_threads[i].join();
        disp_threads[i].join();
    }
    std::println("All threads joined.");

    // 7. Cleanup
    delete g_sync_module;
    g_cameras.clear();
    CameraLibraryShutdown();
    SDL_Quit();
    
    // Clean up packet pool
    AVPacket* pkt = nullptr;
    while(g_packet_pool.pop(pkt)) {
        av_packet_free(&pkt);
    }

    std::println("Shutdown complete. Exiting.");
    return 0;
}
