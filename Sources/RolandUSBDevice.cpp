#include "RolandUSBDevice.h"
#include "USBMIDIParser.h"
#include <os/log.h>
#include <mach/mach_time.h>

static os_log_t sLog = os_log_create("se.cutup.MultiRolandDriver", "usb");

const RolandDeviceInfo *FindRolandDevice(uint16_t productID)
{
    for (size_t i = 0; i < kNumSupportedDevices; i++) {
        if (kSupportedDevices[i].productID == productID)
            return &kSupportedDevices[i];
    }
    return nullptr;
}

RolandUSBDevice::RolandUSBDevice(io_service_t usbService, const RolandDeviceInfo *info)
    : deviceInfo(info), service(usbService)
{
    IOObjectRetain(service);
}

RolandUSBDevice::~RolandUSBDevice()
{
    Close();
    if (service) {
        IOObjectRelease(service);
        service = 0;
    }
}

void RolandUSBDevice::UpdateService(io_service_t newService)
{
    if (service) IOObjectRelease(service);
    service = newService;
    IOObjectRetain(service);
}

bool RolandUSBDevice::Open()
{
    IOCFPlugInInterface **plugInIntf = nullptr;
    SInt32 score = 0;

    kern_return_t kr = IOCreatePlugInInterfaceForService(
        service,
        kIOUSBDeviceUserClientTypeID,
        kIOCFPlugInInterfaceID,
        &plugInIntf,
        &score);

    if (kr != kIOReturnSuccess || !plugInIntf) {
        os_log_error(sLog, "Open: failed to create plugin for %{public}s", deviceInfo->name);
        return false;
    }

    HRESULT hr = (*plugInIntf)->QueryInterface(
        plugInIntf,
        CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID650),
        (LPVOID *)&deviceIntf);

    (*plugInIntf)->Release(plugInIntf);

    if (hr != S_OK || !deviceIntf) {
        os_log_error(sLog, "Open: failed to get device interface for %{public}s", deviceInfo->name);
        return false;
    }

    kr = (*deviceIntf)->USBDeviceOpen(deviceIntf);
    if (kr != kIOReturnSuccess && kr != kIOReturnExclusiveAccess) {
        os_log_error(sLog, "Open: USBDeviceOpen failed for %{public}s (0x%x)", deviceInfo->name, kr);
        (*deviceIntf)->Release(deviceIntf);
        deviceIntf = nullptr;
        return false;
    }

    UInt32 locID = 0;
    (*deviceIntf)->GetLocationID(deviceIntf, &locID);
    locationID = locID;

    // Set USB configuration 1 (required for interfaces to appear)
    UInt8 numConf = 0;
    (*deviceIntf)->GetNumberOfConfigurations(deviceIntf, &numConf);

    if (numConf > 0) {
        IOUSBConfigurationDescriptorPtr confDesc = nullptr;
        kr = (*deviceIntf)->GetConfigurationDescriptorPtr(deviceIntf, 0, &confDesc);
        if (kr == kIOReturnSuccess && confDesc)
            (*deviceIntf)->SetConfiguration(deviceIntf, confDesc->bConfigurationValue);
    }

    if (!FindInterface()) {
        os_log_error(sLog, "Open: FindInterface failed for %{public}s", deviceInfo->name);
        Close();
        return false;
    }

    if (!FindPipes()) {
        os_log_error(sLog, "Open: FindPipes failed for %{public}s", deviceInfo->name);
        Close();
        return false;
    }

    os_log(sLog, "Open: %{public}s (locationID=0x%llx)", deviceInfo->name, locationID);
    return true;
}

void RolandUSBDevice::Close()
{
    StopIO();

    if (interfaceIntf) {
        (*interfaceIntf)->USBInterfaceClose(interfaceIntf);
        (*interfaceIntf)->Release(interfaceIntf);
        interfaceIntf = nullptr;
    }

    if (deviceIntf) {
        (*deviceIntf)->USBDeviceClose(deviceIntf);
        (*deviceIntf)->Release(deviceIntf);
        deviceIntf = nullptr;
    }

    bulkInPipeRef = 0;
    bulkOutPipeRef = 0;
}

bool RolandUSBDevice::FindInterface()
{
    // First try legacy CreateInterfaceIterator
    IOUSBFindInterfaceRequest req;
    req.bInterfaceClass    = kIOUSBFindInterfaceDontCare;
    req.bInterfaceSubClass = kIOUSBFindInterfaceDontCare;
    req.bInterfaceProtocol = kIOUSBFindInterfaceDontCare;
    req.bAlternateSetting  = kIOUSBFindInterfaceDontCare;

    io_iterator_t iter = 0;
    kern_return_t kr = (*deviceIntf)->CreateInterfaceIterator(deviceIntf, &req, &iter);

    if (kr == kIOReturnSuccess) {
        io_service_t intfService;
        while ((intfService = IOIteratorNext(iter)) != 0) {
            if (OpenUSBInterface(intfService)) {
                IOObjectRelease(iter);
                return true;
            }
        }
        IOObjectRelease(iter);
    }
    // Fallback: find IOUSBHostInterface children of the device service
    io_iterator_t childIter = 0;
    kr = IORegistryEntryGetChildIterator(service, kIOServicePlane, &childIter);
    if (kr != kIOReturnSuccess) {
        os_log_error(sLog, "FindInterface: GetChildIterator failed (0x%x)", kr);
        return false;
    }

    io_service_t child;
    int childCount = 0;
    while ((child = IOIteratorNext(childIter)) != 0) {
        childCount++;
        if (IOObjectConformsTo(child, "IOUSBHostInterface")) {
            if (OpenUSBInterface(child)) {
                IOObjectRelease(childIter);
                return true;
            }
        }
        IOObjectRelease(child);
    }

    os_log_error(sLog, "FindInterface: no usable interface found (%d children checked)", childCount);
    IOObjectRelease(childIter);
    return false;
}

bool RolandUSBDevice::OpenUSBInterface(io_service_t intfService)
{
    IOCFPlugInInterface **plugIn = nullptr;
    SInt32 score = 0;

    kern_return_t kr = IOCreatePlugInInterfaceForService(
        intfService, kIOUSBInterfaceUserClientTypeID,
        kIOCFPlugInInterfaceID, &plugIn, &score);
    IOObjectRelease(intfService);

    if (kr != kIOReturnSuccess || !plugIn) {
        os_log_error(sLog, "OpenUSBInterface: IOCreatePlugIn failed (0x%x)", kr);
        return false;
    }

    IOUSBInterfaceInterface650 **intf = nullptr;
    HRESULT hr = (*plugIn)->QueryInterface(
        plugIn,
        CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID650),
        (LPVOID *)&intf);
    (*plugIn)->Release(plugIn);

    if (hr != S_OK || !intf) {
        os_log_error(sLog, "OpenUSBInterface: QI for InterfaceInterface650 failed");
        return false;
    }

    kr = (*intf)->USBInterfaceOpen(intf);
    if (kr == kIOReturnSuccess) {
        interfaceIntf = intf;
        return true;
    }

    os_log_error(sLog, "OpenUSBInterface: USBInterfaceOpen failed (0x%x)", kr);
    (*intf)->Release(intf);
    return false;
}

bool RolandUSBDevice::FindPipes()
{
    if (!interfaceIntf) return false;

    UInt8 numEP = 0;
    (*interfaceIntf)->GetNumEndpoints(interfaceIntf, &numEP);

    for (UInt8 i = 1; i <= numEP; i++) {
        UInt8 dir, num, xferType, interval;
        UInt16 maxPkt;

        kern_return_t kr = (*interfaceIntf)->GetPipeProperties(
            interfaceIntf, i, &dir, &num, &xferType, &maxPkt, &interval);
        if (kr != kIOReturnSuccess) continue;

        // Accept Bulk or Interrupt transfers (Roland uses both across alt settings)
        if (xferType == kUSBBulk || xferType == kUSBInterrupt) {
            if (dir == kUSBIn && bulkInPipeRef == 0)
                bulkInPipeRef = i;
            else if (dir == kUSBOut && bulkOutPipeRef == 0)
                bulkOutPipeRef = i;
        }
    }

    return (bulkInPipeRef != 0 && bulkOutPipeRef != 0);
}

bool RolandUSBDevice::StartIO(CFRunLoopRef runLoop)
{
    if (ioRunning || !interfaceIntf) return false;

    kern_return_t kr = (*interfaceIntf)->CreateInterfaceAsyncEventSource(
        interfaceIntf, &asyncSource);
    if (kr != kIOReturnSuccess) {
        os_log_error(sLog, "StartIO: CreateAsyncEventSource failed for %{public}s", deviceInfo->name);
        return false;
    }

    CFRunLoopAddSource(runLoop, asyncSource, kCFRunLoopDefaultMode);
    ioRunning = true;
    SubmitRead();

    os_log(sLog, "StartIO: I/O started for %{public}s", deviceInfo->name);
    return true;
}

void RolandUSBDevice::StopIO()
{
    if (!ioRunning) return;
    ioRunning = false;

    if (interfaceIntf && bulkInPipeRef)
        (*interfaceIntf)->AbortPipe(interfaceIntf, bulkInPipeRef);

    if (asyncSource) {
        CFRunLoopSourceInvalidate(asyncSource);
        CFRelease(asyncSource);
        asyncSource = nullptr;
    }

    os_log(sLog, "StopIO: I/O stopped for %{public}s", deviceInfo->name);
}

void RolandUSBDevice::SubmitRead()
{
    if (!ioRunning || !interfaceIntf || !bulkInPipeRef) return;

    kern_return_t kr = (*interfaceIntf)->ReadPipeAsync(
        interfaceIntf, bulkInPipeRef,
        rxBuffer, sizeof(rxBuffer),
        ReadCallback, this);

    if (kr != kIOReturnSuccess) {
        os_log_error(sLog, "SubmitRead: ReadPipeAsync failed for %{public}s (0x%x)", deviceInfo->name, kr);
    }
}

void RolandUSBDevice::ReadCallback(void *refCon, IOReturn result, void *arg0)
{
    auto *self = static_cast<RolandUSBDevice *>(refCon);
    if (!self || !self->ioRunning) return;

    if (result == kIOReturnSuccess) {
        UInt32 bytesRead = (UInt32)(uintptr_t)arg0;

        if (bytesRead > 0 && self->driverRef) {
            // Parse USB-MIDI bulk IN and route by cable number to correct source
            USBMIDIParseBulkIn(self->rxBuffer, bytesRead,
                [](uint8_t cable, const uint8_t *midiBytes,
                   uint8_t byteCount, void *ctx) {
                    auto *dev = static_cast<RolandUSBDevice *>(ctx);

                    // Find port matching this cable number
                    MIDIEndpointRef source = 0;
                    for (uint8_t p = 0; p < dev->deviceInfo->numPorts; p++) {
                        if (dev->deviceInfo->ports[p].cable == cable) {
                            source = dev->midiSources[p];
                            break;
                        }
                    }
                    if (!source) return;

                    Byte pktBuf[256];
                    auto *pktList = reinterpret_cast<MIDIPacketList *>(pktBuf);
                    MIDIPacket *pkt = MIDIPacketListInit(pktList);
                    pkt = MIDIPacketListAdd(pktList, sizeof(pktBuf), pkt,
                                            mach_absolute_time(),
                                            byteCount, midiBytes);
                    if (pkt)
                        MIDIReceived(source, pktList);
                }, self);
        }
    } else if (result != kIOReturnAborted) {
        os_log_error(sLog, "ReadCallback: error for %{public}s (0x%x)", self->deviceInfo->name, result);
    }

    // Resubmit read unless stopped or aborted
    if (self->ioRunning && result != kIOReturnAborted)
        self->SubmitRead();
}

bool RolandUSBDevice::SendMIDI(uint8_t cable, const uint8_t *data, uint32_t length)
{
    if (!interfaceIntf || !bulkOutPipeRef || !data || length == 0)
        return false;

    // Delegate large SysEx to throttled sender
    if (data[0] == 0xF0 && length > kSysExChunkSize)
        return SendSysExThrottled(cable, data, length);

    uint8_t usbBuf[512];
    uint32_t usbLen = USBMIDIBuildBulkOut(data, length, cable,
                                           usbBuf, sizeof(usbBuf));
    if (usbLen == 0) return false;

    kern_return_t kr = (*interfaceIntf)->WritePipe(
        interfaceIntf, bulkOutPipeRef, usbBuf, usbLen);

    return (kr == kIOReturnSuccess);
}

bool RolandUSBDevice::SendSysExThrottled(uint8_t cable, const uint8_t *data, uint32_t length)
{
    // Build USB-MIDI packets directly and send in chunks with delay.
    // Each USB-MIDI packet is 4 bytes: [cable<<4|CIN, b0, b1, b2]
    // Max USB transfer per chunk: 512 bytes (128 USB-MIDI packets = 384 MIDI bytes max)
    static constexpr uint32_t kUSBBufSize = 512;
    uint8_t usbBuf[kUSBBufSize];
    uint32_t usbOffset = 0;
    uint32_t midiBytesSinceFlush = 0;
    uint8_t headerBase = (cable << 4);

    uint32_t i = 0;
    while (i < length) {
        // Collect up to 3 bytes for one USB-MIDI packet
        uint8_t group[3] = {0, 0, 0};
        uint8_t count = 0;
        bool endFound = false;

        while (count < 3 && i < length) {
            group[count] = data[i];
            count++;
            if (data[i] == 0xF7) {
                endFound = true;
                i++;
                break;
            }
            i++;
        }

        // Determine CIN
        uint8_t cin;
        if (endFound) {
            switch (count) {
                case 1: cin = kCIN_SysExEnd1Byte; break;
                case 2: cin = kCIN_SysExEnd2Byte; break;
                case 3: cin = kCIN_SysExEnd3Byte; break;
                default: cin = kCIN_SysExEnd1Byte; break;
            }
        } else {
            cin = kCIN_SysExStart; // SysEx start or continue (CIN=4)
        }

        usbBuf[usbOffset + 0] = headerBase | cin;
        usbBuf[usbOffset + 1] = group[0];
        usbBuf[usbOffset + 2] = group[1];
        usbBuf[usbOffset + 3] = group[2];
        usbOffset += 4;
        midiBytesSinceFlush += count;

        // Flush when we've accumulated enough MIDI bytes or buffer is full
        bool shouldFlush = (midiBytesSinceFlush >= kSysExChunkSize) ||
                           (usbOffset + 4 > kUSBBufSize) ||
                           endFound ||
                           (i >= length);

        if (shouldFlush && usbOffset > 0) {
            kern_return_t kr = (*interfaceIntf)->WritePipe(
                interfaceIntf, bulkOutPipeRef, usbBuf, usbOffset);
            if (kr != kIOReturnSuccess) {
                os_log_error(sLog, "SendSysExThrottled: WritePipe failed for %{public}s (0x%x)",
                             deviceInfo->name, kr);
                return false;
            }
            usbOffset = 0;
            midiBytesSinceFlush = 0;

            // Delay between chunks, but not after the last one
            if (!endFound && i < length)
                usleep(kSysExChunkDelay);
        }
    }

    return true;
}
