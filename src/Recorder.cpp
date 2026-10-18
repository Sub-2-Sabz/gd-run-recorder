#include "Recorder.hpp"

#ifdef __ANDROID__

#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <media/NdkMediaMuxer.h>

#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <mutex>
#include <thread>

namespace {
    constexpr int kFlagConfig = 2;      // BUFFER_FLAG_CODEC_CONFIG
    constexpr int kFlagEos = 4;         // BUFFER_FLAG_END_OF_STREAM
    constexpr int kConfigureEncode = 1; // CONFIGURE_FLAG_ENCODE
    constexpr ssize_t kTryAgain = -1;
    constexpr ssize_t kFormatChanged = -2;
    constexpr ssize_t kBuffersChanged = -3;

    struct Job {
        std::vector<uint8_t> rgba;
        int64_t ptsUs = 0;
    };

    AMediaCodec* g_codec = nullptr;
    AMediaMuxer* g_muxer = nullptr;
    int g_fd = -1;
    ssize_t g_track = -1;
    bool g_muxStarted = false;
    std::atomic<bool> g_active{false};
    bool g_nv12 = true;
    int g_srcW = 0, g_srcH = 0, g_outW = 0, g_outH = 0;
    std::string g_tmp;
    std::string g_err;
    std::chrono::steady_clock::time_point g_t0;
    int64_t g_lastPts = -1;
    bool g_gotFrames = false;

    std::mutex g_mutex;
    std::condition_variable g_cv;
    std::deque<Job> g_queue;
    bool g_quit = false;
    std::thread g_thread;
    std::vector<int> g_xmap;

    inline uint8_t clamp8(int v) {
        return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
    }

    // RGBA (bottom-up, srcW x srcH) -> YUV420 (outW x outH), nearest-neighbour scaled
    void convert(uint8_t const* src, uint8_t* dst) {
        int const W = g_outW, H = g_outH;
        uint8_t* yp = dst;
        uint8_t* uBase = dst + (size_t)W * H;
        uint8_t* vBase = g_nv12 ? uBase + 1 : uBase + (size_t)(W / 2) * (H / 2);
        int const step = g_nv12 ? 2 : 1;

        g_xmap.resize(W);
        for (int x = 0; x < W; x++) {
            g_xmap[x] = (int)((int64_t)x * g_srcW / W) * 4;
        }

        for (int y = 0; y < H; y++) {
            int sy = (int)((int64_t)y * g_srcH / H);
            // glReadPixels is bottom-up, so flip vertically
            uint8_t const* row = src + (size_t)(g_srcH - 1 - sy) * g_srcW * 4;
            for (int x = 0; x < W; x++) {
                uint8_t const* p = row + g_xmap[x];
                int r = p[0], g = p[1], b = p[2];
                yp[(size_t)y * W + x] = clamp8(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
                if (((x | y) & 1) == 0) {
                    size_t ci = (size_t)(y >> 1) * (W >> 1) + (x >> 1);
                    uBase[ci * step] = clamp8(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
                    vBase[ci * step] = clamp8(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
                }
            }
        }
    }

    // pull finished packets out of the encoder and into the muxer
    void drain(bool eos) {
        if (!g_codec) return;
        AMediaCodecBufferInfo info;
        int idle = 0;
        for (;;) {
            ssize_t idx = AMediaCodec_dequeueOutputBuffer(g_codec, &info, eos ? 10000 : 0);
            if (idx == kTryAgain) {
                if (!eos || ++idle > 300) return;
                continue;
            }
            if (idx == kFormatChanged) {
                if (!g_muxStarted) {
                    AMediaFormat* fmt = AMediaCodec_getOutputFormat(g_codec);
                    g_track = AMediaMuxer_addTrack(g_muxer, fmt);
                    AMediaFormat_delete(fmt);
                    if (g_track >= 0 && AMediaMuxer_start(g_muxer) == AMEDIA_OK) {
                        g_muxStarted = true;
                    } else {
                        g_err = "muxer start failed";
                    }
                }
                continue;
            }
            if (idx == kBuffersChanged) continue;
            if (idx < 0) return;

            size_t sz = 0;
            uint8_t* out = AMediaCodec_getOutputBuffer(g_codec, (size_t)idx, &sz);
            if ((info.flags & kFlagConfig) != 0) info.size = 0;
            if (out && info.size > 0 && g_muxStarted) {
                AMediaMuxer_writeSampleData(g_muxer, (size_t)g_track, out, &info);
                g_gotFrames = true;
            }
            AMediaCodec_releaseOutputBuffer(g_codec, (size_t)idx, false);
            if ((info.flags & kFlagEos) != 0) return;
        }
    }

    void feed(Job& job) {
        ssize_t idx = -1;
        for (int i = 0; i < 20 && idx < 0; i++) {
            idx = AMediaCodec_dequeueInputBuffer(g_codec, 5000);
            if (idx < 0) drain(false);
        }
        if (idx < 0) return; // encoder is stuck, drop this frame

        size_t cap = 0;
        uint8_t* in = AMediaCodec_getInputBuffer(g_codec, (size_t)idx, &cap);
        size_t const need = (size_t)g_outW * g_outH * 3 / 2;
        if (!in || cap < need) {
            g_err = "encoder input buffer too small";
            AMediaCodec_queueInputBuffer(g_codec, (size_t)idx, 0, 0, job.ptsUs, 0);
            return;
        }
        if (job.ptsUs <= g_lastPts) job.ptsUs = g_lastPts + 1000;
        g_lastPts = job.ptsUs;

        convert(job.rgba.data(), in);
        AMediaCodec_queueInputBuffer(g_codec, (size_t)idx, 0, need, (uint64_t)job.ptsUs, 0);
    }

    void workerMain() {
        for (;;) {
            Job job;
            {
                std::unique_lock<std::mutex> lk(g_mutex);
                g_cv.wait(lk, [] { return g_quit || !g_queue.empty(); });
                if (g_queue.empty()) return; // quitting and nothing left
                job = std::move(g_queue.front());
                g_queue.pop_front();
            }
            feed(job);
            drain(false);
        }
    }

    void teardown() {
        if (g_codec) {
            AMediaCodec_stop(g_codec);
            AMediaCodec_delete(g_codec);
            g_codec = nullptr;
        }
        if (g_muxer) {
            if (g_muxStarted) AMediaMuxer_stop(g_muxer);
            AMediaMuxer_delete(g_muxer);
            g_muxer = nullptr;
        }
        if (g_fd >= 0) {
            close(g_fd);
            g_fd = -1;
        }
        g_muxStarted = false;
        g_track = -1;
    }
}

namespace rec {
    bool begin(std::string const& tmpPath, int srcW, int srcH, int outW, int outH, int fps, int bitrateKbps) {
        if (g_active) return false;
        g_err.clear();
        g_srcW = srcW;
        g_srcH = srcH;
        g_outW = outW & ~1;
        g_outH = outH & ~1;
        g_tmp = tmpPath;
        g_lastPts = -1;
        g_gotFrames = false;
        g_muxStarted = false;
        g_track = -1;

        g_fd = open(tmpPath.c_str(), O_CREAT | O_RDWR | O_TRUNC, 0644);
        if (g_fd < 0) {
            g_err = "could not open output file";
            return false;
        }
        g_muxer = AMediaMuxer_new(g_fd, AMEDIAMUXER_OUTPUT_FORMAT_MPEG_4);
        if (!g_muxer) {
            g_err = "could not create muxer";
            teardown();
            return false;
        }

        // Different phones accept different raw layouts, so try a few.
        // 21 = NV12 (semi-planar), 19 = I420 (planar), 0x7F420888 = "flexible"
        static int const formats[] = {21, 19, 0x7F420888};
        bool ok = false;
        for (int fmt : formats) {
            g_codec = AMediaCodec_createEncoderByType("video/avc");
            if (!g_codec) {
                g_err = "no H.264 encoder on this device";
                break;
            }
            AMediaFormat* f = AMediaFormat_new();
            AMediaFormat_setString(f, "mime", "video/avc");
            AMediaFormat_setInt32(f, "width", g_outW);
            AMediaFormat_setInt32(f, "height", g_outH);
            AMediaFormat_setInt32(f, "bitrate", bitrateKbps * 1000);
            AMediaFormat_setInt32(f, "frame-rate", fps);
            AMediaFormat_setInt32(f, "i-frame-interval", 1);
            AMediaFormat_setInt32(f, "color-format", fmt);
            media_status_t s = AMediaCodec_configure(g_codec, f, nullptr, nullptr, kConfigureEncode);
            AMediaFormat_delete(f);
            if (s == AMEDIA_OK && AMediaCodec_start(g_codec) == AMEDIA_OK) {
                g_nv12 = (fmt != 19);
                ok = true;
                break;
            }
            AMediaCodec_delete(g_codec);
            g_codec = nullptr;
        }
        if (!ok) {
            if (g_err.empty()) g_err = "encoder rejected every color format";
            teardown();
            std::error_code ec;
            std::filesystem::remove(g_tmp, ec);
            return false;
        }

        {
            std::lock_guard<std::mutex> lk(g_mutex);
            g_queue.clear();
            g_quit = false;
        }
        g_t0 = std::chrono::steady_clock::now();
        g_active = true;
        g_thread = std::thread(workerMain);
        return true;
    }

    bool wantsFrame() {
        if (!g_active) return false;
        std::lock_guard<std::mutex> lk(g_mutex);
        return g_queue.size() < 2;
    }

    void submit(std::vector<uint8_t>&& rgba) {
        if (!g_active) return;
        Job job;
        job.rgba = std::move(rgba);
        job.ptsUs = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - g_t0).count();
        {
            std::lock_guard<std::mutex> lk(g_mutex);
            g_queue.push_back(std::move(job));
        }
        g_cv.notify_one();
    }

    bool finish(bool keep, std::string const& finalPath) {
        if (!g_active) return false;
        g_active = false;
        {
            std::lock_guard<std::mutex> lk(g_mutex);
            g_quit = true;
            if (!keep) g_queue.clear();
        }
        g_cv.notify_all();
        if (g_thread.joinable()) g_thread.join();

        bool ok = false;
        if (keep && g_codec && g_lastPts >= 0) {
            ssize_t idx = -1;
            for (int i = 0; i < 100 && idx < 0; i++) {
                idx = AMediaCodec_dequeueInputBuffer(g_codec, 10000);
                if (idx < 0) drain(false);
            }
            if (idx >= 0) {
                AMediaCodec_queueInputBuffer(g_codec, (size_t)idx, 0, 0, (uint64_t)(g_lastPts + 1000), kFlagEos);
            }
            drain(true);
            ok = g_muxStarted && g_gotFrames;
            if (!ok && g_err.empty()) g_err = "encoder produced no frames";
        }
        teardown();

        std::error_code ec;
        if (keep && ok) {
            std::filesystem::rename(g_tmp, finalPath, ec);
            if (ec) {
                g_err = ec.message();
                ok = false;
            }
        } else {
            std::filesystem::remove(g_tmp, ec);
        }
        return ok;
    }

    bool active() { return g_active; }
    std::string lastError() { return g_err; }
}

#else // ---- non-Android: stubs so the mod still compiles everywhere ----

namespace rec {
    bool begin(std::string const&, int, int, int, int, int, int) { return false; }
    bool wantsFrame() { return false; }
    void submit(std::vector<uint8_t>&&) {}
    bool finish(bool, std::string const&) { return false; }
    bool active() { return false; }
    std::string lastError() { return "recording is only implemented for Android"; }
}

#endif
