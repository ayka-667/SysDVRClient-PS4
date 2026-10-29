# SysDVR for PS4

**Play your Nintendo Switch on your TV through a jailbroken PS4, no dock needed.**

SysDVR-PS4 is a PS4 homebrew port of the [SysDVR](https://github.com/exelix11/SysDVR) client. The Switch runs the SysDVR sysmodule and streams its screen and sound over Wi-Fi or a USB cable. The PS4 receives the stream, decodes it and shows it on the TV it's plugged into.

```
 ┌──────────────┐   Wi-Fi / LAN    ┌──────────────┐   HDMI   ┌──────┐
 │ Nintendo     │  TCP 9911 video  │ PS4          │ ───────► │  TV  │
 │ Switch       │ ───────────────► │ SysDVR-PS4   │          └──────┘
 │ (SysDVR)     │  TCP 9922 audio  │ (.pkg)       │
 └──────────────┘  UDP 19999 disc. └──────────────┘
```

> ⚠️ **Status: experimental.** The protocol, H.264 decoding, scaling and audio code was tested on a PC against a simulated Switch (see [Testing](#testing)). The app has **not yet been tested on real PS4 hardware**. Please open an issue with your `log.txt` if something goes wrong.

---

## Features

- 🔌 **Wi-Fi or USB cable:** stream over the network (TCP Bridge) or plug the Switch straight into a PS4 USB port
- 📡 **Automatic discovery:** finds the Switch on your network (SysDVR's UDP broadcast), no IP needed
- 🎮 **Manual IP entry** with the controller if your network blocks broadcasts
- 🎞️ **720p ~30 fps H.264**, software decoding (FFmpeg, multithreaded, SSE/AVX optimised for the PS4's Jaguar CPU)
- ⚡ **Low latency:** the socket is always drained on its own thread, and if decoding falls behind the app skips to the next keyframe instead of piling up delay
- 🖥️ **Fullscreen 1080p** (scaled) or **1:1 720p**, with a **Sharp** scaling mode (sharpened bicubic)
- 🔊 **Audio**, 48 kHz stereo PCM with a low-latency jitter buffer
- 🔁 **Auto-reconnects** if the Wi-Fi drops or the Switch goes to sleep
- 💡 Option to **turn off the Switch's screen** while streaming (SysDVR 6.1+)
- 📊 On-screen stats (fps, bitrate, decoder load, delay, skipped frames, audio buffer)
- ⚙️ Settings saved to `/data/sysdvr-ps4/config.ini`

## Requirements

### Switch
- A Switch with custom firmware (Atmosphère) and the **[SysDVR](https://github.com/exelix11/SysDVR/releases) sysmodule** installed (5.x / 6.x, protocol `02` or `03`).
- SysDVR set to **TCP Bridge** mode (network) or **USB** mode (cable). "Simple network / RTSP" mode **won't work**.

### PS4
- A jailbroken PS4 (GoldHEN or similar) that can install **fake PKGs** (debug settings → Package Installer).
- Network mode: connected to the **same local network** as the Switch. Wired Ethernet on the PS4 is recommended.
- USB mode: a USB-C to USB-A **data** cable (some charge-only cables have no data wires).

## Installation

1. Download `IV0000-SDVR00001_00-SYSDVRPS4CLIENT0.pkg` from the [Releases](../../releases) page.
2. Copy it to a FAT32/exFAT USB drive (or send it with a remote package installer).
3. On the PS4: **Settings → Debug Settings → Game → Package Installer**, pick the pkg.
4. The **SysDVR PS4** app shows up on the home screen.

## Usage

### Over the network (default)

1. **On the Switch:** open the *SysDVR Settings* homebrew and choose **TCP Bridge**. Reboot if it asks you to.
2. Make sure the Switch and the PS4 are on the **same Wi-Fi network / router**.
3. **On the PS4:** start *SysDVR PS4*. It searches for the Switch and connects on its own.
4. **Launch a game on the Switch.** The picture appears on the TV.

### Over a USB cable

1. **On the Switch:** open *SysDVR Settings* and choose **USB**. Reboot if it asks you to.
2. Plug the Switch into a PS4 USB port.
3. **On the PS4:** start *SysDVR PS4*, press **OPTIONS** and set **Connection** to **USB cable**. The setting is saved.
4. **Launch a game on the Switch.**

USB avoids Wi-Fi drops and interference. The PS4 port charges the Switch, but slowly.

> ⚠️ USB mode is **experimental**: it follows SysDVR's USB protocol (device `18D1:4EE0`, one bulk transfer per packet) through the PS4's `sceUsbd` library, but has not been confirmed on hardware yet. If it doesn't connect, please open an issue with your `log.txt`.

> SysDVR only captures **games**. The Switch home menu and some applets are not streamed, so a black or "waiting" screen is normal until a game is running. A few games block capture entirely.

### Controls

| Button | Action |
|---|---|
| **OPTIONS** | Open / close the settings menu |
| **△ Triangle** | Show / hide stats (see [Stats overlay](#stats-overlay)) |
| **□ Square** | Toggle fullscreen (1080p) / 1:1 (720p) |
| **✕ Cross** | Reconnect (when not connected) |
| **PS button** | Return to the PS4 home screen |

**Settings menu:** D-pad Up/Down to select, Left/Right or ✕ to change, ○ to close.

**IP editor:** Left/Right picks the octet, Up/Down changes it by 1, L1/R1 by 10, ✕ saves, □ switches back to automatic discovery, ○ cancels.

### Settings (`/data/sysdvr-ps4/config.ini`)

The file is created on first launch. Edit it over FTP or change the values from the in-app menu.

| Key | Default | Description |
|---|---|---|
| `connection` | `network` | `network` (SysDVR in TCP Bridge mode) or `usb` (SysDVR in USB mode, cable plugged into the PS4) |
| `ip` | `auto` | Switch IP address, or `auto` for discovery (network only) |
| `threads` | `2` | H.264 decoder threads (1–6). Each extra thread decodes faster but adds about 33 ms (one frame) of latency. `1` gives the lowest latency if your PS4 keeps up. |
| `fast` | `0` | `1` skips the deblocking filter. Much faster decoding, slightly blockier picture. It is also switched on automatically while the decoder is catching up. |
| `scaler` | `smooth` | Scaling to 1080p: `fast` (fast bilinear), `smooth` (bilinear) or `sharp` (sharpened bicubic, ~35% more CPU than `smooth`) |
| `max_delay_ms` | `150` | When decoded video falls this far behind, queued frames are dropped and decoding restarts at the next keyframe. |
| `audio` | `1` | Enable sound |
| `fullscreen` | `1` | Scale to 1080p (`0` = native 720p, centred) |
| `screen_off` | `0` | Turn off the Switch screen while streaming (SysDVR 6.1+) |
| `audio_batch` | `0` | SysDVR audio batching (0–3). Each step adds ~21 ms of audio per packet, so `0` is the lowest latency. |
| `audio_buffer_ms` | `60` | Target audio buffer. Raise it if the sound crackles, lower it for less delay. |

Config files from 1.0 are upgraded automatically: `threads`, `audio_batch` and `audio_buffer_ms` are reset to the new low-latency defaults once.

### Stats overlay

Press **△** while streaming:

| Field | Meaning |
|---|---|
| `fps` / `Mbit/s` | Frames decoded per second and stream bitrate |
| `decode` | Average time the decoder thread spends per frame. Close to 33 ms means the PS4 is at its limit: try `fast` or more `threads`. |
| `delay` | Extra delay of the shown frame compared to the fastest frame of this session (network + queue + decoding) |
| `queue` / `skipped` | Frames waiting to be decoded / frames dropped to catch up. `skipped` going up all the time means the decoder is too slow. |
| `audio` / `underruns` | Audio buffered right now / times the buffer ran dry (crackles). Raise `audio_buffer_ms` if `underruns` keeps going up. |

A log is written to `/data/sysdvr-ps4/log.txt`.

## Troubleshooting

| Problem | Fix |
|---|---|
| Stuck on *"Searching for the Switch on the network..."* | Broadcasts may be blocked (guest Wi-Fi, AP isolation, mesh). Enter the Switch IP by hand (System Settings → Internet on the Switch). |
| *"Unexpected reply: is SysDVR in TCP Bridge mode?"* | SysDVR is in USB or RTSP mode. Switch it to **TCP Bridge**. |
| *"Unsupported SysDVR protocol version"* | Update SysDVR on the Switch to 5.5 or later. |
| Connected but black screen | Launch a game. The home menu isn't captured. |
| Video freezes for a moment now and then | The decoder fell behind and skipped to the next keyframe (`skipped` goes up in the stats). Turn on *Fast decoding*, set `threads` to 3, or set *Scaling* to `fast`. |
| Choppy video, `skipped` stays at 0 | Network problem: use Ethernet on the PS4 and keep the Switch close to the router (5 GHz), or switch to USB. |
| USB: *"Plug the Switch into the PS4 with a USB cable"* stays on screen | SysDVR must be in **USB** mode (not TCP Bridge), and the cable must carry data. Try another PS4 port. |
| `skipped` goes up with *Scaling: sharp* | Your PS4 is at its limit: use `smooth`. |
| Too much latency | `threads = 1` (if `decode` stays well under 33 ms), lower `max_delay_ms` and `audio_buffer_ms`. |
| Crackling sound | Raise `audio_buffer_ms` to 100–200. |

## How it works

SysDVR's **TCP Bridge** protocol ([spec](https://github.com/exelix11/SysDVR/blob/master/protocol.md)):

1. **Discovery:** every 2 s the Switch broadcasts `SysDVR|<version>|<proto>|<serial>` on UDP port 19999.
2. **Connection:** the client opens two sockets, **9911 (video)** and **9922 (audio)**. The Switch sends the 10-byte hello `SysDVR|03\0`. The client replies with a 16-byte `ProtoHandshakeRequest` (magic `0xAAAAAAAA`, version, stream flags, *InjectPPSSPS*). The Switch answers with a result code (4 bytes for proto 02, 72 bytes for proto 03).
3. **Streaming:** each packet has an 18-byte header (`0xCCCCCCCC` magic, size, timestamp, flags) followed by an H.264 access unit (Annex B) or 16-bit 48 kHz stereo PCM.

On the PS4 side:

| Module | Role |
|---|---|
| `src/sysdvr_proto.c` | Discovery, connect with timeout, handshake (proto 02/03, shared by TCP and USB), packet reading and resync |
| `src/usb.c` | USB mode: finds the SysDVR device (`18D1:4EE0`, serial `SysDVR\|...`), handshake over bulk endpoints, then splits the multiplexed packets between video and audio |
| `src/video.c` | Network thread (always drains the socket) → packet queue with catch-up (drops to the next keyframe when it gets older than `max_delay_ms`) → decoder thread, FFmpeg `libavcodec` H.264 (frame threads, deblocking skipped while behind) → `libswscale` YUV→BGRA scaling (sliced, multithreaded) written **directly** into the framebuffer |
| `src/audio.c` | Network thread → ring buffer with latency control → `sceAudioOut` (256-sample grains) |
| `src/gfx.c` | `sceVideoOut` triple-buffered 1920×1080 framebuffer (a new frame never waits for the previous vsync), text rendering with `stb_easy_font` |
| `src/main.c` | UI, DualShock 4 input, settings menu, IP editor, auto-discovery thread |
| `src/util.c` | Logging, config file |

The Switch encodes **H.264 High profile, CABAC, 1280×720**. The PS4's hardware decoder (`sceVideodec2`) is barely documented for homebrew, so this port uses a trimmed-down FFmpeg (H.264 decoder only, ~2.6 MB) cross-compiled for the PS4 with its x86 SIMD (SSSE3/AVX) enabled.

## Building from source

The build has been tested on Windows 11 + WSL (Debian 13). Plain Linux works too.

### 1. Prerequisites

- [OpenOrbis PS4 Toolchain](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain) (copied to `~/ps4build/oo` in WSL, or point `OO_PS4_TOOLCHAIN` at it)
- In Linux/WSL: `clang lld llvm make nasm curl`
  ```sh
  sudo apt install clang lld llvm make nasm curl
  ```
- On Windows, to package: the OpenOrbis Windows tools plus any .NET runtime ≥ 6

### 2. Build FFmpeg for the PS4 (once)

```sh
./toolchain/build_ffmpeg.sh        # -> ~/ps4build/ffmpeg-ps4
```

The script downloads FFmpeg 7.1.1, configures it for `x86_64-pc-freebsd12` with the OpenOrbis sysroot (wrapper `toolchain/ps4-cc`), keeps only the H.264 decoder and swscale, and works around two PS4-specific issues (no `sysctl`/`sched_getaffinity`, and the FFmpeg 7.1 `aom_film_grain` link bug).

### 3. Build the app

**Windows (WSL + Windows PkgTool):**
```powershell
powershell -ExecutionPolicy Bypass -File build.ps1
```

**Linux:**
```sh
make            # needs libssl1.1 for OpenOrbis' Linux PkgTool
# or just the gp4 + eboot, then package with another PkgTool:
make pkg.gp4
```

Output: `IV0000-SDVR00001_00-SYSDVRPS4CLIENT0.pkg`

### Testing

`make test` builds the **real** `video.c`, `audio.c` and `sysdvr_proto.c` natively (PS4 APIs replaced by stubs in `tests/stubs`). It runs them against `tests/fake_switch.py`, a fake Switch that implements the SysDVR sysmodule side (UDP discovery, handshake 02/03, H.264 + PCM streaming). It runs two scenarios:

1. a normal decoder: nothing may be skipped, and the delay must stay low;
2. a decoder made artificially too slow for the stream (`SYSDVR_TEST_DECODE_DELAY_MS=45`): frames must be skipped, and the delay must stay bounded instead of growing.

```sh
sudo apt install ffmpeg libavcodec-dev libswscale-dev python3
make test
# ==> Real-time decoder
# ... frames decoded=255 rendered=239 skipped=0 ... delay avg=34 max=44 ms
# RESULT: OK
# ==> Decoder too slow for the stream (must skip frames, not build up latency)
# ... skipped=124 ... delay avg=123 max=200 ms
# RESULT: OK
```

You can also run `python3 tests/fake_switch.py video.h264` on a PC and point the PS4 app at that PC's IP to test on a real PS4 without a Switch.

## Project layout

```
├── src/                 PS4 app source (C)
├── tests/               fake Switch + native test harness + stubs
├── toolchain/           FFmpeg cross-build script and clang wrapper
├── tools/make_icon.py   generates sce_sys/icon0.png and pic1.png
├── sce_sys/             package metadata (icon, background, about)
├── sce_module/          libc.prx / libSceFios2.prx from OpenOrbis
├── Makefile             Linux/WSL build
└── build.ps1            one-click Windows build
```

## Credits

- [exelix11/SysDVR](https://github.com/exelix11/SysDVR), the Switch sysmodule and the protocol this client implements (GPLv2)
- [OpenOrbis PS4 Toolchain](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain)
- [FFmpeg](https://ffmpeg.org) (LGPL 2.1+), statically linked
- [stb_easy_font](https://github.com/nothings/stb) (public domain)

## License

GPL-2.0, like SysDVR. See [LICENSE](LICENSE).

This project is not affiliated with Nintendo or Sony. Only use it with hardware and games you own.
