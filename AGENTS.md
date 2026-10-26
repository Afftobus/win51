# AGENTS.md

Guidance for AI coding agents working on this repository. The human-facing documentation is [README.md](README.md) (English) and [README.ru.md](README.ru.md) (Russian). Read README.md first: it explains the design, the latency budget and the log format.

## What this is

Win51 is a virtual 5.1 audio output for Windows whose mix is encoded to AC-3 and bitstreamed to HDMI. It has two parts:

- `driver/` — a PortCls WaveRT kernel driver (C++, KMDF 1.15, derived from Microsoft's SimpleAudioSample, MS-PL). It exposes one 6-channel render endpoint and moves no data itself; position and notifications are timer/QPC driven.
- `service/` — `win51svc.exe` (C++20, user mode). It runs this pipeline: WASAPI loopback of the Win51 endpoint → jitter buffer with drift correction → libavcodec AC-3 → IEC 61937 bursts → WASAPI exclusive, event-driven HDMI output. It runs as the Windows service `win51svc`.

## Repository map

| Path | Contents |
|---|---|
| `driver/speakerwavtable.h` | Endpoint formats (48 kHz, 6 ch, float/PCM32/PCM16, masks 0x60F and 0x3F) and data ranges |
| `driver/speakertoptable.h` | Topology: deliberately no volume/mute nodes |
| `driver/minwavertstream.cpp` | WaveRT stream: buffer, `UpdatePosition` (64-bit math), 1 ms notification timer |
| `driver/minwavert.cpp` | Wave miniport, `DataRangeIntersection` |
| `driver/win51.inx` | INF template: default format blob (`PKEY_AudioEngine_OEMFormat`), speaker config |
| `service/src/pcm_source.*` | `LoopbackSource` (capture, FIFO, lead-in, drop/repeat, resync) and `ToneSource` |
| `service/src/ac3_encoder.*` | libavcodec wrapper, one 1536-sample frame in, one AC-3 frame out |
| `service/src/iec61937.h` | Burst packing and the passthrough `WAVEFORMAT(EXTENSIBLE)` definitions |
| `service/src/hdmi_output.*` | Exclusive event-driven renderer |
| `service/src/endpoints.*` | Endpoint enumeration; auto output = HDMI/DisplayPort only |
| `service/src/main.cpp` | Command line (`list`, `tone`, `run`, `service`), pipeline, service control handler |
| `tools/build.ps1` | Build, test-sign and assemble the install bundle `out\win51\` |
| `tools/*.ps1` | Install and uninstall scripts; copied into the bundle and resolved relative to `$PSScriptRoot` |
| `tools/analyze_dump.py` | Decodes a `--dump` file with ffmpeg and prints per-channel RMS |
| `tools/sync.sh` | Copies the tree to a Windows PC over SSH, keeping its `deps\` and `out\` |

## Invariants — keep these consistent across files

- **Channel order and mask.** The order is FL FR FC LFE SL SR, `KSAUDIO_SPEAKER_5POINT1_SURROUND` (0x60F). It appears in:
  - the driver formats (`speakerwavtable.h`) and the jack channel mapping (`speakertoptable.h`);
  - the INF (`PKEY_AudioEndpoint_PhysicalSpeakers` and the OEMFormat blob);
  - the service (`kMask51Side`, the channel map in `LoopbackSource::Open`);
  - the encoder: libavcodec's native order for that mask is the same, so no reordering happens.
- **48 kHz only**, in the driver ranges and in the service checks.
- **IEC 61937 for AC-3:** a burst of 1536 frames × 4 bytes = 6144 bytes; `Pa=0xF872`, `Pb=0x4E1F`, `Pc = 1 | bsmod << 8`, `Pd` = payload length in bits; payload byte-swapped into 16-bit words.
- **The HDMI output never stops.** Silence is encoded too, so the receiver stays locked. Do not "optimise" this away.
- **No volume/mute nodes in the driver.** Windows then applies volume in software, and it reaches the loopback.
- **Version** is kept in three places: `driver/win51.rc`, `<TimeStamp>` in `driver/win51.vcxproj` (INF DriverVer) and `service/win51svc.rc`. Without `<TimeStamp>`, stampinf stamps a time-based version, and such stale packages outrank real versions in the driver store.
- **Log format.** The periodic stats line is documented in the README and read by people. Change both together.

## Building and verifying

Building works only on Windows, with the Enterprise WDK. There is no unit test suite. Verification means:

1. `tools\build.ps1` succeeds. Both projects build with `/W4 /WX`, so a warning is an error.
2. Offline encoder check, no hardware involved: `win51svc tone --out none --seconds 12 --dump t.spdif`, then `python3 tools/analyze_dump.py t.spdif`. Each channel should light up in turn: FL, FC, FR, SR, SL, LFE.
3. On the target, the service log (`C:\ProgramData\Win51\win51svc.log`) shows the expected stats. See *Diagnostics* in the README for normal values.

Dependencies go to `deps\` (gitignored): the EWDK ISO and an FFmpeg lgpl-shared win64 build (FFmpeg ≥ 5.1). Output goes to `out\` (gitignored), with the install bundle in `out\win51\`.

## Working on a remote Windows PC

The maintainer develops on another machine and builds on the Windows PC over SSH (Win32-OpenSSH with PowerShell as the default shell):

- `tools/sync.sh` copies the tree. `WIN_HOST` and `WIN_DIR` override the SSH host and the target directory.
- Windows OpenSSH strips double quotes from the remote command line. For anything non-trivial, send a script as `powershell -NoProfile -EncodedCommand <base64 of UTF-16LE>`. Prefix it with `[Console]::OutputEncoding=[Text.Encoding]::UTF8`, since localized Windows prints non-ASCII text.
- Processes started over SSH die when the session ends. Long-running things belong in the service, not in an SSH session.
- Key-based SSH logons have no DPAPI access. That is why the signing certificate lives in `Cert:\LocalMachine\My`.
- `install-service.ps1` restarts the service (about 3 s of silence). `install-driver.ps1` restarts the device, and the service recovers by itself. Stop the service before running `win51svc tone` or `run`: they need the HDMI output exclusively.

## Rules

- **Never change boot or security configuration on the user's machine.** That covers `bcdedit`, Secure Boot, BitLocker, certificate trust stores (Root, TrustedPublisher) and driver signature policy. The owner does these by hand with `enable-testsigning.ps1` and `disable-testsigning.ps1`.
- A driver bug crashes the machine (bugcheck). Keep driver changes minimal, reason about IRQL (most stream code runs at DISPATCH_LEVEL under a spin lock) and paged/non-paged code, and say so when a change has not been run on real hardware.
- Keep the Microsoft copyright headers in `driver/` files; they are MS-PL derivatives.
- Style:
  - match the surrounding code;
  - comments in English and short;
  - C++ sources are UTF-8 and compiled with `/utf-8`;
  - PowerShell scripts must run on Windows PowerShell 5.1 and stay ASCII-only, because 5.1 reads BOM-less files in the ANSI code page.
- Documentation changes go into both `README.md` and `README.ru.md`.
