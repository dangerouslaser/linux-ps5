# Linux 7.3 for the PlayStation 5 / PS5 Pro, with Dolby Vision

This is Linux 7.3 for the PlayStation 5 and PS5 Pro. It adds:

- **PS5 platform support**, ported from
  [ps5-linux/ps5-linux-patches](https://github.com/ps5-linux/ps5-linux-patches).
- **Dolby Vision (TV-led "Standard" DV) output.** This is a port of CroqueMr's CB1
  AMD Dolby Vision kernel support from
  [libreelec-x86-DV](https://github.com/CroqueMr/libreelec-x86-DV) to Linux 7.3,
  adapted to the PS5's DisplayPort-to-HDMI converter.
- **Converter fixes:**
  - audio passthrough, configured for each audio stream
  - HDR 4:2:2 on every accepted CTA mode
  - 24/25/30/50 Hz modes
  - a fix for a firmware EDID/modeset loop

The branch `ps5-7.3` starts with an unmodified import of stable v7.3 and
puts the PS5 commits on top. Run `git log` to see what changed and where it
came from.

## Status

Tested on a PS5 Pro (GPU 1002:13da, DCN 2.0.1, FLAVA3 converter) with an LG B4:

- **Dolby Vision.** Profile 7 FEL (1917) plays in Dolby Vision at 4K 23.976 Hz
  with TrueHD Atmos passthrough. It uses CB1 Kodi in "Dolby Vision Standard"
  mode. The TV reports `dolbyHdr` / Dolby Vision Cinema.
- **HDR10 and SDR.** Work, including 23.976/24/25/30/50/60 Hz modes.
- **Audio.** LPCM, AC3 and TrueHD (HBR) passthrough work over HDMI. E-AC3 is
  untested.
- **Known issue: AC3/E-AC3 passthrough with Dolby Vision.** With Dolby Vision
  active, AC3/E-AC3 *bitstream* passthrough makes the LG drop Dolby Vision
  exactly 9 s after it starts (`md crc error`). TrueHD and PCM don't.
  Workaround: pick the TrueHD track, or let Kodi decode AC3 to PCM.
- **Not ported yet:** the NXP IW620 Wi-Fi driver (out-of-tree in ps5-linux).

## How Dolby Vision works on the PS5

The PS5's GPU only drives DisplayPort. A converter chip (FLAVA3, driven over
the ICC i2c channel) turns that into HDMI. CB1's DV mode tunnels 12-bit
YCbCr 4:2:2 and the DV metadata inside an 8-bit RGB signal. That only works
if every bit reaches the TV unchanged and the TV gets the Dolby
vendor-specific InfoFrame. On the PS5 that means:

- **The CB1 atomic-check and stream code is used unchanged.** The connector
  check accepts the PS5's DP-1 because it feeds an HDMI sink.
- **The Dolby VSIF and the AVI InfoFrame go into the converter's InfoFrame
  slots** (`sceHdmiSetDolbyVsif`). The GPU's own InfoFrame packets don't reach
  HDMI.
- **The colour path is bit-exact.** This was verified with the DP sink CRC
  against the framebuffer, and on the TV with its HDMI receiver pixel monitor.
- **A firmware EDID notification loop is broken.** Every TMDS drop made the
  driver swap sinks and force another modeset, which re-initialised the
  converter every few seconds and knocked the TV out of DV. The driver now
  keeps the existing sink when the EDID is unchanged.
- **The AVI content type defaults to cinema.** With "game", LG TVs switch to
  their ALLM game modes. Set `hdmi.content_type=-1` to restore game.

## Building

- **Debian/Ubuntu package:** use the kernel builder from
  [ps5-linux/ps5-linux-image](https://github.com/ps5-linux/ps5-linux-image).
  Mount `ps5-linux-patches/firmware` at `/firmware`, because the patched
  `amdgpu/ps5_vcn.bin` is built into the kernel.
- **LibreELEC:** use `DEVICE=PS5` in the companion LibreELEC tree. It builds this
  branch as `LINUX=ps5`.

Useful parameters:

| Parameter | Meaning |
| --- | --- |
| `hdmi.extra_vics=93,94,95,96,31,32,33,34` | enable the 4K/1080p 24/25/30/50 Hz modes |
| `hdmi.content_type=N` | AVI content type: 3 cinema (default), -1 or 4 game, 0 none |
| `amdgpu.ps5_fractional_modes=0` | hide the 1000/1001 (23.976/29.97/59.94 Hz) modes |

## Credits

This work builds directly on other people's work:

- **PS5 Linux platform support:** [ps5-linux](https://github.com/ps5-linux) —
  ps5-linux-patches and ps5-linux-image, by Andy Nguyen (TheFloW), rmux,
  Slip, masterzorag, sleirsgoevy, mia26MAjFm, c0w-ar, MikeM64, MassZero0 and
  the other contributors.
- **Dolby Vision kernel support (CB1):** CroqueMr,
  [libreelec-x86-DV](https://github.com/CroqueMr/libreelec-x86-DV) (patches
  9900-08/09, 9910, 9920, 9921). CB1 credits these, and so do we:
  - [LibreELEC Hades](https://github.com/hadesdevls/LibreELEC-Hades) (MIT)
  - the YBLOD transport work
  - intel-dv-linux
- **PS4 MN86471A bridge driver:**
  [fail0verflow](https://github.com/fail0verflow/ps4-linux). Its register names
  (UPDCTRL, CSCMOD, ...) identified the converter's update and colour
  registers.
- **Linux kernel:** the Linux kernel contributors (GPL-2.0).

## License

GPL-2.0, like the Linux kernel. See `COPYING`. Ported files keep their
original copyright and SPDX headers.
