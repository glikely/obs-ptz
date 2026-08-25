// Copyright (C) 2011-2012 Denis Shienkov <denis.shienkov@gmail.com>
// Copyright (C) 2011 Sergey Belyashov <Sergey.Belyashov@gmail.com>
// Copyright (C) 2012 Laszlo Papp <lpapp@kde.org>
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only
// Qt-Security score:critical reason:data-parser

#include "qserialportinfo.h"
#include "qserialportinfo_p.h"
#include "qserialport_p.h"

#include <QtCore/quuid.h>
#include <QtCore/qpair.h>
#include <QtCore/qstringlist.h>
#include <QtCore/private/qwinregistry_p.h>
#include <QtCore/private/quniquehandle_types_p.h>

#include <vector>

#include <initguid.h>
#include <devguid.h> // for GUID_DEVCLASS_PORTS and GUID_DEVCLASS_MODEM
#include <winioctl.h> // for GUID_DEVINTERFACE_COMPORT
#include <setupapi.h>
#include <cfgmgr32.h>
#include <usbioctl.h>
#include <usbiodef.h>
#include <ioapiset.h>

#ifdef QT_NO_REDEFINE_GUID_DEVINTERFACE_MODEM
#  include <ntddmodm.h> // for GUID_DEVINTERFACE_MODEM
#else
  DEFINE_GUID(GUID_DEVINTERFACE_MODEM, 0x2c7089aa, 0x2e0e, 0x11d1, 0xb1, 0x14, 0x00, 0xc0, 0x4f, 0xc2, 0xaa, 0xe4);
#endif

QT_BEGIN_NAMESPACE

namespace {

struct DevInfoHandleTraits
{
    using Type = HDEVINFO;
    static Type invalidValue() noexcept
    {
        return INVALID_HANDLE_VALUE;
    }
    static bool close(Type handle) noexcept
    {
        return SetupDiDestroyDeviceInfoList(handle) == TRUE;
    }
};

using DevInfoHandle = QUniqueHandle<DevInfoHandleTraits>;

} // namespace

static QStringList portNamesFromHardwareDeviceMap()
{
    const QWinRegistryKey key{ HKEY_LOCAL_MACHINE, L"HARDWARE\\DEVICEMAP\\SERIALCOMM", 0,
                               KEY_QUERY_VALUE };

    if (!key.isValid())
        return { };

    QStringList result;
    DWORD index = 0;

    // This is a maximum length of value name, see:
    // https://msdn.microsoft.com/en-us/library/windows/desktop/ms724872%28v=vs.85%29.aspx
    enum { MaximumValueNameInChars = 16383 };

    std::vector<wchar_t> outputValueName(MaximumValueNameInChars, 0);
    std::vector<wchar_t> outputBuffer(MAX_PATH + 1, 0);
    DWORD bytesRequired = MAX_PATH;
    for (;;) {
        DWORD requiredValueNameChars = MaximumValueNameInChars;
        const LONG ret = ::RegEnumValue(key, index, &outputValueName[0], &requiredValueNameChars,
                                        nullptr, nullptr, reinterpret_cast<PBYTE>(&outputBuffer[0]), &bytesRequired);
        if (ret == ERROR_MORE_DATA) {
            outputBuffer.resize(bytesRequired / sizeof(wchar_t) + 2, 0);
        } else if (ret == ERROR_SUCCESS) {
            result.append(QString::fromWCharArray(&outputBuffer[0]));
            ++index;
        } else {
            break;
        }
    }
    return result;
}

static QString deviceRegistryProperty(HDEVINFO deviceInfoSet,
                                      PSP_DEVINFO_DATA deviceInfoData,
                                      DWORD property)
{
    DWORD dataType = 0;
    std::vector<wchar_t> outputBuffer(MAX_PATH + 1, 0);
    DWORD bytesRequired = MAX_PATH;
    for (;;) {
        if (::SetupDiGetDeviceRegistryProperty(deviceInfoSet, deviceInfoData, property, &dataType,
                                               reinterpret_cast<PBYTE>(&outputBuffer[0]),
                                               bytesRequired, &bytesRequired)) {
            break;
        }

        if (::GetLastError() != ERROR_INSUFFICIENT_BUFFER
                || (dataType != REG_SZ && dataType != REG_EXPAND_SZ)) {
            return QString();
        }
        outputBuffer.resize(bytesRequired / sizeof(wchar_t) + 2, 0);
    }
    return QString::fromWCharArray(&outputBuffer[0]);
}

static QString getStringDescriptor(HANDLE hHubDevice, ULONG connectionIndex,
                                   UCHAR descriptorIndex, USHORT languageID)
{

    constexpr DWORD bufferSize = sizeof(USB_DESCRIPTOR_REQUEST) + MAXIMUM_USB_STRING_LENGTH;
    std::array<UCHAR, bufferSize> buffer{};
    auto request = reinterpret_cast<PUSB_DESCRIPTOR_REQUEST>(buffer.data());

    request->ConnectionIndex = connectionIndex;
    request->SetupPacket.wValue = (USB_STRING_DESCRIPTOR_TYPE << 8) | descriptorIndex;
    request->SetupPacket.wIndex = languageID;
    request->SetupPacket.wLength = MAXIMUM_USB_STRING_LENGTH;

    ULONG   bytesReturned = 0;
    BOOL success = DeviceIoControl(hHubDevice, IOCTL_USB_GET_DESCRIPTOR_FROM_NODE_CONNECTION,
                                   request, bufferSize, request, bufferSize, &bytesReturned,
                                   nullptr);

    if (!success)
        return {};

    constexpr ULONG minExpectedSize = sizeof(USB_DESCRIPTOR_REQUEST) +
            sizeof(USB_STRING_DESCRIPTOR);

    if (bytesReturned < minExpectedSize)
        return {};

    const auto stringDesc = reinterpret_cast<PUSB_STRING_DESCRIPTOR>(request->Data);

    if (stringDesc->bDescriptorType != USB_STRING_DESCRIPTOR_TYPE)
        return {};

    if (stringDesc->bLength != bytesReturned - sizeof(USB_DESCRIPTOR_REQUEST))
        return {};

    if (stringDesc->bLength % 2 != 0)
        return {};

    // Offset per USB 2.0 standard, section 9.6.7 table 9-16.
    // bLength appears to include the size of the first 2 bytes of the descriptor
    constexpr int stringOffset = 2;
    const int numWideChars = (stringDesc->bLength - stringOffset) / sizeof(wchar_t);
    return QString::fromWCharArray(stringDesc->bString, numWideChars);
}

static std::vector<wchar_t> getDevicePath(DEVINST devInst)
{
    unsigned long devIdSize;

    if (::CM_Get_Device_ID_Size(&devIdSize, devInst, 0) != CR_SUCCESS) {
        return {};
    }

    std::vector<wchar_t> buffer(devIdSize + 1);

    if (::CM_Get_Device_ID(devInst, buffer.data(), devIdSize, 0) != CR_SUCCESS) {
        return {};
    }

    return buffer;
}

// Returns true if a hub is found, false if we gave up and didn't find one
// Passes devLocation by ref - port number on the hub of the device we need info for.
// Passes buffer by ref - path to the device we need to query to get information
static bool getUSBLocationAndPath(DWORD devInst, int &devLocation, std::vector<wchar_t> &buffer)
{
    bool hubFound = false;
    int count = 0;
    QString devLocationStr;
    std::vector<wchar_t> devicePath = {};


    // In order to find the actual information we want to find in subsequent function calls,
    // We have to get the port number on the hub of the device we want from that device,
    // and we have to get the path to the hub device. Both are required to get
    // any of the usb data we want. So we loop through starting with the passed-in
    // device instance, and we return (via out arguments) the port of the device connected
    // to the hub, and the path to the hub itself.

    while (!hubFound && (count < 3)) {
        DEVINST parentDevInst;
        if (count == 0) {
            parentDevInst = devInst;
        } else {
            if (::CM_Get_Parent(&parentDevInst, devInst, 0) != CR_SUCCESS) {
                return false;
            }
        }

        devicePath = getDevicePath(parentDevInst);

        if (devicePath.empty())
            return false;

        // Get info for this device. If it's a composite device we'll have to go up another level.

        const DevInfoHandle currDeviceInformation{ ::SetupDiGetClassDevsW(
                nullptr, devicePath.data(), nullptr,
                DIGCF_ALLCLASSES | DIGCF_PRESENT | DIGCF_DEVICEINTERFACE) };

        if (!currDeviceInformation)
            return false;

        SP_DEVINFO_DATA currDeviceInfoData = {};
        currDeviceInfoData.cbSize = sizeof(currDeviceInfoData);

        // There should only be one since we're getting info for the parent device
        if (!::SetupDiEnumDeviceInfo(currDeviceInformation.get(), 0, &currDeviceInfoData)) {
            return false;
        }

        // Find out if this device is a hub device, if so, we're done
        const QString devDescription = deviceRegistryProperty(currDeviceInformation.get(),
                                                              &currDeviceInfoData,
                                                              SPDRP_DEVICEDESC);

        if (devDescription.contains(L"Hub")) {
            hubFound = true;
        }  else {
            devInst = parentDevInst;
            devLocationStr = deviceRegistryProperty(currDeviceInformation.get(),
                                                    &currDeviceInfoData,
                                                    SPDRP_LOCATION_INFORMATION);
            count++;
        }
    }

    if (count == 3)
        // didn't find what we were looking for. Give up.
        return false;

    if (!hubFound)
        return false;

    const QString portString(L"Port_#");

    if (devLocationStr.contains(portString)) {
        devLocationStr.remove(portString);
        devLocation = devLocationStr.left(devLocationStr.indexOf(u'.')).toInt();
    } else {
        return false;
    }

    // Build the path based on information found here:
    // https://stackoverflow.com/questions/
    // 8007468/how-do-i-obtain-usb-device-descriptor-given-a-device-path/32641140#32641140
    QString devPath = QString::fromStdWString(devicePath.data());
    QString hubUuidStr = u'#' + QUuid(GUID_DEVINTERFACE_USB_HUB).toString();

    devPath.replace(u'\\', u'#');
    devPath.prepend(L"\\\\?\\");
    devPath.append(hubUuidStr); // GUID for hub devices
    buffer.resize(devPath.size()+1, 0);
    devPath.toWCharArray(buffer.data());

    return true;
}

struct UsbData
{
    QString iManufacturer;
    QString iProduct;
};

static UsbData getUSBDataFromDevice(DWORD devInst)
{
    std::vector<wchar_t> buffer;
    int devLocation;

    if (!getUSBLocationAndPath(devInst, devLocation, buffer))
        return {};

    // Open the Hub device. To get the information we want, we have to open the Hub device,
    // and pass in the port number in the argument structure.

    const QUniqueWin32Handle fileHandle{ ::CreateFile(buffer.data(), GENERIC_READ, FILE_SHARE_READ,
                                                      nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                                                      nullptr) };

    if (!fileHandle)
        return {};

    // Some of the following code was from Microsoft's sample usbview code
    constexpr ULONG size = sizeof(USB_NODE_CONNECTION_INFORMATION);
    std::array<UCHAR, size> connectionInfoData;

    // Set the port number of the device on the hub for the DeviceIoControl call.
    auto connectionInfo =
            reinterpret_cast<PUSB_NODE_CONNECTION_INFORMATION>(connectionInfoData.data());
    connectionInfo->ConnectionIndex = devLocation;

    ULONG dataSize = 0;
    if (!::DeviceIoControl(fileHandle.get(), IOCTL_USB_GET_NODE_CONNECTION_INFORMATION,
                          connectionInfo, size, connectionInfo, size, &dataSize, nullptr)) {
        return {};
    }

    // Get the language information, but we're only going to use the first one since we're going to
    // use this information for the manufacturer returned to Qt's caller, and there's only 1 allowed
    // there

    QString usbStringData = getStringDescriptor(fileHandle.get(),
                                                connectionInfo->ConnectionIndex,
                                                0, 0);

    if (usbStringData.isEmpty())
        return {};

    USHORT languageIDs = usbStringData[0].unicode();
    UsbData usbData;

    if (connectionInfo->DeviceDescriptor.iManufacturer != 0)
        usbData.iManufacturer = getStringDescriptor(fileHandle.get(),
                                                    connectionInfo->ConnectionIndex,
                                                    connectionInfo->DeviceDescriptor.iManufacturer,
                                                    languageIDs);

    if (connectionInfo->DeviceDescriptor.iProduct != 0)
        usbData.iProduct = getStringDescriptor(fileHandle.get(),
                                               connectionInfo->ConnectionIndex,
                                               connectionInfo->DeviceDescriptor.iProduct,
                                               languageIDs);

    return usbData;
}

static QString deviceInstanceIdentifier(DEVINST deviceInstanceNumber)
{
    std::vector<wchar_t> outputBuffer(MAX_DEVICE_ID_LEN + 1, 0);
    if (::CM_Get_Device_ID(
                deviceInstanceNumber,
                &outputBuffer[0],
                MAX_DEVICE_ID_LEN,
                0) != CR_SUCCESS) {
        return QString();
    }
    return QString::fromWCharArray(&outputBuffer[0]).toUpper();
}

static DEVINST parentDeviceInstanceNumber(DEVINST childDeviceInstanceNumber)
{
    ULONG nodeStatus = 0;
    ULONG problemNumber = 0;
    if (::CM_Get_DevNode_Status(&nodeStatus, &problemNumber,
                                childDeviceInstanceNumber, 0) != CR_SUCCESS) {
        return 0;
    }
    DEVINST parentInstanceNumber = 0;
    if (::CM_Get_Parent(&parentInstanceNumber, childDeviceInstanceNumber, 0) != CR_SUCCESS)
        return 0;
    return parentInstanceNumber;
}

static QString devicePortName(HDEVINFO deviceInfoSet, PSP_DEVINFO_DATA deviceInfoData)
{
    const HKEY key = ::SetupDiOpenDevRegKey(deviceInfoSet, deviceInfoData, DICS_FLAG_GLOBAL,
                                            0, DIREG_DEV, KEY_READ);
    if (key == INVALID_HANDLE_VALUE)
        return QString();

    static const wchar_t * const keyTokens[] = {
            L"PortName\0",
            L"PortNumber\0"
    };

    QString portName;
    for (auto keyToken : keyTokens) {
        DWORD dataType = 0;
        std::vector<wchar_t> outputBuffer(MAX_PATH + 1, 0);
        DWORD bytesRequired = MAX_PATH;
        for (;;) {
            const LONG ret = ::RegQueryValueEx(key, keyToken, nullptr, &dataType,
                                               reinterpret_cast<PBYTE>(&outputBuffer[0]), &bytesRequired);
            if (ret == ERROR_MORE_DATA) {
                outputBuffer.resize(bytesRequired / sizeof(wchar_t) + 2, 0);
                continue;
            } else if (ret == ERROR_SUCCESS) {
                if (dataType == REG_SZ)
                    portName = QString::fromWCharArray(&outputBuffer[0]);
                else if (dataType == REG_DWORD)
                    portName = QStringLiteral("COM%1").arg(*(PDWORD(&outputBuffer[0])));
            }
            break;
        }

        if (!portName.isEmpty())
            break;
    }
    ::RegCloseKey(key);
    return portName;
}

static QString deviceDescription(HDEVINFO deviceInfoSet,
                                 PSP_DEVINFO_DATA deviceInfoData)
{
    return deviceRegistryProperty(deviceInfoSet, deviceInfoData, SPDRP_DEVICEDESC);
}

static QString deviceManufacturer(HDEVINFO deviceInfoSet,
                                  PSP_DEVINFO_DATA deviceInfoData)
{
    return deviceRegistryProperty(deviceInfoSet, deviceInfoData, SPDRP_MFG);
}

static quint16 parseDeviceIdentifier(const QString &instanceIdentifier,
                                     const QString &identifierPrefix,
                                     qsizetype identifierSize, bool &ok)
{
    const qsizetype index = instanceIdentifier.indexOf(identifierPrefix);
    if (index == -1)
        return quint16(0);
    return instanceIdentifier.mid(index + identifierPrefix.size(), identifierSize).toInt(&ok, 16);
}

static quint16 deviceVendorIdentifier(const QString &instanceIdentifier, bool &ok)
{
    static constexpr qsizetype vendorIdentifierSize = 4;
    quint16 result = parseDeviceIdentifier(
                instanceIdentifier, QStringLiteral("VID_"), vendorIdentifierSize, ok);
    if (!ok)
        result = parseDeviceIdentifier(
                    instanceIdentifier, QStringLiteral("VEN_"), vendorIdentifierSize, ok);
    return result;
}

static quint16 deviceProductIdentifier(const QString &instanceIdentifier, bool &ok)
{
    static constexpr qsizetype productIdentifierSize = 4;
    quint16 result = parseDeviceIdentifier(
                instanceIdentifier, QStringLiteral("PID_"), productIdentifierSize, ok);
    if (!ok)
        result = parseDeviceIdentifier(
                    instanceIdentifier, QStringLiteral("DEV_"), productIdentifierSize, ok);
    return result;
}

static QString parseDeviceSerialNumber(const QString &instanceIdentifier)
{
    qsizetype firstbound = instanceIdentifier.lastIndexOf(QLatin1Char('\\'));
    qsizetype lastbound = instanceIdentifier.indexOf(QLatin1Char('_'), firstbound);
    if (instanceIdentifier.startsWith(QLatin1String("USB\\"))) {
        if (lastbound != instanceIdentifier.size() - 3)
            lastbound = instanceIdentifier.size();
        qsizetype ampersand = instanceIdentifier.indexOf(QLatin1Char('&'), firstbound);
        if (ampersand != -1 && ampersand < lastbound)
            return QString();
    } else if (instanceIdentifier.startsWith(QLatin1String("FTDIBUS\\"))) {
        firstbound = instanceIdentifier.lastIndexOf(QLatin1Char('+'));
        lastbound = instanceIdentifier.indexOf(QLatin1Char('\\'), firstbound);
        if (lastbound == -1)
            return QString();
    } else {
        return QString();
    }

    return instanceIdentifier.mid(firstbound + 1, lastbound - firstbound - 1);
}

static QString deviceSerialNumber(QString instanceIdentifier,
                                  DEVINST deviceInstanceNumber)
{
    for (;;) {
        const QString result = parseDeviceSerialNumber(instanceIdentifier);
        if (!result.isEmpty())
            return result;
        deviceInstanceNumber = parentDeviceInstanceNumber(deviceInstanceNumber);
        if (deviceInstanceNumber == 0)
            break;
        instanceIdentifier = deviceInstanceIdentifier(deviceInstanceNumber);
        if (instanceIdentifier.isEmpty())
            break;
    }

    return QString();
}

static bool anyOfPorts(const QList<QSerialPortInfo> &ports, const QString &portName)
{
    const auto end = ports.end();
    auto isPortNamesEquals = [&portName](const QSerialPortInfo &portInfo) {
        return portInfo.portName() == portName;
    };
    return std::find_if(ports.begin(), end, isPortNamesEquals) != end;
}

QList<QSerialPortInfo> QSerialPortInfo::availablePorts()
{
    static const struct {
        GUID guid; DWORD flags;
    } setupTokens[] =  {
        { GUID_DEVCLASS_PORTS, DIGCF_PRESENT },
        { GUID_DEVCLASS_MODEM, DIGCF_PRESENT },
        { GUID_DEVINTERFACE_COMPORT, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE },
        { GUID_DEVINTERFACE_MODEM, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE }
    };

    QList<QSerialPortInfo> serialPortInfoList;

    for (const auto& setupToken : setupTokens) {
        const DevInfoHandle deviceInfoSet{ ::SetupDiGetClassDevs(&setupToken.guid, nullptr,
                                                                 nullptr, setupToken.flags) };
        if (!deviceInfoSet)
            return serialPortInfoList;

        SP_DEVINFO_DATA deviceInfoData{};
        deviceInfoData.cbSize = sizeof(deviceInfoData);

        DWORD index = 0;
        while (::SetupDiEnumDeviceInfo(deviceInfoSet.get(), index++, &deviceInfoData)) {
            const QString portName = devicePortName(deviceInfoSet.get(), &deviceInfoData);
            if (portName.isEmpty() || portName.contains(QLatin1String("LPT")))
                continue;

            if (anyOfPorts(serialPortInfoList, portName))
                continue;

            QSerialPortInfoPrivate priv;

            priv.portName = portName;
            priv.device = QSerialPortInfoPrivate::portNameToSystemLocation(portName);
            priv.description = deviceDescription(deviceInfoSet.get(), &deviceInfoData);
            priv.manufacturer = deviceManufacturer(deviceInfoSet.get(), &deviceInfoData);

            const QString instanceIdentifier = deviceInstanceIdentifier(deviceInfoData.DevInst);

            priv.serialNumber =
                    deviceSerialNumber(instanceIdentifier, deviceInfoData.DevInst);
            priv.vendorIdentifier =
                    deviceVendorIdentifier(instanceIdentifier, priv.hasVendorIdentifier);
            priv.productIdentifier =
                    deviceProductIdentifier(instanceIdentifier, priv.hasProductIdentifier);

            // This makes Windows return the same data that linux does for Manufacturer and Product
            if ((instanceIdentifier.startsWith(L"USB\\")
                 || instanceIdentifier.startsWith(L"FTDIBUS\\"))
                && priv.hasVendorIdentifier && priv.hasProductIdentifier) {
                UsbData usbStrings = getUSBDataFromDevice(deviceInfoData.DevInst);

                if (!usbStrings.iManufacturer.isEmpty())
                    priv.manufacturer = usbStrings.iManufacturer;

                if (!usbStrings.iProduct.isEmpty())
                    priv.description = usbStrings.iProduct;
                // End of making Windows return the same data as Linux
            }

            serialPortInfoList.append(priv);
        }
    }

    const auto portNames = portNamesFromHardwareDeviceMap();
    for (const QString &portName : portNames) {
        if (!anyOfPorts(serialPortInfoList, portName)) {
            QSerialPortInfoPrivate priv;
            priv.portName = portName;
            priv.device =  QSerialPortInfoPrivate::portNameToSystemLocation(portName);
            serialPortInfoList.append(priv);
        }
    }

    return serialPortInfoList;
}

QString QSerialPortInfoPrivate::portNameToSystemLocation(const QString &source)
{
    return source.startsWith(QLatin1String("COM"))
            ? (QLatin1String("\\\\.\\") + source) : source;
}

QString QSerialPortInfoPrivate::portNameFromSystemLocation(const QString &source)
{
    return (source.startsWith(QLatin1String("\\\\.\\"))
            || source.startsWith(QLatin1String("//./")))
            ? source.mid(4) : source;
}

QT_END_NAMESPACE
