# MultiRolandDriver

> MultiRolandDriver is an independent project and is not affiliated with, sponsored by, or endorsed by Roland Corporation.

Open-source CoreMIDI driver plugin for macOS that replaces Roland's broken legacy drivers. Supports 31 Roland/Edirol USB devices as a universal binary (arm64 + x86_64).

## About

This driver provides modern macOS (Apple Silicon) support for a wide range of legacy synthesizers. It was developed to ensure continued interoperability and preservation of hardware instruments that are no longer supported by their original manufacturer.

**Technical Implementation:** This driver contains no code, binaries, or firmware from Roland Corporation. It is a 100% original implementation written from scratch, based entirely on public USB-MIDI specifications and observation of hardware communication protocols. The plugin installs per-user with no kernel extension and no elevated privileges required. A single universal binary covers all 31 supported devices.

**Purpose:** This is a non-commercial project dedicated to the synthesizer community, ensuring that classic instruments from 2002--2014 remain functional in modern studio environments.

## Why?

Roland's official macOS drivers consist of a kernel extension (kext) and a CoreMIDI plugin that work together. The kext component is deprecated and no longer loads on modern macOS, which breaks the entire driver. This plugin replaces both components with a single CoreMIDI plugin using IOKit USB access -- no kernel extension or DriverKit extension required.

## Supported Devices

All devices use USB Vendor ID `0x0582` (Roland).

| Device | USB PID | Ports | Notes |
|--------|---------|-------|-------|
| SC-8850 | `0x0003` | 4 | Part A/B/C/D |
| SC-8820 | `0x0007` | 2 | Part A/B |
| SK-500 | `0x000B` | 2 | Part A/B |
| SC-D70 | `0x000C` | 2 | Composite (Audio+MIDI) |
| XV-5050 | `0x0012` | 1 | |
| SD-90 | `0x0016` | 2 | Composite (Audio+MIDI) |
| V-Synth | `0x001D` | 1 | |
| SD-20 | `0x0027` | 1 | |
| SD-80 | `0x0029` | 2 | MIDI 1/2 |
| XV-2020 | `0x002D` | 1 | |
| Edirol PCR | `0x0033` | 3 | MIDI + PCR 1/2, Advanced Driver mode |
| Fantom-X | `0x006D` | 1 | |
| G-70 | `0x0080` | 2 | MIDI + Control |
| V-Synth XT | `0x0084` | 1 | |
| Juno-G | `0x00A6` | 1 | |
| MC-808 | `0x00A9` | 1 | |
| SH-201 | `0x00AD` | 1 | |
| SonicCell | `0x00C2` | 1 | |
| V-Synth GT | `0x00C7` | 1 | |
| Fantom-G | `0x00DE` | 1 | |
| Juno-Di/Stage | `0x00F8` | 1 | Shared PID with XPS-10 |
| VS-700C | `0x00FC` | 1 | V-Studio 700 Console |
| GAIA SH-01 | `0x0111` | 1 | ⚠️ Support on hold — class-compliant, macOS claims it natively |
| Lucina AX-09 | `0x011C` | 1 | |
| Juno-Gi | `0x0123` | 1 | |
| Jupiter-80 | `0x013A` | 1 | |
| Jupiter-50 | `0x0154` | 1 | |
| INTEGRA-7 | `0x015B` | 1 | |
| FA-06/07/08 | `0x0174` | 2 | MIDI + DAW Control |
| JD-Xi | `0x01A1` | 1 | |
| **Interfaces** | | | |
| UM-ONE | `0x012A` | 1 | |
| QUAD-CAPTURE | `0x012F` | 1 | Composite (Audio+MIDI) |

Composite devices (SC-D70, SD-90, QUAD-CAPTURE) coexist safely with CoreAudio -- the driver filters USB interfaces by class and only claims the MIDI interface.

## Building and Installing

```bash
make clean && make
make install
```

Builds a universal binary (arm64 + x86_64) with ad-hoc code signing. Installs the plugin to `~/Library/Audio/MIDI Drivers/` and restarts MIDIServer.

## Pre-built plugin

Download `MultiRolandDriver.plugin` from [Releases](../../releases), then run:

```bash
./install.sh
```

The script removes the macOS quarantine flag, ad-hoc signs the plugin, installs it, and restarts MIDIServer.

## System Requirements

- macOS 12 Monterey or later
- No kernel extensions required
- No third-party dependencies (Apple frameworks only)

## Architecture

```
MultiRolandDriver.plugin (CFPlugIn, MIDIDriverInterface)
  |
  +-- MultiRolandDriver.cpp   CFPlugIn factory + MIDIDriverInterface vtable
  |                            FindDevices/Start/Stop/Send + USB hotplug
  |
  +-- RolandUSBDevice.cpp/h    Per-device USB I/O (IOKit user-space)
  |                            Open/Close/StartIO/StopIO/SendMIDI
  |                            Async bulk IN read + ReadCallback
  |
  +-- USBMIDIParser.cpp/h      USB-MIDI 1.0 packet handling
                               CIN-based parse (BulkIn) and build (BulkOut)
                               Cable number in high nibble = port routing
```

## License

This project is licensed under the GNU General Public License v3.0 - see the [LICENSE](LICENSE) file for details.
