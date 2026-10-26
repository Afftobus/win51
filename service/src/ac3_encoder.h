// AC-3 encoder on top of libavcodec.
#pragma once

#include <cstdint>
#include <vector>

struct AVCodecContext;
struct AVFrame;
struct AVPacket;

namespace w51 {

class Ac3Encoder {
public:
    static constexpr int kFrameSamples = 1536;
    static constexpr int kChannels = 6;

    Ac3Encoder() = default;
    Ac3Encoder(const Ac3Encoder&) = delete;
    Ac3Encoder& operator=(const Ac3Encoder&) = delete;
    ~Ac3Encoder();

    // channelMask is a 5.1 WAVEFORMATEXTENSIBLE mask: side (0x60F) or back (0x3F) surrounds.
    bool Open(int sampleRate, uint64_t channelMask, int bitRate);
    void Close();

    // pcm: kFrameSamples frames of kChannels interleaved floats in WAVEFORMATEXTENSIBLE channel order
    // (identical to libavcodec's native order for the same mask). Produces exactly one AC-3 frame.
    bool Encode(const float* pcm, std::vector<uint8_t>& out);

    int BitRate() const { return bitRate_; }

private:
    AVCodecContext* ctx_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVPacket* packet_ = nullptr;
    int64_t pts_ = 0;
    int bitRate_ = 0;
};

}  // namespace w51
