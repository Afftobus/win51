# Win51 — a virtual 5.1 sound card that streams Dolby Digital (AC-3) over HDMI

[Русская версия](README.ru.md)

Win51 adds a 5.1 output device to Windows. Everything played to it — games, browsers, system sounds — is encoded to AC-3 in real time and sent over HDMI as a bitstream, exactly like a media player's "AC-3 passthrough". A TV that forwards the bitstream to its optical output, and an AV receiver behind it, then play real 5.1. This is what sound cards with Dolby Digital Live do in hardware; GPU HDMI audio cannot.

```
 apps ──► "Speakers (Win51 AC3 5.1)" ──► win51svc ──────────────────► HDMI ──► TV ──► optical ──► AV receiver
          virtual 6-channel endpoint     AC-3 640 kbit/s, IEC 61937     bitstream         S/PDIF       decodes 5.1
          (kernel driver)                (user-mode service)
```

**Why this is needed.** Over optical (S/PDIF) a receiver gets either stereo PCM or a compressed bitstream such as AC-3 or DTS. Many TVs also advertise only stereo PCM to the PC in their HDMI EDID. So the receiver gets surround sound only when a player passes an AC-3/DTS track through untouched, and games, which produce multichannel PCM, play in stereo. Win51 encodes that multichannel PCM into AC-3 on the fly.

## Status

It runs every day on one system: Windows 10 22H2, NVIDIA RTX 2060 HDMI → LG TV (2018) → optical → AV receiver. Verified on that system:

- channel mapping of all six channels, LFE included (built-in test signal);
- ordinary Windows audio plays as 5.1;
- the stream recovers after the TV is switched off and back on and after HDMI renegotiation;
- the service restarts automatically after a crash;
- the installer, and the controls the tray icon uses: output choice, releasing the HDMI output, a busy output.

Other GPUs, Windows 11 and other TVs have not been tested yet. Reports are welcome.

## Will it work on my PC?

Very likely, if all of the following hold:

| Requirement | Why |
|---|---|
| Windows 10 2004 (build 19041) or newer, or Windows 11, x64 | The INF targets 10.0.19041+. Windows 11 should work but is untested. |
| An output that accepts AC-3 passthrough in WASAPI exclusive mode | The service picks an HDMI/DisplayPort output by itself (most GPUs have one). An S/PDIF output also works when chosen in the tray menu. `win51svc list` shows what accepts AC-3. |
| A downstream chain that decodes AC-3 | If a player's AC-3 passthrough already gives you 5.1, this will too. On a TV, set the digital audio output to *Auto / Bitstream / Pass-through*, not *PCM*. |
| **Secure Boot disabled and Windows test mode enabled** | The driver is signed with a self-signed test certificate (see below). |

### About test signing

64-bit Windows loads kernel drivers only if they are signed by Microsoft. A driver for everyone needs Microsoft *attestation signing*: an EV code-signing certificate plus a Windows Hardware Dev Center account. That is not done (yet). Until then the driver is test-signed, which means:

- Secure Boot must be off in UEFI, and test mode is enabled with `bcdedit /set testsigning on`. If BitLocker is on, suspend it first, or Windows asks for the recovery key after the change.
- Windows shows a "Test Mode" watermark in the corner of the desktop.
- Games with kernel-level anti-cheat (Riot Vanguard, FACEIT, some Easy Anti-Cheat/BattlEye titles) refuse to start in test mode. Offline games and DRM such as Denuvo are not affected.
- Each build machine creates its own test certificate, and the PCs that install the driver trust it. Trusting a certificate means trusting anything signed with it, so prefer building yourself over installing someone else's test-signed binaries.

## How it works

### Driver (`driver/`)

A PortCls WaveRT miniport driver (KMDF 1.15) derived from Microsoft's [SimpleAudioSample](https://github.com/microsoft/Windows-driver-samples/tree/main/audio/simpleaudiosample). It is a root-enumerated device (`ROOT\Win51`) with a single render endpoint and no hardware behind it.

- **Formats.** 48 kHz, 6 channels: 32-bit float, 24-in-32-bit PCM or 16-bit PCM. Each format comes with channel mask `0x60F` (5.1, side surrounds) and `0x3F` (5.1, back surrounds). The default format and speaker setup are float 5.1 with side surrounds.
- **Clock.** The DMA position is computed from QPC time, and a 1 ms high-resolution timer signals buffer completion in event-driven mode. The driver never touches the audio data: Windows mixes into the WaveRT buffer and the service reads the mix through loopback.
- **Volume.** There are no volume or mute nodes, so Windows applies the volume slider in software and it ends up in the encoded stream.
- **Single stream.** The endpoint has one system stream pin, which the Windows audio engine uses.
- **Changes from the sample.** The microphone array, tone generator and WAV dumping are removed. The 5.1 formats are added. A 32-bit overflow in the position math is fixed: 6-channel float moves 1.15 MB/s, which overflowed after about 3.7 s without a position update.

### Service (`service/`)

`win51svc.exe` runs as the Windows service *Win51 AC3 encoder*. Pipeline:

1. **Capture.** WASAPI loopback capture of the Win51 endpoint mix: 48 kHz float, any 5.1 mask, mapped to the order FL FR FC LFE SL SR; back surrounds fold into side.
2. **Jitter buffer.** A FIFO held at 45 ms (`--target-ms`) at the moment each AC-3 frame is taken. The loopback stream and HDMI run on unrelated clocks:
   - drift is corrected by dropping or repeating a single sample frame when the smoothed level leaves ±1 ms;
   - when playback starts, silence is put in front of the audio, so the start lands exactly on the target without cutting anything;
   - if the output stalls, the buffer catches up at once instead of slowly trimming.
3. **AC-3 encoding.** libavcodec's `ac3` encoder: 640 kbit/s (`--bitrate`), 1536 samples per frame, about 0.1 ms per frame on an i7-8700K.
4. **IEC 61937 framing.** Each frame is wrapped into a 6144-byte data burst: preamble `Pa=0xF872 Pb=0x4E1F Pc=AC-3|bsmod Pd=length`, payload byte-swapped into 16-bit words, zero stuffing. This is the same signal a player sends with AC-3 passthrough.
5. **HDMI output.** WASAPI exclusive mode, event-driven, 10 ms period (`--period-ms`), format `KSDATAFORMAT_SUBTYPE_IEC61937_DOLBY_DIGITAL`, 2 ch / 16 bit / 48 kHz, with `WAVE_FORMAT_DOLBY_AC3_SPDIF` as a fallback. The thread runs under MMCSS "Pro Audio".

The output never stops. When nothing plays, the service keeps sending encoded silence, so the receiver stays locked to Dolby Digital and the first syllable of a sound is never lost to resynchronization.

When the TV is switched off or HDMI renegotiates, the service waits and reopens the output, typically within 1–2 s of it coming back. It never falls back to S/PDIF on its own. If another program holds the output exclusively, the service retries every 2 s and takes the output as soon as it is free.

**Control.** The service serves a named pipe, `\\.\pipe\Win51`. The tray icon and `win51svc ctl` use it to read the status, choose the output and release the HDMI output; the protocol is described in [`shared/win51_control.h`](shared/win51_control.h). Interactive users and administrators may connect. The output choice is stored in `HKLM\SOFTWARE\Win51` (`Output`, a name fragment; empty means automatic). Releasing the HDMI output lasts until it is taken back or the service restarts.

On the reference system the driver's QPC clock and the HDMI clock showed no measurable drift: more than 17 minutes of playback needed no corrections. The service uses about 0.6 % of one CPU core.

### Latency

An estimate from the parameters; the end-to-end latency has not been measured.

| Stage | Time |
|---|---|
| Windows audio engine period (plus whatever the application buffers) | 10 ms |
| Loopback packet | up to 10 ms |
| Jitter buffer, including assembly of the 1536-sample AC-3 frame | 45 ms |
| HDMI buffer | 10 ms |
| Transfer of the AC-3 frame inside the IEC 61937 burst (640 kbit/s) | ~13 ms |
| AC-3 decoder overlap (256 samples) | 5.3 ms |
| **PC side, up to the receiver's decoder** | **≈ 85–95 ms** |

The TV's passthrough and the receiver's processing add to this. The AC-3 format itself costs about 50 ms: a 32 ms frame, its transfer and the decoder overlap. The rest is buffering that can be reduced; see *Ideas*.

Windows is not told about this delay, so video players do not compensate for it. If lips run ahead of the sound, shift the audio by about −90 ms in the player.

## Installation

### With the installer

Download `Win51-Setup-<version>.exe` from [Releases](../../releases) and run it. It is not code-signed, so Windows SmartScreen may warn; choose *More info → Run anyway*.

- **Secure Boot.** If Secure Boot is on, setup says so and stops. Turn it off in the UEFI (BIOS) settings and run setup again.
- **BitLocker.** If BitLocker protects the system drive and test mode is not on yet, setup offers to suspend BitLocker for one restart. Without that, Windows asks for the recovery key after the boot settings change.
- **What it installs.** Setup turns on test mode (unless it is already on), trusts the Win51 test certificate, and installs the driver, the service and the tray icon into `C:\Program Files\Win51`.
- **Restart.** If test mode had to be turned on, restart Windows when setup asks.

Silent install: `Win51-Setup-<version>.exe /VERYSILENT /SUPPRESSMSGBOXES /NORESTART` (BitLocker is then suspended automatically when needed).

### Without the installer

`Win51-<version>-x64.zip` holds the same files. Unpack it and, if it was downloaded, unblock it: `Get-ChildItem -Recurse | Unblock-File`. Then run this in an elevated PowerShell in that folder:

```powershell
powershell -ExecutionPolicy Bypass -File .\setup.ps1                    # add -SuspendBitLocker if setup asks for it
```

`setup.ps1` does exactly what the installer does; the installer itself runs it. It copies the files to `C:\Program Files\Win51`. Its exit code is 3010 when Windows has to be restarted. Its log is `C:\ProgramData\Win51\setup.log`.

### Windows settings

In *Control Panel → Sound* (also *Sound settings…* in the tray menu):

- make **Speakers (Win51 AC3 5.1)** the default device;
- in its *Properties → Advanced*, untick **Allow applications to take exclusive control of this device**. An application that opens Win51 in exclusive mode bypasses the Windows mixer and would be silent.

In media players, decode audio to PCM and output to Win51 or the system default. The HDMI output is held by the service. To bitstream a DTS or TrueHD track from a player, release it in the tray menu first.

### Updating and uninstalling

To update, run the newer installer (or `setup.ps1`) over the old version. A driver of the same version is not reinstalled.

To uninstall, use *Settings → Apps → Win51*, or run `setup.ps1 -Uninstall`. This removes the driver, the service, the tray icon and the certificate. Test mode is turned off again if setup turned it on; restart Windows afterwards, and you can turn Secure Boot back on.

## The tray icon

`win51tray.exe` starts at sign-in and shows the service's state:

| Icon | State |
|---|---|
| green speaker with waves | Streaming to the output; the tooltip also says whether sound is playing |
| amber speaker with dots | Waiting for the output (TV off, output busy), HDMI output released, or the Win51 device missing |
| grey speaker with a cross | The service is not running |

The menu (left or right click):

- **Output.** *Automatic (HDMI)* or any output that accepts AC-3, S/PDIF included.
- **Release HDMI output.** Frees the output for a player's own passthrough; untick it to take the output back.
- **Sound settings…** Opens the Windows Sound control panel.
- **Open log folder.** Opens `C:\ProgramData\Win51`.
- **Close.** Removes the icon until the next sign-in. Start it again from the Start menu (*Win51*).

## Building

You need a Windows 10/11 x64 machine (it can be the target PC) and administrator rights:

- **Enterprise WDK (EWDK)**: the ISO from Microsoft's [WDK download page](https://learn.microsoft.com/windows-hardware/drivers/download-the-wdk). It is a self-contained compiler, WDK and SDK, nothing to install. Tested with EWDK 28000 (Visual Studio 2026 Build Tools).
- **FFmpeg**: a 64-bit *lgpl-shared* build, for example `ffmpeg-n8.1-latest-win64-lgpl-shared-8.1.zip` from [BtbN/FFmpeg-Builds](https://github.com/BtbN/FFmpeg-Builds/releases), extracted. FFmpeg 5.1 or newer is required for the channel layout API; 8.1 is tested.
- **Inno Setup** 6.3 or newer, optional, for the installer. Tested with 7.1. It can live in `deps` without being installed: `innosetup-7.1.0-x64.exe /PORTABLE=1 /VERYSILENT /CURRENTUSER /DIR=<repo>\deps\innosetup`.

Put them into `deps\` in the repository (ignored by git), or pass them as `-Ewdk` / `-Ffmpeg` / `-InnoSetup`:

```
deps\EWDK_28000.iso
deps\ffmpeg-n8.1-latest-win64-lgpl-shared-8.1\{bin,include,lib}
deps\innosetup\ISCC.exe
```

Then build in an elevated PowerShell:

```powershell
powershell -ExecutionPolicy Bypass -File tools\build.ps1            # -Configuration Debug
```

The script mounts the ISO and builds the driver, the service and the tray icon with `/W4 /WX`. On the first run it creates the test certificate `CN=Win51 Test Signing` in `Cert:\LocalMachine\My`. It then signs `win51.sys`, generates and signs the catalog, and assembles:

```
out\win51\                      the install bundle
  setup.ps1  win51svc.exe  win51tray.exe  avcodec-*.dll  avutil-*.dll  swresample-*.dll
  driver\    win51.sys  win51.inf  win51.cat  win51-test.cer
  LICENSE.txt  LICENSE-MS-PL.txt  FFMPEG-LICENSE.txt
out\Win51-<version>-x64.zip      the bundle as an archive
out\Win51-Setup-<version>.exe    the installer
```

The version is set in one place, [`Directory.Build.props`](Directory.Build.props). Windows keeps a driver package it already has for the same version, so bump the version to deploy driver changes.

## The `win51svc` command line

The installed copy lives in `C:\Program Files\Win51`. `tone` and `run` need the HDMI output, so release it in the tray menu (or stop the service) first.

```
win51svc list                    render endpoints, which accept AC-3 ([AC3 passthrough]) and which is picked ([auto output])
win51svc tone                    channel test: noise on FL, C, FR, SR, SL in turn, then a 35-90 Hz sweep on LFE; no driver needed
win51svc run                     encode what plays on Win51 (what the service does), in a console
win51svc service                 the same, under the service control manager
win51svc ctl status|outputs|pause|resume|output [<name>]
                                 talk to the running service, as the tray icon does
win51svc driver-install <inf>    create or update the root-enumerated Win51 device (setup.ps1 uses it)
win51svc driver-remove           remove it

options for tone / run / service:
  --out <name>|none              output (name fragment); default: the tray's choice, else the HDMI/DisplayPort
                                 output that accepts AC-3; none = no output, encode --seconds of audio at once
  --in <name>                    endpoint to capture (default: Win51)
  --bitrate <kbit/s>             AC-3 bit rate, 640 by default (the AC-3 maximum)
  --period-ms <ms>               HDMI exclusive-mode period, 10 by default
  --target-ms <ms>               jitter buffer target, 45 by default
  --seconds <n>                  stop after n seconds
  --only fl|fr|fc|lfe|sl|sr      tone: one channel only
  --dump <file>                  also write the IEC 61937 stream to a file (s16le stereo)
  --log <file>                   also write the log to a file
```

## Diagnostics

The service writes its log to `C:\ProgramData\Win51\win51svc.log` (*Open log folder* in the tray menu). `setup.log` next to it records installs. At 10 MB the service log moves to `win51svc.log.old` and starts over, so at most about 20 MB are kept, roughly 1–2 weeks of history. Every 10 s it adds a line like this:

```
AC3 frames 316 (failed 0), encode avg 0.12 / max 0.20 ms; capture 486720 frames (0 silent), buffer 1924..2308 frames,
underruns 0, drop/repeat 0/0, resyncs 0, overflow 0, discontinuities 0
```

| Field | Normal | Meaning |
|---|---|---|
| `AC3 frames` | 312–318 | Frames sent in 10 s (31.25/s). Fewer means the HDMI output was down. |
| `failed` | 0 | Encoder or framing errors. |
| `capture` | ~480 000 while sound plays, 0 in silence | Frames received from the Win51 mix. Windows sends nothing while nobody plays. |
| `buffer` | about 1900..2300 | Jitter buffer level, in frames at 48 kHz, when each AC-3 frame is taken. |
| `underruns` | 0, or 1 when playback stops | The buffer ran dry. Mid-playback it means a gap in the capture. |
| `drop/repeat` | 0/0, rare single counts | Clock drift corrections, one sample each. |
| `resyncs` | 0 | The output stalled and the buffer skipped ahead. |
| `overflow` | 0 | More than 1 s queued up. |
| `discontinuities` | 0, or 1 at a start | Glitch flags from the Windows loopback. |

To check the encoder and the channel order offline, dump a stream and decode it with FFmpeg:

```powershell
win51svc tone --out none --seconds 12 --dump tone.spdif
python tools\analyze_dump.py tone.spdif    # needs Python 3 and ffmpeg/ffprobe on PATH; prints per-channel levels
```

## Limitations

- Test mode only (see *About test signing*).
- AC-3 only, 5.1 at most. AC-3 has no 7.1, and there is no DTS or E-AC-3 encoder.
- The HDMI output is exclusively owned by the service. To bitstream DTS, TrueHD or E-AC-3 from a player, release the output in the tray menu. AC-3 and DTS tracks played through Win51 are decoded and re-encoded to AC-3 640 kbit/s.
- No upmix. Stereo sources stay on the front left and right speakers. Windows does not upmix, and the receiver sees a 5.1 stream, so its own upmixer does not engage.
- 48 kHz only; Windows resamples other rates.
- The latency is not reported to Windows, so there is no automatic lip-sync (see *Latency*).
- x64 only.

## Ideas

- Lower latency: a 3 ms HDMI period, a smaller jitter buffer, and feeding the service from the driver directly, clocked by HDMI, instead of WASAPI loopback.
- Report the pipeline latency through the stream position, so players keep lip-sync on their own.
- Attestation signing, so the driver installs without test mode.
- A DTS option (libavcodec has an experimental DTS encoder).

## Repository layout

```
driver/                 kernel driver (MS-PL, derived from SimpleAudioSample)
  speakerwavtable.h       formats and data ranges of the 5.1 endpoint
  speakertoptable.h       topology: no volume/mute nodes, 5.1 jack
  minwavertstream.cpp     WaveRT stream: buffer, QPC-driven position, notification timer
  win51.inx               INF template: endpoint properties, default 5.1 float format
service/                win51svc (MIT)
  src/pcm_source.*        loopback capture, jitter buffer and drift correction; test signal
  src/ac3_encoder.*       libavcodec AC-3 encoder
  src/iec61937.h          IEC 61937 burst framing, passthrough wave formats
  src/hdmi_output.*       WASAPI exclusive, event-driven output
  src/pipeline.*          the pipeline with its status, pause and output choice
  src/control_pipe.*      the control pipe server
  src/endpoints.*         endpoint enumeration, choice of the output
  src/device_setup.*      creating and removing the root-enumerated device (instead of devcon)
  src/settings.*          HKLM\SOFTWARE\Win51
  src/main.cpp            command line, Windows service
tray/                   win51tray: the notification area icon
shared/                 control pipe protocol and version macros, used by service and tray
installer/              Inno Setup script and the setup info pages
tools/
  build.ps1               build, sign, assemble the bundle, the archive and the installer
  setup.ps1               install, update, uninstall (copied into the bundle)
  make_icons.py           draws the tray icons
  analyze_dump.py         decodes a --dump file and prints per-channel levels
  sync.sh                 development from another machine: copies the tree to the PC over SSH
Directory.Build.props   the version
```

## Licenses

- Win51's own code: [MIT](LICENSE).
- `driver/` is derived from the [Windows Driver Samples](https://github.com/microsoft/Windows-driver-samples) and is under the [Microsoft Public License](driver/LICENSE-MS-PL.txt).
- The service dynamically links [FFmpeg](https://ffmpeg.org) (libavcodec, libavutil, libswresample) under the LGPL. The bundle ships its license text, and the source is available from ffmpeg.org and BtbN/FFmpeg-Builds.

Dolby, Dolby Digital and AC-3 are trademarks of Dolby Laboratories. This project is not affiliated with or endorsed by Dolby.
