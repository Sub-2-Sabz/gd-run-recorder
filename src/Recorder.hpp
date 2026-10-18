#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Minimal H.264 -> MP4 recorder (Android MediaCodec/MediaMuxer via the NDK).
// Frames are RGBA, bottom-up (exactly what glReadPixels returns).
// On non-Android platforms every function is a harmless stub.
namespace rec {
    // srcW/srcH = size of the RGBA frames you will submit.
    // outW/outH = size of the video (multiples of 16 are safest).
    bool begin(std::string const& tmpPath, int srcW, int srcH, int outW, int outH, int fps, int bitrateKbps);

    // false if the encoder thread is behind (skip grabbing a frame) or not recording
    bool wantsFrame();

    // hands a frame to the encoder thread; timestamped on arrival
    void submit(std::vector<uint8_t>&& rgba);

    // keep=true  -> finalize the mp4 and move it to finalPath
    // keep=false -> throw everything away (used for deaths)
    bool finish(bool keep, std::string const& finalPath);

    bool active();
    std::string lastError();
}
