#include "ac3_encoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
}

#include <algorithm>

#include "log.h"

namespace w51 {
namespace {

const char* AvErr(int err) {
    static thread_local char buf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(err, buf, sizeof(buf));
    return buf;
}

}  // namespace

Ac3Encoder::~Ac3Encoder() { Close(); }

void Ac3Encoder::Close() {
    av_packet_free(&packet_);
    av_frame_free(&frame_);
    avcodec_free_context(&ctx_);
    pts_ = 0;
    bitRate_ = 0;
}

bool Ac3Encoder::Open(int sampleRate, uint64_t channelMask, int bitRate) {
    Close();

    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_AC3);
    if (!codec) {
        Log("libavcodec has no AC-3 encoder");
        return false;
    }
    ctx_ = avcodec_alloc_context3(codec);
    if (!ctx_) return false;

    ctx_->sample_rate = sampleRate;
    ctx_->sample_fmt = AV_SAMPLE_FMT_FLTP;
    ctx_->bit_rate = bitRate;
    if (av_channel_layout_from_mask(&ctx_->ch_layout, channelMask) < 0 || ctx_->ch_layout.nb_channels != kChannels) {
        Log("AC-3 encoder: unsupported channel mask 0x%llx", (unsigned long long)channelMask);
        Close();
        return false;
    }

    int err = avcodec_open2(ctx_, codec, nullptr);
    if (err < 0) {
        Log("avcodec_open2(%s) failed: %s", codec->name, AvErr(err));
        Close();
        return false;
    }
    if (ctx_->frame_size != kFrameSamples) {
        Log("AC-3 encoder: unexpected frame size %d", ctx_->frame_size);
        Close();
        return false;
    }

    frame_ = av_frame_alloc();
    packet_ = av_packet_alloc();
    if (!frame_ || !packet_) {
        Close();
        return false;
    }
    frame_->nb_samples = kFrameSamples;
    frame_->format = AV_SAMPLE_FMT_FLTP;
    frame_->sample_rate = sampleRate;
    if (av_channel_layout_copy(&frame_->ch_layout, &ctx_->ch_layout) < 0 || av_frame_get_buffer(frame_, 0) < 0) {
        Log("AC-3 encoder: frame allocation failed");
        Close();
        return false;
    }

    bitRate_ = (int)ctx_->bit_rate;
    Log("AC-3 encoder (%s): %d Hz, channel mask 0x%llx, %d kbit/s, encoder delay %d samples", codec->name,
        sampleRate, (unsigned long long)channelMask, bitRate_ / 1000, ctx_->initial_padding);
    return true;
}

bool Ac3Encoder::Encode(const float* pcm, std::vector<uint8_t>& out) {
    if (!ctx_) return false;
    int err = av_frame_make_writable(frame_);
    if (err < 0) {
        Log("av_frame_make_writable failed: %s", AvErr(err));
        return false;
    }
    for (int c = 0; c < kChannels; ++c) {
        float* dst = reinterpret_cast<float*>(frame_->data[c]);
        for (int i = 0; i < kFrameSamples; ++i) {
            // The Windows mixer hands over unclipped floats; the encoder expects [-1, 1].
            dst[i] = std::clamp(pcm[i * kChannels + c], -1.0f, 1.0f);
        }
    }
    frame_->pts = pts_;
    pts_ += kFrameSamples;

    err = avcodec_send_frame(ctx_, frame_);
    if (err < 0) {
        Log("avcodec_send_frame failed: %s", AvErr(err));
        return false;
    }
    err = avcodec_receive_packet(ctx_, packet_);
    if (err < 0) {
        Log("avcodec_receive_packet failed: %s", AvErr(err));
        return false;
    }
    out.assign(packet_->data, packet_->data + packet_->size);
    av_packet_unref(packet_);
    return true;
}

}  // namespace w51
