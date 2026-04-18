# PotatoStream

**PotatoStream** is a game streaming client for **all Nintendo 3DS and 2DS models**, forked from [moonlight-N3DS](https://github.com/zoeyjodon/moonlight-N3DS) by zoeyjodon. Built with a focus on making streaming actually usable on **Old 3DS, Old 3DS XL and 2DS**, but works on New 3DS and New 2DS XL too.

Compatible with [Sunshine](https://github.com/LizardByte/Sunshine) (open-source, recommended) and NVIDIA GameStream.

> The original project targets the *New* 3DS and its hardware MVD decoder. PotatoStream keeps full New 3DS support while adding a dedicated Potato mode for older hardware: ARM11 compiler optimizations, smart frame skipping, auto-configured stream profile, and native Y2RU video pipeline.

---

## Why PotatoStream?

The Old 3DS / 2DS has **no** H.264 hardware decoder (MVD is New 3DS only). The original Moonlight runs unoptimized soft decode on these devices.

PotatoStream changes that:

- **Automatic hardware detection**: Potato mode activates automatically on Old 3DS/2DS | New 3DS keeps using the MVD hardware decoder as usual
- **Adapted stream config**: 400×240 @ 24fps, 3 Mbps —> matched to the actual capabilities of the ARM11 @ 268MHz
- **Y2RU pipeline**: uses the hardware YUV→RGB converter available on *all* 3DS models for frame conversion
- **Smart frame skip**: if the CPU falls behind its frame budget, packets are cleanly dropped instead of letting latency accumulate
- **Correct ARM11 compiler flags**: `-march=armv6k -mfpu=vfp -mfloat-abi=hard` — no NEON (not available on Old 3DS), no emulated float

---

## Requirements

- Any **3DS or 2DS** model with custom firmware (Luma3DS recommended)
- [Sunshine](https://github.com/LizardByte/Sunshine) installed on your PC
- 2.4GHz WiFi network (802.11n is enough)

To install CFW: [3ds.hacks.guide](https://3ds.hacks.guide/)

---

## Installation

1. Download `potatostream.cia` from the [Releases](https://github.com/PainDe0Mie/PotatoStream/releases) page
2. Copy the file to your SD card
3. Install it using [FBI](https://github.com/Steveice10/FBI)
4. Launch PotatoStream from the HOME menu

Or scan the QR code with FBI:

<img width="272" height="270" alt="image" src="https://github.com/user-attachments/assets/29751d3c-f134-4863-9cef-a7f77add8ff5" />

---

## First time setup

### Pairing

1. Open PotatoStream on your 3DS/2DS
2. Press `A` to set up a new host
3. Enter your PC's IP address
   - Windows: `Win + R` → `cmd` → `ipconfig` → look for "IPv4 Address"
4. Select `Pair` and note the PIN displayed on screen
5. In the Sunshine web UI (`https://localhost:47990`), enter the PIN
6. Pairing is complete — you can now start streaming

### Streaming

1. Select your PC from the main menu
2. Choose `Stream`, then pick which app to launch
3. View options appear on the bottom screen
4. Press HOME during a stream to access the in-stream menu

---

## Configuration

The config file is located on the SD card: `sd:/3ds/potatostream/potatostream.conf`

Recommended settings for Old 3DS/2DS (applied automatically by Potato mode):

```
width=400
height=240
fps=24
bitrate=3000
sops=false
```

New 3DS users can use higher settings — the default config from the original moonlight-N3DS applies.

---

## Building from source

Dependencies (expat, openssl, ffmpeg) are compiled via the provided scripts:

```bash
source /etc/profile.d/devkit-env.sh
bash ./3ds/build-expat.sh
bash ./3ds/build-openssl.sh
bash ./3ds/build-ffmpeg.sh
make
```

Or with Docker:

```bash
docker build --network=host -t potatostream .
docker run --rm -it -v .:/PotatoStream -w /PotatoStream potatostream:latest
make
```

---

## Based on

- [moonlight-N3DS](https://github.com/zoeyjodon/moonlight-N3DS) by zoeyjodon
- [moonlight-common-c](https://github.com/moonlight-stream/moonlight-common-c)
- [Moonlight Embedded](https://github.com/moonlight-stream/moonlight-embedded)

---

## License

GPL-3.0 — see [LICENSE](LICENSE)
