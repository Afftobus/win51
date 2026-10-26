/*++

Copyright (c) Microsoft Corporation All Rights Reserved

Module Name:

    speakerwavtable.h

Abstract:

    Declaration of wave miniport tables for the 5.1 render endpoint.
--*/

#ifndef _WIN51_SPEAKERWAVTABLE_H_
#define _WIN51_SPEAKERWAVTABLE_H_

// The endpoint is a fixed 5.1 device: 48 KHz, 6 channels, 16-bit / 24-in-32-bit PCM
// or 32-bit float. Each sample format is listed with both 5.1 channel masks
// (side and back surrounds) because Windows and applications use either one.

#define SPEAKER_DEVICE_MAX_CHANNELS                 6       // Max Channels.

#define SPEAKER_HOST_MAX_CHANNELS                   6       // Max Channels.
#define SPEAKER_HOST_MIN_BITS_PER_SAMPLE            16      // Min Bits Per Sample
#define SPEAKER_HOST_MAX_BITS_PER_SAMPLE            32      // Max Bits Per Sample
#define SPEAKER_HOST_MIN_SAMPLE_RATE                48000   // Min Sample Rate
#define SPEAKER_HOST_MAX_SAMPLE_RATE                48000   // Max Sample Rate

//
// Max # of pin instances.
//
#define SPEAKER_MAX_INPUT_SYSTEM_STREAMS            1

//=============================================================================

// StaticSubFormat is the STATIC_ form of the subformat GUID (e.g. STATIC_KSDATAFORMAT_SUBTYPE_PCM).
#define WIN51_WAVE_FORMAT(StaticSubFormat, BitsPerSample, ValidBits, ChannelMask) \
    {                                                                           \
        {                                                                       \
            sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE),                          \
            0,                                                                  \
            0,                                                                  \
            0,                                                                  \
            STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),                              \
            StaticSubFormat,                                                    \
            STATICGUIDOF(KSDATAFORMAT_SPECIFIER_WAVEFORMATEX)                   \
        },                                                                      \
        {                                                                       \
            {                                                                   \
                WAVE_FORMAT_EXTENSIBLE,                                         \
                SPEAKER_HOST_MAX_CHANNELS,                                      \
                48000,                                                          \
                48000 * SPEAKER_HOST_MAX_CHANNELS * ((BitsPerSample) / 8),      \
                SPEAKER_HOST_MAX_CHANNELS * ((BitsPerSample) / 8),              \
                (BitsPerSample),                                                \
                sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)             \
            },                                                                  \
            (ValidBits),                                                        \
            (ChannelMask),                                                      \
            StaticSubFormat                                                     \
        }                                                                       \
    }

static
KSDATAFORMAT_WAVEFORMATEXTENSIBLE SpeakerHostPinSupportedDeviceFormats[] =
{
    // 0: default format - 32-bit float, 5.1 with side surrounds
    WIN51_WAVE_FORMAT(STATIC_KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, 32, 32, KSAUDIO_SPEAKER_5POINT1_SURROUND),
    WIN51_WAVE_FORMAT(STATIC_KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, 32, 32, KSAUDIO_SPEAKER_5POINT1),
    WIN51_WAVE_FORMAT(STATIC_KSDATAFORMAT_SUBTYPE_PCM,        32, 24, KSAUDIO_SPEAKER_5POINT1_SURROUND),
    WIN51_WAVE_FORMAT(STATIC_KSDATAFORMAT_SUBTYPE_PCM,        32, 24, KSAUDIO_SPEAKER_5POINT1),
    WIN51_WAVE_FORMAT(STATIC_KSDATAFORMAT_SUBTYPE_PCM,        16, 16, KSAUDIO_SPEAKER_5POINT1_SURROUND),
    WIN51_WAVE_FORMAT(STATIC_KSDATAFORMAT_SUBTYPE_PCM,        16, 16, KSAUDIO_SPEAKER_5POINT1),
};

//
// Supported modes (only on streaming pins).
//
static
MODE_AND_DEFAULT_FORMAT SpeakerHostPinSupportedDeviceModes[] =
{
    {
        STATIC_AUDIO_SIGNALPROCESSINGMODE_DEFAULT,
        &SpeakerHostPinSupportedDeviceFormats[0].DataFormat  // 48KHz float 5.1
    }
};

//
// The entries here must follow the same order as the filter's pin
// descriptor array.
//
static
PIN_DEVICE_FORMATS_AND_MODES SpeakerPinDeviceFormatsAndModes[] =
{
    {
        SystemRenderPin,
        SpeakerHostPinSupportedDeviceFormats,
        SIZEOF_ARRAY(SpeakerHostPinSupportedDeviceFormats),
        SpeakerHostPinSupportedDeviceModes,
        SIZEOF_ARRAY(SpeakerHostPinSupportedDeviceModes)
    },
    {
        BridgePin,
        NULL,
        0,
        NULL,
        0
    }
};

//=============================================================================
static
KSDATARANGE_AUDIO SpeakerPinDataRangesStream[] =
{
    { // 0 - PCM
        {
            sizeof(KSDATARANGE_AUDIO),
            KSDATARANGE_ATTRIBUTES,         // An attributes list follows this data range
            0,
            0,
            STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
            STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM),
            STATICGUIDOF(KSDATAFORMAT_SPECIFIER_WAVEFORMATEX)
        },
        SPEAKER_HOST_MAX_CHANNELS,
        SPEAKER_HOST_MIN_BITS_PER_SAMPLE,
        SPEAKER_HOST_MAX_BITS_PER_SAMPLE,
        SPEAKER_HOST_MIN_SAMPLE_RATE,
        SPEAKER_HOST_MAX_SAMPLE_RATE
    },
    { // 1 - IEEE float
        {
            sizeof(KSDATARANGE_AUDIO),
            KSDATARANGE_ATTRIBUTES,         // An attributes list follows this data range
            0,
            0,
            STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
            STATICGUIDOF(KSDATAFORMAT_SUBTYPE_IEEE_FLOAT),
            STATICGUIDOF(KSDATAFORMAT_SPECIFIER_WAVEFORMATEX)
        },
        SPEAKER_HOST_MAX_CHANNELS,
        32,
        32,
        SPEAKER_HOST_MIN_SAMPLE_RATE,
        SPEAKER_HOST_MAX_SAMPLE_RATE
    }
};

static
PKSDATARANGE SpeakerPinDataRangePointersStream[] =
{
    PKSDATARANGE(&SpeakerPinDataRangesStream[0]),
    PKSDATARANGE(&PinDataRangeAttributeList),
    PKSDATARANGE(&SpeakerPinDataRangesStream[1]),
    PKSDATARANGE(&PinDataRangeAttributeList),
};

//=============================================================================
static
KSDATARANGE SpeakerPinDataRangesBridge[] =
{
    {
        sizeof(KSDATARANGE),
        0,
        0,
        0,
        STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
        STATICGUIDOF(KSDATAFORMAT_SUBTYPE_ANALOG),
        STATICGUIDOF(KSDATAFORMAT_SPECIFIER_NONE)
    }
};

static
PKSDATARANGE SpeakerPinDataRangePointersBridge[] =
{
    &SpeakerPinDataRangesBridge[0]
};

//=============================================================================
static
PCPIN_DESCRIPTOR SpeakerWaveMiniportPins[] =
{
    // Wave Out Streaming Pin (Renderer) KSPIN_WAVE_RENDER3_SINK_SYSTEM
    {
        SPEAKER_MAX_INPUT_SYSTEM_STREAMS,
        SPEAKER_MAX_INPUT_SYSTEM_STREAMS,
        0,
        NULL,        // AutomationTable
        {
            0,
            NULL,
            0,
            NULL,
            SIZEOF_ARRAY(SpeakerPinDataRangePointersStream),
            SpeakerPinDataRangePointersStream,
            KSPIN_DATAFLOW_IN,
            KSPIN_COMMUNICATION_SINK,
            &KSCATEGORY_AUDIO,
            NULL,
            0
        }
    },
    // Wave Out Bridge Pin (Renderer) KSPIN_WAVE_RENDER3_SOURCE
    {
        0,
        0,
        0,
        NULL,
        {
            0,
            NULL,
            0,
            NULL,
            SIZEOF_ARRAY(SpeakerPinDataRangePointersBridge),
            SpeakerPinDataRangePointersBridge,
            KSPIN_DATAFLOW_OUT,
            KSPIN_COMMUNICATION_NONE,
            &KSCATEGORY_AUDIO,
            NULL,
            0
        }
    },
};

//=============================================================================
//
//                   ----------------------------
//                   |                          |
//  Host Pin     0-->|                          |--> 1 KSPIN_WAVE_RENDER3_SOURCE
//                   |                          |
//                   ----------------------------
static
PCCONNECTION_DESCRIPTOR SpeakerWaveMiniportConnections[] =
{
    { PCFILTER_NODE,            KSPIN_WAVE_RENDER3_SINK_SYSTEM,     PCFILTER_NODE,   KSPIN_WAVE_RENDER3_SOURCE }
};

//=============================================================================
static
PCPROPERTY_ITEM PropertiesSpeakerWaveFilter[] =
{
    {
        &KSPROPSETID_Pin,
        KSPROPERTY_PIN_PROPOSEDATAFORMAT,
        KSPROPERTY_TYPE_SET | KSPROPERTY_TYPE_BASICSUPPORT,
        PropertyHandler_WaveFilter
    },
    {
        &KSPROPSETID_Pin,
        KSPROPERTY_PIN_PROPOSEDATAFORMAT2,
        KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_BASICSUPPORT,
        PropertyHandler_WaveFilter
    }
};

DEFINE_PCAUTOMATION_TABLE_PROP(AutomationSpeakerWaveFilter, PropertiesSpeakerWaveFilter);

//=============================================================================
static
PCFILTER_DESCRIPTOR SpeakerWaveMiniportFilterDescriptor =
{
    0,                                              // Version
    &AutomationSpeakerWaveFilter,                   // AutomationTable
    sizeof(PCPIN_DESCRIPTOR),                       // PinSize
    SIZEOF_ARRAY(SpeakerWaveMiniportPins),          // PinCount
    SpeakerWaveMiniportPins,                        // Pins
    sizeof(PCNODE_DESCRIPTOR),                      // NodeSize
    0,                                              // NodeCount
    NULL,                                           // Nodes
    SIZEOF_ARRAY(SpeakerWaveMiniportConnections),   // ConnectionCount
    SpeakerWaveMiniportConnections,                 // Connections
    0,                                              // CategoryCount
    NULL                                            // Categories  - use defaults (audio, render, capture)
};

#endif // _WIN51_SPEAKERWAVTABLE_H_
