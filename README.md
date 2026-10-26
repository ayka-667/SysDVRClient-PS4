# SysDVR for PS4

**Play your Nintendo Switch on your TV through a jailbroken PS4, no dock needed.**

SysDVR-PS4 is a PS4 homebrew port of the [SysDVR](https://github.com/exelix11/SysDVR) client. The Switch runs the SysDVR sysmodule and streams its screen and sound over Wi-Fi. The PS4 receives the stream, decodes it and shows it on the TV it's plugged into.

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

- 📡 **Automatic discovery:** finds the Switch on your network (SysDVR's UDP broadcast), no IP needed
- 🎮 **Manual IP entry** with the controller if your network blocks broadcasts
- 🎞️ **720p ~30 fps H.264**, hardware-agnostic software decoding (FFmpeg, multithreaded, SSE/AVX optimised for the PS4's Jaguar CPU)
- 🖥️ **Fullscreen 1080p** (scaled) or **1:1 720p**
- 🔊 **Audio**, 48 kHz stereo PCM with a low-latency jitter buffer
- 🔁 **Auto-reconnects** if the Wi-Fi drops or the Switch goes to sleep
- 💡 Option to **turn off the Switch's screen** while streaming (SysDVR 6.1+)
- 📊 On-screen stats (fps, bitrate)
- ⚙️ Settings saved to `/data/sysdvr-ps4/config.ini`

The UI is in French (the author's language). Pull requests for translations are welcome.

## Requirements

### Switch
- A Switch with custom firmware (Atmosphère) and the **[SysDVR](https://github.com/exelix11/SysDVR/releases) sysmodule** installed (5.x / 6.x, protocol `02` or `03`).
- SysDVR set to **TCP Bridge** mode (network). USB mode and "Simple network / RTSP" mode **won't work**.

### PS4
- A jailbroken PS4 (GoldHEN or similar) that can install **fake PKGs** (debug settings → Package Installer).
- Connected to the **same local network** as the Switch. Wired Ethernet on the PS4 is recommended.

## Installation

1. Download `IV0000-SDVR00001_00-SYSDVRPS4CLIENT0.pkg` from the [Releases](../../releases) page.
2. Copy it to a FAT32/exFAT USB drive (or send it with a remote package installer).
3. On the PS4: **Settings → Debug Settings → Game → Package Installer**, pick the pkg.
4. The **SysDVR PS4** app shows up on the home screen.

## Usage

1. **On the Switch:** open the *SysDVR Settings* homebrew and choose **TCP Bridge**. Reboot if it asks you to.
2. Make sure the Switch and the PS4 are on the **same Wi-Fi network / router**.
3. **On the PS4:** start *SysDVR PS4*. It searches for the Switch and connects on its own.
4. **Launch a game on the Switch.** The picture appears on the TV.

> SysDVR only captures **games**. The Switch home menu and some applets are not streamed, so a black or "waiting" screen is normal until a game is running. A few games block capture entirely.

### Controls

| Button | Action |
|---|---|
| **OPTIONS** | Open / close the settings menu |
| **△ Triangle** | Show / hide stats (fps, Mbit/s) |
| **□ Square** | Toggle fullscreen (1080p) / 1:1 (720p) |
| **✕ Cross** | Reconnect (when not connected) |
| **PS button** | Return to the PS4 home screen |

**Settings menu:** D-pad Up/Down to select, Left/Right or ✕ to change, ○ to close.

**IP editor:** Left/Right picks the octet, Up/Down changes it by 1, L1/R1 by 10, ✕ saves, □ switches back to automatic discovery, ○ cancels.

### Settings (`/data/sysdvr-ps4/config.ini`)

The file is created on first launch. Edit it over FTP or change the values from the in-app menu.

| Key | Default | Description |
|---|---|---|
| `ip` | `auto` | Switch IP address, or `auto` for discovery |
| `threads` | `3` | H.264 decoder threads (1–6). More threads are smoother but add about 1 frame of latency each. `1` gives the lowest latency. |
| `fast` | `0` | `1` skips the deblocking filter. Much faster decoding, slightly blockier picture. |
| `audio` | `1` | Enable sound |
| `fullscreen` | `1` | Scale to 1080p (`0` = native 720p, centred) |
| `screen_off` | `0` | Turn off the Switch screen while streaming (SysDVR 6.1+) |
| `audio_batch` | `2` | SysDVR audio batching (0–3) |
| `audio_buffer_ms` | `80` | Target audio buffer. Raise it if the sound crackles, lower it for less delay. |

A log is written to `/data/sysdvr-ps4/log.txt`.

## Troubleshooting

| Problem | Fix |
|---|---|
| Stuck on *"Recherche de la Switch…"* (searching) | Broadcasts may be blocked (guest Wi-Fi, AP isolation, mesh). Enter the Switch IP by hand (System Settings → Internet on the Switch). |
| *"Reponse inattendue… TCP Bridge ?"* | SysDVR is in USB or RTSP mode. Switch it to **TCP Bridge**. |
| *"Version de protocole SysDVR non supportee"* | Update SysDVR on the Switch to 5.5 or later. |
| Connected but black screen | Launch a game. The home menu isn't captured. |
| Choppy video | Use Ethernet on the PS4, keep the Switch close to the router (5 GHz), try `fast = 1`, or set `threads` to 4. |
| Too much latency | `threads = 1` or `2`, lower `audio_buffer_ms`. |
| Crackling sound | Raise `audio_buffer_ms` to 120–200. |

## How it works

SysDVR's **TCP Bridge** protocol ([spec](https://github.com/exelix11/SysDVR/blob/master/protocol.md)):

1. **Discovery:** every 2 s the Switch broadcasts `SysDVR|<version>|<proto>|<serial>` on UDP port 19999.
2. **Connection:** the client opens two sockets, **9911 (video)** and **9922 (audio)**. The Switch sends the 10-byte hello `SysDVR|03\0`. The client replies with a 16-byte `ProtoHandshakeRequest` (magic `0xAAAAAAAA`, version, stream flags, *InjectPPSSPS*). The Switch answers with a result code (4 bytes for proto 02, 72 bytes for proto 03).
3. **Streaming:** each packet has an 18-byte header (`0xCCCCCCCC` magic, size, timestamp, flags) followed by an H.264 access unit (Annex B) or 16-bit 48 kHz stereo PCM.

On the PS4 side:

| Module | Role |
|---|---|
| `src/sysdvr_proto.c` | Discovery, connect with timeout, handshake (proto 02/03), packet reading and resync |
| `src/video.c` | Network thread → FFmpeg `libavcodec` H.264 decoder (frame threads) → `libswscale` YUV→BGRA scaling (sliced, multithreaded) written **directly** into the framebuffer |
| `src/audio.c` | Network thread → ring buffer with drift/latency control → `sceAudioOut` (256-sample grains) |
| `src/gfx.c` | `sceVideoOut` double-buffered 1920×1080 framebuffer, vsync flip, text rendering with `stb_easy_font` |
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

`make test` builds the **real** `video.c`, `audio.c` and `sysdvr_proto.c` natively (PS4 APIs replaced by stubs in `tests/stubs`). It runs them against `tests/fake_switch.py`, a fake Switch that implements the SysDVR sysmodule side (UDP discovery, handshake 02/03, H.264 + PCM streaming):

```sh
sudo apt install ffmpeg libavcodec-dev libswscale-dev python3
make test
# ... Decouverte OK : 172.x.x.x serie=FAKE-SWITCH-0001
# ... Images decodees=194 rendues=178 ... RESULTAT : OK
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
