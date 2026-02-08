#include "USBMIDIParser.h"

uint8_t USBMIDICinToMIDIByteCount(uint8_t cin)
{
    switch (cin) {
        case kCIN_Misc:              return 0;
        case kCIN_CableEvent:        return 0;
        case kCIN_SystemCommon2Byte:  return 2;
        case kCIN_SystemCommon3Byte:  return 3;
        case kCIN_SysExStart:        return 3;
        case kCIN_SysExEnd1Byte:     return 1;
        case kCIN_SysExEnd2Byte:     return 2;
        case kCIN_SysExEnd3Byte:     return 3;
        case kCIN_NoteOff:           return 3;
        case kCIN_NoteOn:            return 3;
        case kCIN_PolyAftertouch:    return 3;
        case kCIN_ControlChange:     return 3;
        case kCIN_ProgramChange:     return 2;
        case kCIN_ChannelPressure:   return 2;
        case kCIN_PitchBend:         return 3;
        case kCIN_SingleByte:        return 1;
        default:                     return 0;
    }
}

uint8_t MIDIStatusToCin(uint8_t statusByte)
{
    if (statusByte < 0x80) return kCIN_Misc;

    uint8_t highNibble = statusByte >> 4;
    switch (highNibble) {
        case 0x8: return kCIN_NoteOff;
        case 0x9: return kCIN_NoteOn;
        case 0xA: return kCIN_PolyAftertouch;
        case 0xB: return kCIN_ControlChange;
        case 0xC: return kCIN_ProgramChange;
        case 0xD: return kCIN_ChannelPressure;
        case 0xE: return kCIN_PitchBend;
        case 0xF:
            switch (statusByte) {
                case 0xF0: return kCIN_SysExStart;
                case 0xF1: return kCIN_SystemCommon2Byte;
                case 0xF2: return kCIN_SystemCommon3Byte;
                case 0xF3: return kCIN_SystemCommon2Byte;
                case 0xF6: return kCIN_SysExEnd1Byte;
                case 0xF7: return kCIN_SysExEnd1Byte;
                case 0xF8: case 0xFA: case 0xFB: case 0xFC:
                case 0xFE: case 0xFF:
                    return kCIN_SingleByte;
                default:   return kCIN_Misc;
            }
        default: return kCIN_Misc;
    }
}

void USBMIDIParseBulkIn(const uint8_t *data,
                        uint32_t length,
                        USBMIDIParseCallback callback,
                        void *context)
{
    if (!data || !callback) return;

    for (uint32_t offset = 0; offset + 4 <= length; offset += 4) {
        uint8_t header = data[offset];
        uint8_t cin   = header & 0x0F;
        uint8_t cable = (header >> 4) & 0x0F;

        uint8_t byteCount = USBMIDICinToMIDIByteCount(cin);
        if (byteCount == 0) continue;

        const uint8_t *midiBytes = &data[offset + 1];
        callback(cable, midiBytes, byteCount, context);
    }
}

static uint8_t channelMessageLength(uint8_t statusByte)
{
    switch (statusByte & 0xF0) {
        case 0xC0: return 2;
        case 0xD0: return 2;
        default:   return 3;
    }
}

uint32_t USBMIDIBuildBulkOut(const uint8_t *midiBytes,
                             uint32_t byteCount,
                             uint8_t cableNumber,
                             uint8_t *outBuffer,
                             uint32_t outBufferSize)
{
    if (!midiBytes || !outBuffer || byteCount == 0) return 0;

    uint32_t outOffset = 0;
    uint32_t i = 0;
    bool inSysex = false;
    uint8_t sysexAccum[3];
    uint8_t sysexCount = 0;

    while (i < byteCount && outOffset + 4 <= outBufferSize) {
        uint8_t b = midiBytes[i];

        if (b == 0xF0) {
            inSysex = true;
            sysexAccum[0] = b;
            sysexCount = 1;
            i++;
        } else if (inSysex) {
            if (b == 0xF7) {
                sysexAccum[sysexCount] = b;
                sysexCount++;

                uint8_t cin;
                switch (sysexCount) {
                    case 1: cin = kCIN_SysExEnd1Byte; break;
                    case 2: cin = kCIN_SysExEnd2Byte; break;
                    case 3: cin = kCIN_SysExEnd3Byte; break;
                    default: cin = kCIN_SysExEnd1Byte; break;
                }

                outBuffer[outOffset + 0] = (cableNumber << 4) | cin;
                outBuffer[outOffset + 1] = (sysexCount >= 1) ? sysexAccum[0] : 0;
                outBuffer[outOffset + 2] = (sysexCount >= 2) ? sysexAccum[1] : 0;
                outBuffer[outOffset + 3] = (sysexCount >= 3) ? sysexAccum[2] : 0;
                outOffset += 4;
                inSysex = false;
                sysexCount = 0;
                i++;
            } else if (b >= 0x80 && b != 0xF7) {
                // Real-time inside SysEx
                outBuffer[outOffset + 0] = (cableNumber << 4) | kCIN_SingleByte;
                outBuffer[outOffset + 1] = b;
                outBuffer[outOffset + 2] = 0;
                outBuffer[outOffset + 3] = 0;
                outOffset += 4;
                i++;
            } else {
                sysexAccum[sysexCount] = b;
                sysexCount++;
                i++;
                if (sysexCount == 3) {
                    outBuffer[outOffset + 0] = (cableNumber << 4) | kCIN_SysExStart;
                    outBuffer[outOffset + 1] = sysexAccum[0];
                    outBuffer[outOffset + 2] = sysexAccum[1];
                    outBuffer[outOffset + 3] = sysexAccum[2];
                    outOffset += 4;
                    sysexCount = 0;
                }
            }
        } else if (b >= 0x80) {
            if (b >= 0xF8) {
                outBuffer[outOffset + 0] = (cableNumber << 4) | kCIN_SingleByte;
                outBuffer[outOffset + 1] = b;
                outBuffer[outOffset + 2] = 0;
                outBuffer[outOffset + 3] = 0;
                outOffset += 4;
                i++;
            } else if (b >= 0xF0) {
                uint8_t cin = MIDIStatusToCin(b);
                uint8_t msgLen = USBMIDICinToMIDIByteCount(cin);
                if (i + msgLen > byteCount) break;
                outBuffer[outOffset + 0] = (cableNumber << 4) | cin;
                outBuffer[outOffset + 1] = (msgLen >= 1) ? midiBytes[i] : 0;
                outBuffer[outOffset + 2] = (msgLen >= 2) ? midiBytes[i + 1] : 0;
                outBuffer[outOffset + 3] = (msgLen >= 3) ? midiBytes[i + 2] : 0;
                outOffset += 4;
                i += msgLen;
            } else {
                uint8_t msgLen = channelMessageLength(b);
                if (i + msgLen > byteCount) break;
                uint8_t cin = MIDIStatusToCin(b);
                outBuffer[outOffset + 0] = (cableNumber << 4) | cin;
                outBuffer[outOffset + 1] = midiBytes[i];
                outBuffer[outOffset + 2] = (msgLen >= 2) ? midiBytes[i + 1] : 0;
                outBuffer[outOffset + 3] = (msgLen >= 3) ? midiBytes[i + 2] : 0;
                outOffset += 4;
                i += msgLen;
            }
        } else {
            i++; // Skip data byte without status
        }
    }

    return outOffset;
}
