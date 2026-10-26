// IEC 61937 framing of AC-3 for transport over a 2-channel 16-bit PCM link (S/PDIF, HDMI).
#pragma once

#include <windows.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>

#include <cstdint>
#include <cstring>

namespace w51::iec61937 {

constexpr uint32_t kAc3FrameSamples = 1536;                        // samples per channel in one AC-3 frame
constexpr uint32_t kBurstFrames = kAc3FrameSamples;                // IEC 61937-3: AC-3 repetition period
constexpr uint32_t kBytesPerFrame = 4;                             // 2 channels x 16 bit
constexpr uint32_t kBurstBytes = kBurstFrames * kBytesPerFrame;    // 6144
constexpr uint32_t kHeaderBytes = 8;                               // Pa, Pb, Pc, Pd

// KSDATAFORMAT_SUBTYPE_IEC61937_DOLBY_DIGITAL {00000092-0000-0010-8000-00AA00389B71}
constexpr GUID kSubtypeDolbyDigital = {0x00000092, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

// The format a player uses for "AC3 passthrough": stereo 16-bit PCM carrying IEC 61937 bursts.
inline WAVEFORMATEXTENSIBLE Ac3Format(DWORD sampleRate = 48000) {
    WAVEFORMATEXTENSIBLE wfx = {};
    wfx.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    wfx.Format.nChannels = 2;
    wfx.Format.nSamplesPerSec = sampleRate;
    wfx.Format.wBitsPerSample = 16;
    wfx.Format.nBlockAlign = kBytesPerFrame;
    wfx.Format.nAvgBytesPerSec = sampleRate * kBytesPerFrame;
    wfx.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    wfx.Samples.wValidBitsPerSample = 16;
    wfx.dwChannelMask = KSAUDIO_SPEAKER_STEREO;
    wfx.SubFormat = kSubtypeDolbyDigital;
    return wfx;
}

// Older drivers only accept the plain WAVEFORMATEX tag.
inline WAVEFORMATEX Ac3FormatLegacy(DWORD sampleRate = 48000) {
    WAVEFORMATEX wfx = {};
    wfx.wFormatTag = WAVE_FORMAT_DOLBY_AC3_SPDIF;
    wfx.nChannels = 2;
    wfx.nSamplesPerSec = sampleRate;
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = kBytesPerFrame;
    wfx.nAvgBytesPerSec = sampleRate * kBytesPerFrame;
    return wfx;
}

// Wraps one AC-3 frame into a data-burst of kBurstBytes bytes (preamble, payload, zero stuffing).
// Words are written as little-endian 16-bit PCM samples; the payload is byte-swapped into 16-bit
// words as IEC 61937 requires.
inline bool PackAc3(const uint8_t* frame, size_t size, uint8_t* burst) {
    if (size < 6 || size > kBurstBytes - kHeaderBytes) return false;
    memset(burst, 0, kBurstBytes);

    const uint16_t bsmod = frame[5] & 0x07;          // bitstream mode from the AC-3 BSI
    const size_t paddedSize = (size + 1) & ~size_t(1);
    const uint16_t header[4] = {
        0xF872,                                       // Pa: sync word 1
        0x4E1F,                                       // Pb: sync word 2
        uint16_t(0x0001 | (bsmod << 8)),              // Pc: data type 1 = AC-3
        uint16_t(paddedSize * 8),                     // Pd: payload length in bits
    };
    memcpy(burst, header, sizeof(header));

    uint8_t* out = burst + kHeaderBytes;
    for (size_t i = 0; i + 1 < size; i += 2) {
        out[i] = frame[i + 1];
        out[i + 1] = frame[i];
    }
    if (size & 1) out[size] = frame[size - 1];       // last byte goes to the high half of its word
    return true;
}

}  // namespace w51::iec61937
