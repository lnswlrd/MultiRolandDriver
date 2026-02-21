#ifndef RolandUSBDevice_h
#define RolandUSBDevice_h

#include <CoreFoundation/CoreFoundation.h>
#include <CoreMIDI/CoreMIDI.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>
#if __has_include(<IOUSBHost/IOUSBHost.h>) && defined(__OBJC__)
#import <IOUSBHost/IOUSBHost.h>
#define HAVE_IOUSBHOST 1
#else
#define HAVE_IOUSBHOST 0
// When not compiling as Objective-C(++) we avoid importing IOUSBHost headers
// and rely on opaque placeholders. Real IOUSBHost usage must be implemented
// in Objective-C++ (.mm) translation units that import the framework.
#endif
#include <IOKit/IOCFPlugIn.h>
#include <unistd.h>

// Supported Roland devices (all share VID 0x0582)
#define kMaxPortsPerDevice 4

struct RolandPortInfo {
    const char *name;   // Entity name in CoreMIDI (e.g. "FA-06/07/08 DAW CTRL")
    uint8_t cable;      // USB-MIDI cable number (0-15)
};

struct RolandDeviceInfo {
    const char *name;
    uint16_t   productID;
    uint8_t    numPorts;  // Number of MIDI cable ports
    RolandPortInfo ports[kMaxPortsPerDevice];
};

static const uint16_t kRolandVendorIDValue = 0x0582;

static const RolandDeviceInfo kSupportedDevices[] = {
    { "Roland SC-8850",          0x0003, 4, {{ "SC-8850 Part A", 0 }, { "SC-8850 Part B", 1 }, { "SC-8850 Part C", 2 }, { "SC-8850 Part D", 3 }} },
    { "Roland SC-8820",          0x0007, 2, {{ "SC-8820 Part A", 0 }, { "SC-8820 Part B", 1 }} },
    { "Roland SK-500",           0x000B, 2, {{ "SK-500 Part A", 0 }, { "SK-500 Part B", 1 }} },
    { "Roland SC-D70",           0x000C, 2, {{ "SC-D70 Part A", 0 }, { "SC-D70 Part B", 1 }} },
    { "Roland XV-5050",          0x0012, 1, {{ "XV-5050", 0 }} },
    { "Roland SD-90",            0x0016, 2, {{ "SD-90 MIDI 1", 0 }, { "SD-90 MIDI 2", 1 }} },
    { "Roland V-Synth",          0x001D, 1, {{ "V-Synth", 0 }} },
    { "Roland SD-20",            0x0027, 1, {{ "SD-20", 0 }} },
    { "Roland SD-80",            0x0029, 2, {{ "SD-80 MIDI 1", 0 }, { "SD-80 MIDI 2", 1 }} },
    { "Roland XV-2020",          0x002D, 1, {{ "XV-2020", 0 }} },
    { "Edirol PCR",              0x0033, 3, {{ "PCR MIDI", 0 }, { "PCR 1", 1 }, { "PCR 2", 2 }} },
    { "Roland Fantom-X",         0x006D, 1, {{ "Fantom-X", 0 }} },
    { "Roland G-70",             0x0080, 2, {{ "G-70 MIDI", 0 }, { "G-70 Control", 1 }} },
    { "Roland V-Synth XT",       0x0084, 1, {{ "V-Synth XT", 0 }} },
    { "Roland Juno-G",           0x00A6, 1, {{ "Juno-G", 0 }} },
    { "Roland MC-808",           0x00A9, 1, {{ "MC-808", 0 }} },
    { "Roland SH-201",           0x00AD, 1, {{ "SH-201", 0 }} },
    { "Roland SonicCell",        0x00C2, 1, {{ "SonicCell", 0 }} },
    { "Roland V-Synth GT",       0x00C7, 1, {{ "V-Synth GT", 0 }} },
    { "Roland Fantom-G",         0x00DE, 1, {{ "Fantom-G", 0 }} },
    { "Roland Juno-Di/Stage",    0x00F8, 1, {{ "JUNO", 0 }} },
    { "Roland GAIA SH-01",       0x0111, 1, {{ "GAIA SH-01", 0 }} },
    { "Roland Lucina AX-09",     0x011C, 1, {{ "Lucina AX-09", 0 }} },
    { "Roland Juno-Gi",          0x0123, 1, {{ "Juno-Gi", 0 }} },
    { "Roland Jupiter-80",       0x013A, 1, {{ "Jupiter-80", 0 }} },
    { "Roland Jupiter-50",       0x0154, 1, {{ "Jupiter-50", 0 }} },
    { "Roland INTEGRA-7",        0x015B, 1, {{ "INTEGRA-7", 0 }} },
    { "Roland FA-06/07/08",         0x0174, 2, {{ "FA-06/07/08", 0 }, { "FA-06/07/08 DAW CTRL", 1 }} },
    { "Roland JD-Xi",            0x01A1, 1, {{ "JD-Xi", 0 }} },
    // Interfaces
    { "Roland UM-ONE",           0x012A, 1, {{ "UM-ONE", 0 }} },
    { "Roland QUAD-CAPTURE",     0x012F, 1, {{ "QUAD-CAPTURE", 0 }} },
};

static const size_t kNumSupportedDevices = sizeof(kSupportedDevices) / sizeof(kSupportedDevices[0]);

/// Find device info for a given product ID. Returns nullptr if not supported.
const RolandDeviceInfo *FindRolandDevice(uint16_t productID);

/// Manages USB I/O for a single Roland device
class RolandUSBDevice {
public:
    RolandUSBDevice(io_service_t usbService, const RolandDeviceInfo *info);
    ~RolandUSBDevice();

    bool Open();
    void Close();

    bool StartIO(CFRunLoopRef runLoop);
    void StopIO();

    /// Send raw MIDI bytes to USB bulk OUT on a given cable
    bool SendMIDI(uint8_t cable, const uint8_t *data, uint32_t length);

    static constexpr uint32_t kSysExChunkSize  = 256;    // MIDI bytes per chunk
    static constexpr useconds_t kSysExChunkDelay = 20000; // 20ms between chunks

    // MIDI device/endpoint associations (multi-port)
    MIDIDeviceRef    midiDevice                     = 0;
    MIDIEntityRef    midiEntities[kMaxPortsPerDevice] = {};
    MIDIEndpointRef  midiSources[kMaxPortsPerDevice]  = {};  // USB IN → CoreMIDI
    MIDIEndpointRef  midiDests[kMaxPortsPerDevice]    = {};   // CoreMIDI → USB OUT

    const RolandDeviceInfo *deviceInfo = nullptr;
    io_service_t    service    = 0;
    uint64_t        locationID = 0;
    io_object_t     removalNotification = 0;
    bool            isOnline   = false;

    void UpdateService(io_service_t newService);

    // Callback context for the driver to deliver received MIDI
    MIDIDriverRef   driverRef  = nullptr;

private:
    bool FindInterface();
    bool FindPipes();
    void SubmitRead();
    static void ReadCallback(void *refCon, IOReturn result, void *arg0);
    bool SendSysExThrottled(uint8_t cable, const uint8_t *data, uint32_t length);

#if HAVE_IOUSBHOST
    // IOUSBHost-based helpers (scaffolding for migration)
    bool OpenHost();
    void CloseHost();
    bool FindInterfaceHost();
    bool FindPipesHost();
    bool StartIOHost(CFRunLoopRef runLoop);
    void StopIOHost();
    void SubmitReadHost();
#endif

    // Legacy IOUSBLib interfaces (kept for compatibility during migration)
    IOUSBDeviceInterface650    **deviceIntf    = nullptr;
    IOUSBInterfaceInterface650 **interfaceIntf = nullptr;

    // IOUSBHost objects (optional - used when HAVE_IOUSBHOST)
#if HAVE_IOUSBHOST
    IOUSBHostDevice *hostDevice = nullptr;
    IOUSBHostInterface *hostInterface = nullptr;
    IOUSBHostPipe *hostInPipe = nullptr;
    IOUSBHostPipe *hostOutPipe = nullptr;
#endif

    bool     deviceOpened    = false;
    uint8_t  bulkInPipeRef   = 0;
    uint8_t  bulkOutPipeRef  = 0;
    uint8_t  rxBuffer[64]    = {};
    bool     ioRunning       = false;

    CFRunLoopSourceRef asyncSource = nullptr;

    // If we successfully migrate this device to IOUSBHost, set this true
    bool     usingIOUSBHost   = false;
};

#endif /* RolandUSBDevice_h */
