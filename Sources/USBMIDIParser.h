#ifndef USBMIDIParser_h
#define USBMIDIParser_h

#include <stdint.h>
#include <stddef.h>

// USB-MIDI 1.0 Code Index Numbers (CIN)
enum USBMIDICin : uint8_t {
    kCIN_Misc              = 0x0,
    kCIN_CableEvent        = 0x1,
    kCIN_SystemCommon2Byte = 0x2,
    kCIN_SystemCommon3Byte = 0x3,
    kCIN_SysExStart        = 0x4,
    kCIN_SysExEnd1Byte     = 0x5,
    kCIN_SysExEnd2Byte     = 0x6,
    kCIN_SysExEnd3Byte     = 0x7,
    kCIN_NoteOff           = 0x8,
    kCIN_NoteOn            = 0x9,
    kCIN_PolyAftertouch    = 0xA,
    kCIN_ControlChange     = 0xB,
    kCIN_ProgramChange     = 0xC,
    kCIN_ChannelPressure   = 0xD,
    kCIN_PitchBend         = 0xE,
    kCIN_SingleByte        = 0xF,
};

static constexpr uint32_t kUSBMIDIMaxPacketSize = 64;
static constexpr uint8_t  kRolandVendorID_Hi    = 0x05;
static constexpr uint8_t  kRolandVendorID_Lo    = 0x82;
static constexpr uint16_t kRolandVendorID       = 0x0582;

/// Returns the number of MIDI data bytes for a given CIN value.
uint8_t USBMIDICinToMIDIByteCount(uint8_t cin);

/// Returns the appropriate CIN for a given MIDI status byte.
uint8_t MIDIStatusToCin(uint8_t statusByte);

/// Callback for parsed MIDI messages from USB bulk IN data.
typedef void (*USBMIDIParseCallback)(uint8_t cable,
                                     const uint8_t *midiBytes,
                                     uint8_t byteCount,
                                     void *context);

/// Parse USB-MIDI bulk IN data into individual MIDI messages.
void USBMIDIParseBulkIn(const uint8_t *data,
                        uint32_t length,
                        USBMIDIParseCallback callback,
                        void *context);

/// Build USB-MIDI event packets from a raw MIDI byte stream.
/// Returns number of bytes written to outBuffer (always a multiple of 4).
uint32_t USBMIDIBuildBulkOut(const uint8_t *midiBytes,
                             uint32_t byteCount,
                             uint8_t cableNumber,
                             uint8_t *outBuffer,
                             uint32_t outBufferSize);

#endif /* USBMIDIParser_h */
