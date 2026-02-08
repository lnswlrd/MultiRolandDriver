# MultiRolandDriver

> MultiRolandDriver is an independent project and is not affiliated with, sponsored by, or endorsed by Roland Corporation.

Open-source CoreMIDI driver plugin for macOS that replaces Roland's broken legacy drivers. Supports 24 Roland USB synthesizers as a universal binary (arm64 + x86_64).

## About

This driver provides modern macOS (Apple Silicon) support for a wide range of legacy synthesizers. It was developed to ensure continued interoperability and preservation of hardware instruments that are no longer supported by their original manufacturer.

**Technical Implementation:** This driver contains no code, binaries, or firmware from Roland Corporation. It is a 100% original implementation written from scratch, based entirely on public USB-MIDI specifications and observation of hardware communication protocols. The plugin installs per-user with no kernel extension and no elevated privileges required. A single universal binary covers all 24 supported devices.

**Purpose:** This is a non-commercial project dedicated to the synthesizer community, ensuring that classic instruments from 2002--2014 remain functional in modern studio environments.

## Why?

Roland's official macOS drivers consist of a kernel extension (kext) and a CoreMIDI plugin that work together. The kext component is deprecated and no longer loads on modern macOS, which breaks the entire driver. This plugin replaces both components with a single CoreMIDI plugin using IOKit USB access -- no kernel extension or DriverKit extension required.

## Supported Devices

All devices use USB Vendor ID `0x0582` (Roland).

| Device | USB PID | Ports |
|--------|---------|-------|
| V-Synth | `0x001F` | 1 |
| XV-5050 | `0x002B` | 1 |
| XV-2020 | `0x002D` | 1 |
| Fantom-S | `0x0032` | 1 |
| V-Synth XT | `0x0033` | 1 |
| Fantom-X | `0x006B` | 1 |
| Juno-D | `0x0074` | 1 |
| SH-201 | `0x009D` | 1 |
| Juno-G | `0x00A5` | 1 |
| V-Synth GT | `0x00AD` | 1 |
| AX-Synth | `0x00B5` | 1 |
| SonicCell | `0x00C4` | 1 |
| Fantom-G | `0x00D2` | 1 |
| Juno-Stage | `0x00EB` | 1 |
| Lucina AX-09 | `0x0103` | 1 |
| Juno-Di | `0x010F` | 1 |
| GAIA SH-01 | `0x0113` | 1 |
| Juno-Gi | `0x0123` | 1 |
| Jupiter-80 | `0x013A` | 1 |
| Jupiter-50 | `0x0150` | 1 |
| INTEGRA-7 | `0x015B` | 1 |
| FA-06/08 | `0x0174` | 2 |
| VR-09 | `0x01A1` | 1 |
| JD-Xi | `0x01D1` | 1 |

The FA-06/08 exposes two ports: MIDI and DAW Control.

## Building

```bash
make clean && make
```

Builds a universal binary (arm64 + x86_64) with ad-hoc code signing.

## Installing

```bash
make install
```

Installs the plugin to `~/Library/Audio/MIDI Drivers/` and restarts the MIDI server.

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
