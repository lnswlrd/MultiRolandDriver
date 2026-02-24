#include "RolandUSBDevice.h"
#include "USBMIDIParser.h"
#include <CoreMIDI/MIDIDriver.h>
#include <CoreMIDI/MIDISetup.h>
#include <IOKit/usb/IOUSBLib.h>
#include <IOKit/IOMessage.h>
#include <os/log.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <vector>
#include <mutex>

static os_log_t sLog = os_log_create("se.cutup.MultiRolandDriver", "driver");

// Factory UUID — must match Info.plist CFPlugInFactories key
#define kDriverFactoryUUID CFUUIDGetConstantUUIDWithBytes(NULL, \
    0xE3, 0xE5, 0xB6, 0xC8, 0x2F, 0x4A, 0x4B, 0x1D, \
    0x9C, 0x7E, 0xA8, 0xD2, 0xF1, 0xB3, 0xC5, 0xE7)

// ---------- Forward declarations ----------
static HRESULT  DrvQueryInterface(void *self, REFIID iid, LPVOID *ppv);
static ULONG    DrvAddRef(void *self);
static ULONG    DrvRelease(void *self);
static OSStatus DrvFindDevices(MIDIDriverRef self, MIDIDeviceListRef devList);
static OSStatus DrvStart(MIDIDriverRef self, MIDIDeviceListRef devList);
static OSStatus DrvStop(MIDIDriverRef self);
static OSStatus DrvConfigure(MIDIDriverRef self, MIDIDeviceRef device);
static OSStatus DrvSend(MIDIDriverRef self, const MIDIPacketList *pktlist,
                         void *destConnRefCon, void *endptRefCon);
static OSStatus DrvEnableSource(MIDIDriverRef self, MIDIEndpointRef src, Boolean enabled);
static OSStatus DrvFlush(MIDIDriverRef self, MIDIEndpointRef dest,
                          void *destConnRefCon0, void *destConnRefCon1);
static OSStatus DrvMonitor(MIDIDriverRef self, MIDIEndpointRef dest,
                            const MIDIPacketList *pktlist);
static OSStatus DrvSendPackets(MIDIDriverRef self, const MIDIEventList *evtlist,
                                void *destRefCon1, void *destRefCon2);
static OSStatus DrvMonitorEvents(MIDIDriverRef self, MIDIEndpointRef dest,
                                  const MIDIEventList *evtlist);

// ---------- Vtable ----------
static MIDIDriverInterface sDriverVtable = {
    NULL,               // _reserved
    DrvQueryInterface,
    DrvAddRef,
    DrvRelease,
    DrvFindDevices,
    DrvStart,
    DrvStop,
    DrvConfigure,
    DrvSend,
    DrvEnableSource,
    DrvFlush,
    DrvMonitor,
    DrvSendPackets,
    DrvMonitorEvents
};

// ---------- Port mapping for outgoing MIDI routing ----------
struct PortMapping {
    RolandUSBDevice *device;
    uint8_t cable;
};

// ---------- Driver state ----------
struct MultiRolandDriverState {
    MIDIDriverInterface *vtable;    // Must be first field (COM layout)
    UInt32 refCount;
    CFUUIDRef factoryID;

    std::vector<RolandUSBDevice *> devices;
    std::vector<PortMapping> portMappings;
    std::mutex devicesMutex;

    // USB hotplug notification
    IONotificationPortRef notifyPort;
    io_iterator_t addedIter;
    CFRunLoopRef runLoop;

    MultiRolandDriverState()
        : vtable(&sDriverVtable)
        , refCount(1)
        , factoryID(nullptr)
        , notifyPort(nullptr)
        , addedIter(0)
        , runLoop(nullptr) {}

    ~MultiRolandDriverState() {
        for (auto *dev : devices)
            delete dev;
    }
};

static inline MultiRolandDriverState *GetState(void *self) {
    return reinterpret_cast<MultiRolandDriverState *>(self);
}

static inline MultiRolandDriverState *GetState(MIDIDriverRef self) {
    return reinterpret_cast<MultiRolandDriverState *>(self);
}

// ---------- IUnknown ----------

static HRESULT DrvQueryInterface(void *self, REFIID iid, LPVOID *ppv)
{
    CFUUIDRef requestedUUID = CFUUIDCreateFromUUIDBytes(NULL, iid);

    if (CFEqual(requestedUUID, IUnknownUUID) ||
        CFEqual(requestedUUID, kMIDIDriverInterfaceID)) {
        CFRelease(requestedUUID);
        DrvAddRef(self);
        *ppv = self;
        return S_OK;
    }

    CFRelease(requestedUUID);
    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG DrvAddRef(void *self)
{
    auto *state = GetState(self);
    return ++state->refCount;
}

static ULONG DrvRelease(void *self)
{
    auto *state = GetState(self);
    UInt32 count = --state->refCount;
    if (count == 0) {
        CFPlugInRemoveInstanceForFactory(state->factoryID);
        CFRelease(state->factoryID);
        delete state;
    }
    return count;
}

// ---------- USB scanning ----------

static void ScanUSBDevices(MultiRolandDriverState *state, MIDIDriverRef driverRef)
{
    CFMutableDictionaryRef matchDict = IOServiceMatching("IOUSBHostDevice");
    if (!matchDict) return;

    io_iterator_t iter = 0;
    kern_return_t kr = IOServiceGetMatchingServices(kIOMainPortDefault, matchDict, &iter);
    if (kr != kIOReturnSuccess) return;

    io_service_t usbService;
    while ((usbService = IOIteratorNext(iter)) != 0) {
        // Read vendor ID and filter for Roland
        CFNumberRef vidRef = (CFNumberRef)IORegistryEntryCreateCFProperty(
            usbService, CFSTR("idVendor"), NULL, 0);
        if (vidRef) {
            SInt32 vid = 0;
            CFNumberGetValue(vidRef, kCFNumberSInt32Type, &vid);
            CFRelease(vidRef);
            if ((uint16_t)vid != kRolandVendorIDValue) {
                IOObjectRelease(usbService);
                continue;
            }
        } else {
            IOObjectRelease(usbService);
            continue;
        }

        CFNumberRef pidRef = (CFNumberRef)IORegistryEntryCreateCFProperty(
            usbService, CFSTR("idProduct"), NULL, 0);

        if (pidRef) {
            SInt32 pid = 0;
            CFNumberGetValue(pidRef, kCFNumberSInt32Type, &pid);
            CFRelease(pidRef);

            const RolandDeviceInfo *info = FindRolandDevice((uint16_t)pid);
            if (info) {
                // Get location ID to identify duplicates
                UInt32 locID = 0;
                CFNumberRef locRef = (CFNumberRef)IORegistryEntryCreateCFProperty(
                    usbService, CFSTR("locationID"), NULL, 0);
                if (locRef) {
                    CFNumberGetValue(locRef, kCFNumberSInt32Type, &locID);
                    CFRelease(locRef);
                }

                bool alreadyTracked = false;
                for (auto *dev : state->devices) {
                    if (dev->locationID == (uint64_t)locID) {
                        alreadyTracked = true;
                        break;
                    }
                }

                if (!alreadyTracked) {
                    auto *dev = new RolandUSBDevice(usbService, info);
                    dev->driverRef = driverRef;
                    dev->locationID = locID;
                    state->devices.push_back(dev);
                    os_log(sLog, "Found %{public}s (PID 0x%04X)",
                           info->name, info->productID);
                }
            }
        }

        IOObjectRelease(usbService);
    }

    IOObjectRelease(iter);
}

// ---------- Realtime thread priority ----------

static void SetRealtimePriority()
{
    mach_timebase_info_data_t timebase;
    mach_timebase_info(&timebase);
    double nsToAbs = (double)timebase.denom / (double)timebase.numer;

    thread_time_constraint_policy_data_t policy;
    policy.period      = (uint32_t)(1000000 * nsToAbs);  // 1ms
    policy.computation = (uint32_t)(500000 * nsToAbs);   // 0.5ms
    policy.constraint  = (uint32_t)(1000000 * nsToAbs);  // 1ms
    policy.preemptible = true;

    thread_policy_set(mach_thread_self(),
                      THREAD_TIME_CONSTRAINT_POLICY,
                      (thread_policy_t)&policy,
                      THREAD_TIME_CONSTRAINT_POLICY_COUNT);

    os_log(sLog, "Realtime thread priority set");
}

// ---------- Device removal callback ----------

static void DeviceRemoved(void *refCon, io_service_t /*service*/,
                           natural_t messageType, void * /*messageArgument*/)
{
    if (messageType != kIOMessageServiceIsTerminated) return;

    auto *dev = static_cast<RolandUSBDevice *>(refCon);
    os_log(sLog, "DeviceRemoved: %{public}s disconnected", dev->deviceInfo->name);

    dev->StopIO();
    dev->Close();
    dev->isOnline = false;

    // Mark offline so Audio MIDI Setup grays it out.
    // Use MIDISetupRemoveDevice only for hotplug-created devices (those added
    // via MIDISetupAddDevice). Devices from FindDevices stay in setup persistently.
    if (dev->midiDevice)
        MIDIObjectSetIntegerProperty(dev->midiDevice, kMIDIPropertyOffline, 1);

    if (dev->removalNotification) {
        IOObjectRelease(dev->removalNotification);
        dev->removalNotification = 0;
    }
}

static void RegisterRemovalNotification(MultiRolandDriverState *state, RolandUSBDevice *dev)
{
    if (!state->notifyPort) return;

    kern_return_t kr = IOServiceAddInterestNotification(
        state->notifyPort, dev->service,
        kIOGeneralInterest, DeviceRemoved, dev,
        &dev->removalNotification);

    if (kr != kIOReturnSuccess) {
        os_log_error(sLog, "RegisterRemovalNotification: failed for %{public}s (0x%x)",
                     dev->deviceInfo->name, kr);
    }
}

// ---------- Hotplug callback ----------

static void DeviceAdded(void *refCon, io_iterator_t iterator)
{
    auto *state = reinterpret_cast<MultiRolandDriverState *>(refCon);

    io_service_t usbService;
    while ((usbService = IOIteratorNext(iterator)) != 0) {
        // Filter for Roland vendor ID
        CFNumberRef vidRef = (CFNumberRef)IORegistryEntryCreateCFProperty(
            usbService, CFSTR("idVendor"), NULL, 0);
        if (vidRef) {
            SInt32 vid = 0;
            CFNumberGetValue(vidRef, kCFNumberSInt32Type, &vid);
            CFRelease(vidRef);
            if ((uint16_t)vid != kRolandVendorIDValue) {
                IOObjectRelease(usbService);
                continue;
            }
        } else {
            IOObjectRelease(usbService);
            continue;
        }

        CFNumberRef pidRef = (CFNumberRef)IORegistryEntryCreateCFProperty(
            usbService, CFSTR("idProduct"), NULL, 0);

        if (pidRef) {
            SInt32 pid = 0;
            CFNumberGetValue(pidRef, kCFNumberSInt32Type, &pid);
            CFRelease(pidRef);

            const RolandDeviceInfo *info = FindRolandDevice((uint16_t)pid);
            if (info) {
                // Read location ID
                UInt32 locID = 0;
                CFNumberRef locRef2 = (CFNumberRef)IORegistryEntryCreateCFProperty(
                    usbService, CFSTR("locationID"), NULL, 0);
                if (locRef2) {
                    CFNumberGetValue(locRef2, kCFNumberSInt32Type, &locID);
                    CFRelease(locRef2);
                }

                // Wait for USB interfaces to settle after hotplug.
                usleep(500000); // 500ms

                std::lock_guard<std::mutex> lock(state->devicesMutex);
                MIDIDriverRef driverRef = (MIDIDriverRef)state;

                // Skip if this locationID is already tracked and online
                // (happens when the drain in DrvStart sees a device that
                // FindDevices + DrvStart already opened).
                bool alreadyOnline = false;
                for (auto *dev : state->devices) {
                    if (dev->isOnline && dev->locationID == (uint64_t)locID) {
                        alreadyOnline = true;
                        break;
                    }
                }
                if (alreadyOnline) {
                    IOObjectRelease(usbService);
                    continue;
                }

                // Check if we already have an offline device with same locationID
                // (device was disconnected and reconnected).
                RolandUSBDevice *existingDev = nullptr;
                for (auto *dev : state->devices) {
                    if (!dev->isOnline && dev->locationID == (uint64_t)locID) {
                        existingDev = dev;
                        break;
                    }
                }

                if (existingDev) {
                    // Reconnect existing offline device
                    existingDev->UpdateService(usbService);
                    if (existingDev->Open()) {
                        CFRunLoopRef rl = state->runLoop ? state->runLoop
                                                         : CFRunLoopGetCurrent();
                        existingDev->StartIO(rl);
                        existingDev->isOnline = true;
                        RegisterRemovalNotification(state, existingDev);

                        if (existingDev->midiDevice)
                            MIDIObjectSetIntegerProperty(existingDev->midiDevice,
                                                         kMIDIPropertyOffline, 0);

                        os_log(sLog, "Hotplug: reconnected %{public}s", info->name);
                    } else {
                        os_log_error(sLog, "Hotplug: failed to reopen %{public}s", info->name);
                    }
                } else {
                    // Create new device
                    auto *dev = new RolandUSBDevice(usbService, info);
                    dev->driverRef = driverRef;

                    if (dev->Open()) {
                        CFStringRef devName = CFStringCreateWithCString(
                            NULL, info->name, kCFStringEncodingUTF8);

                        MIDIDeviceCreate(driverRef, devName, CFSTR("Roland"),
                                         devName, &dev->midiDevice);

                        for (uint8_t p = 0; p < info->numPorts; p++) {
                            CFStringRef portName = CFStringCreateWithCString(
                                NULL, info->ports[p].name, kCFStringEncodingUTF8);
                            MIDIDeviceAddEntity(dev->midiDevice, portName, false,
                                                1, 1, &dev->midiEntities[p]);
                            dev->midiSources[p] = MIDIEntityGetSource(dev->midiEntities[p], 0);
                            dev->midiDests[p]   = MIDIEntityGetDestination(dev->midiEntities[p], 0);

                            size_t globalIdx = state->portMappings.size();
                            state->portMappings.push_back({dev, info->ports[p].cable});
                            MIDIEndpointSetRefCons(dev->midiDests[p],
                                                   (void *)(uintptr_t)(globalIdx + 1), NULL);
                            CFRelease(portName);
                        }

                        CFRelease(devName);

                        // Register with CoreMIDI — required for devices added
                        // outside of FindDevices (hotplug / boot drain).
                        MIDISetupAddDevice(dev->midiDevice);

                        CFRunLoopRef rl = state->runLoop ? state->runLoop
                                                         : CFRunLoopGetCurrent();
                        dev->StartIO(rl);
                        dev->isOnline = true;
                        dev->locationID = locID;
                        RegisterRemovalNotification(state, dev);

                        state->devices.push_back(dev);
                        os_log(sLog, "Hotplug: added %{public}s (%u port(s))",
                               info->name, info->numPorts);
                    } else {
                        delete dev;
                    }
                }
            }
        }

        IOObjectRelease(usbService);
    }
}

// ---------- MIDIDriverInterface ----------

static OSStatus DrvFindDevices(MIDIDriverRef self, MIDIDeviceListRef devList)
{
    auto *state = GetState(self);

    ScanUSBDevices(state, self);

    // Reset port mappings
    state->portMappings.clear();

    // CoreMIDI calls FindDevices multiple times. We must call MIDIDeviceCreate
    // on every call — CoreMIDI uses the returned devList as the authoritative
    // list and needs fresh MIDIDeviceRef objects each time. (Attempting to reuse
    // an existing MIDIDeviceRef from a prior call causes the device to go missing.)
    for (auto *dev : state->devices) {
        CFStringRef devName = CFStringCreateWithCString(
            NULL, dev->deviceInfo->name, kCFStringEncodingUTF8);

        MIDIDeviceCreate(self, devName, CFSTR("Roland"), devName,
                         &dev->midiDevice);

        for (uint8_t p = 0; p < dev->deviceInfo->numPorts; p++) {
            CFStringRef portName = CFStringCreateWithCString(
                NULL, dev->deviceInfo->ports[p].name, kCFStringEncodingUTF8);
            MIDIDeviceAddEntity(dev->midiDevice, portName, false,
                                1, 1, &dev->midiEntities[p]);
            dev->midiSources[p] = MIDIEntityGetSource(dev->midiEntities[p], 0);
            dev->midiDests[p]   = MIDIEntityGetDestination(dev->midiEntities[p], 0);

            // Refcon encodes global port index (1-based to distinguish from NULL).
            // portMappings is rebuilt from scratch each call (cleared above).
            size_t globalIdx = state->portMappings.size();
            state->portMappings.push_back({dev, dev->deviceInfo->ports[p].cable});
            MIDIEndpointSetRefCons(dev->midiDests[p],
                                   (void *)(uintptr_t)(globalIdx + 1), NULL);
            CFRelease(portName);
        }

        MIDIDeviceListAddDevice(devList, dev->midiDevice);
        CFRelease(devName);

        os_log(sLog, "FindDevices: registered %{public}s (%u port(s))",
               dev->deviceInfo->name, dev->deviceInfo->numPorts);
    }

    os_log(sLog, "FindDevices: %zu device(s), %zu port(s)",
           state->devices.size(), state->portMappings.size());
    return noErr;
}

static OSStatus DrvStart(MIDIDriverRef self, MIDIDeviceListRef devList)
{
    auto *state = GetState(self);
    state->runLoop = CFRunLoopGetCurrent();

    SetRealtimePriority();

    // Re-read MIDI device/entity/endpoint refs from devList.
    // Per MIDIDriver.h, the driver must NOT retain refs from FindDevices;
    // the devList passed to Start is the authoritative source.
    // CoreMIDI guarantees devList contains the same MIDIDeviceRef objects
    // in the same order as FindDevices populated them.
    state->portMappings.clear();
    ItemCount numMidiDevs = MIDIDeviceListGetNumberOfDevices(devList);
    os_log(sLog, "Start: devList contains %lu device(s), state->devices has %zu",
           (unsigned long)numMidiDevs, state->devices.size());

    for (ItemCount mi = 0; mi < numMidiDevs && mi < (ItemCount)state->devices.size(); mi++) {
        RolandUSBDevice *dev = state->devices[mi];
        MIDIDeviceRef midiDev = MIDIDeviceListGetDevice(devList, mi);
        dev->midiDevice = midiDev;

        ItemCount numEntities = MIDIDeviceGetNumberOfEntities(midiDev);
        os_log(sLog, "Start: devList[%lu] = midiDev %lu, numEntities=%lu (expected %u for %{public}s)",
               (unsigned long)mi,
               (unsigned long)midiDev,
               (unsigned long)numEntities,
               dev->deviceInfo->numPorts,
               dev->deviceInfo->name);

        for (ItemCount p = 0; p < numEntities && p < (ItemCount)dev->deviceInfo->numPorts; p++) {
            MIDIEntityRef ent = MIDIDeviceGetEntity(midiDev, p);
            dev->midiEntities[p] = ent;

            ItemCount nSrc  = MIDIEntityGetNumberOfSources(ent);
            ItemCount nDest = MIDIEntityGetNumberOfDestinations(ent);
            dev->midiSources[p] = (nSrc  > 0) ? MIDIEntityGetSource(ent, 0)      : 0;
            dev->midiDests[p]   = (nDest > 0) ? MIDIEntityGetDestination(ent, 0) : 0;

            os_log(sLog,
                   "Start:   entity[%lu] ent=%lu nSrc=%lu nDest=%lu src=%lu dst=%lu",
                   (unsigned long)p,
                   (unsigned long)ent,
                   (unsigned long)nSrc,
                   (unsigned long)nDest,
                   (unsigned long)dev->midiSources[p],
                   (unsigned long)dev->midiDests[p]);

            size_t globalIdx = state->portMappings.size();
            state->portMappings.push_back({dev, dev->deviceInfo->ports[p].cable});
            if (dev->midiDests[p])
                MIDIEndpointSetRefCons(dev->midiDests[p],
                                       (void *)(uintptr_t)(globalIdx + 1), NULL);
        }
    }
    os_log(sLog, "Start: portMappings rebuilt: %zu entries", state->portMappings.size());

    // Create notification port before opening devices (needed for removal notifications)
    state->notifyPort = IONotificationPortCreate(kIOMainPortDefault);
    if (state->notifyPort) {
        CFRunLoopSourceRef notifySrc =
            IONotificationPortGetRunLoopSource(state->notifyPort);
        CFRunLoopAddSource(state->runLoop, notifySrc, kCFRunLoopDefaultMode);
    }

    // Open USB and start I/O for each device
    for (auto *dev : state->devices) {
        if (dev->Open()) {
            dev->StartIO(state->runLoop);
            dev->isOnline = true;
            
            // Mark endpoints as available
            if (dev->midiDevice)
                MIDIObjectSetIntegerProperty(dev->midiDevice,
                                            kMIDIPropertyOffline, 0);
            RegisterRemovalNotification(state, dev);
            os_log(sLog, "Start: opened %{public}s", dev->deviceInfo->name);
        } else {
            // Keep device in list but marked offline
            dev->isOnline = false;
            if (dev->midiDevice)
                MIDIObjectSetIntegerProperty(dev->midiDevice,
                                            kMIDIPropertyOffline, 1);
            os_log(sLog, "Start: %{public}s not ready yet", dev->deviceInfo->name);
        }
    }

    // Register for USB hotplug notifications (all USB devices, filtered in callback)
    if (state->notifyPort) {
        CFMutableDictionaryRef matchDict =
            IOServiceMatching("IOUSBHostDevice");
        if (matchDict) {
            IOServiceAddMatchingNotification(
                state->notifyPort, kIOFirstMatchNotification,
                matchDict, DeviceAdded, state, &state->addedIter);

            // Drain the initial iterator (required by IOKit). Devices already
            // handled by FindDevices+DrvStart are online; the hotplug notification
            // will fire for any device that connects after this point.
            io_service_t svc;
            while ((svc = IOIteratorNext(state->addedIter)) != 0)
                IOObjectRelease(svc);
        }
    }

    os_log(sLog, "Started (%zu device(s))", state->devices.size());
    return noErr;
}

static OSStatus DrvStop(MIDIDriverRef self)
{
    auto *state = GetState(self);

    if (state->addedIter) {
        IOObjectRelease(state->addedIter);
        state->addedIter = 0;
    }

    if (state->notifyPort) {
        IONotificationPortDestroy(state->notifyPort);
        state->notifyPort = nullptr;
    }

    for (auto *dev : state->devices) {
        if (dev->removalNotification) {
            IOObjectRelease(dev->removalNotification);
            dev->removalNotification = 0;
        }
        dev->StopIO();
        dev->Close();
        dev->isOnline = false;
    }

    os_log(sLog, "Stopped");
    return noErr;
}

static OSStatus DrvConfigure(MIDIDriverRef /*self*/, MIDIDeviceRef /*device*/)
{
    return noErr;
}

static OSStatus DrvSend(MIDIDriverRef self, const MIDIPacketList *pktlist,
                         void * /*destConnRefCon*/, void *endptRefCon)
{
    auto *state = GetState(self);

    // Look up port mapping via refcon (1-based index into portMappings)
    size_t idx = (size_t)(uintptr_t)endptRefCon;

    const MIDIPacket *pkt = &pktlist->packet[0];
    for (UInt32 i = 0; i < pktlist->numPackets; i++) {
        if (pkt->length > 0) {
            if (idx > 0 && idx <= state->portMappings.size()) {
                auto &pm = state->portMappings[idx - 1];
                pm.device->SendMIDI(pm.cable, pkt->data, pkt->length);
            } else {
                // Fallback: send to all devices on cable 0
                for (auto *dev : state->devices)
                    dev->SendMIDI(0, pkt->data, pkt->length);
            }
        }
        pkt = MIDIPacketNext(pkt);
    }

    return noErr;
}

static OSStatus DrvEnableSource(MIDIDriverRef /*self*/, MIDIEndpointRef /*src*/,
                                 Boolean /*enabled*/)
{
    return noErr;
}

static OSStatus DrvFlush(MIDIDriverRef /*self*/, MIDIEndpointRef /*dest*/,
                          void * /*destConnRefCon0*/, void * /*destConnRefCon1*/)
{
    return noErr;
}

static OSStatus DrvMonitor(MIDIDriverRef /*self*/, MIDIEndpointRef /*dest*/,
                            const MIDIPacketList * /*pktlist*/)
{
    return noErr;
}

static OSStatus DrvSendPackets(MIDIDriverRef /*self*/,
                                const MIDIEventList * /*evtlist*/,
                                void * /*destRefCon1*/, void * /*destRefCon2*/)
{
    // MIDI 2.0 UMP sending — not implemented (MIDI 1.0 devices)
    return noErr;
}

static OSStatus DrvMonitorEvents(MIDIDriverRef /*self*/, MIDIEndpointRef /*dest*/,
                                  const MIDIEventList * /*evtlist*/)
{
    return noErr;
}

// ---------- CFPlugIn factory ----------

extern "C" __attribute__((visibility("default")))
void *MultiRolandDriverCreate(CFAllocatorRef /*alloc*/, CFUUIDRef typeUUID)
{
    if (!CFEqual(typeUUID, kMIDIDriverTypeID))
        return NULL;

    auto *state = new MultiRolandDriverState();
    state->factoryID = (CFUUIDRef)CFRetain(kDriverFactoryUUID);

    CFPlugInAddInstanceForFactory(state->factoryID);

    os_log(sLog, "MultiRolandDriver v1.4.10 loaded");
    return state;
}
