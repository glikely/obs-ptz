// Copyright (C) 2012 Denis Shienkov <denis.shienkov@gmail.com>
// Copyright (C) 2013 Laszlo Papp <lpapp@kde.org>
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only
// Qt-Security score:significant reason:default

#ifndef QSERIALPORT_H
#define QSERIALPORT_H

#include <QtCore/qiodevice.h>

// Quoted, not <QtSerialPort/qserialportglobal.h> - the angle-bracket form
// resolves against whatever's on the include path, and the prebuilt Qt6
// SDK this project vendors from (see ../README.md) ships its own real
// QtSerialPort module headers alongside the ones actually used here,
// under the same relative path. Angle brackets would silently pick up
// that real Q_SERIALPORT_EXPORT (a dllimport on Windows) instead of this
// file's own (empty - this is a static lib), which is a hard MSVC error
// (declaration/definition dllimport mismatch) and a silent, latent bug
// everywhere else. Quoting pins it to this directory's own file.
#include "qserialportglobal.h"

#include <memory>

QT_BEGIN_NAMESPACE

class QSerialPortInfo;
class QSerialPortPrivate;

class Q_SERIALPORT_EXPORT QSerialPort : public QIODevice
{
    Q_OBJECT

    // Was Q_DECLARE_PRIVATE(QSerialPort) - that macro's generated
    // d_func() reinterpret_casts QObject's own d_ptr, which for a class
    // built through QIODevice's public constructor (see qserialport.cpp)
    // holds Qt's own real QIODevicePrivate, not a QSerialPortPrivate at
    // all. d_func() below is hand-written instead, backed by the `d`
    // member declared in the private: section further down (referencing
    // it here, ahead of its declaration, is fine - an inline member
    // function body is parsed as if it appeared after the complete class)
    // - same name/signature as the macro-generated version, so every
    // Q_D(QSerialPort) call site elsewhere needed no changes. See
    // ../README.md.
    inline QSerialPortPrivate *d_func() { return d.get(); }
    inline const QSerialPortPrivate *d_func() const { return d.get(); }
    friend class QSerialPortPrivate;

    Q_PROPERTY(qint32 baudRate READ baudRate WRITE setBaudRate NOTIFY baudRateChanged)
    Q_PROPERTY(DataBits dataBits READ dataBits WRITE setDataBits NOTIFY dataBitsChanged)
    Q_PROPERTY(Parity parity READ parity WRITE setParity NOTIFY parityChanged)
    Q_PROPERTY(StopBits stopBits READ stopBits WRITE setStopBits NOTIFY stopBitsChanged)
    Q_PROPERTY(FlowControl flowControl READ flowControl WRITE setFlowControl NOTIFY flowControlChanged)
    Q_PROPERTY(bool dataTerminalReady READ isDataTerminalReady WRITE setDataTerminalReady
                NOTIFY dataTerminalReadyChanged)
    Q_PROPERTY(bool requestToSend READ isRequestToSend WRITE setRequestToSend NOTIFY requestToSendChanged)
    Q_PROPERTY(SerialPortError error READ error RESET clearError NOTIFY errorOccurred)
    Q_PROPERTY(bool breakEnabled READ isBreakEnabled WRITE setBreakEnabled NOTIFY breakEnabledChanged)
    Q_PROPERTY(bool settingsRestoredOnClose READ settingsRestoredOnClose
                WRITE setSettingsRestoredOnClose NOTIFY settingsRestoredOnCloseChanged
                REVISION(6, 9))

#if defined(Q_OS_WIN32)
    typedef void* Handle;
#else
    typedef int Handle;
#endif

public:

    enum Direction  {
        Input = 1,
        Output = 2,
        AllDirections = Input | Output
    };
    Q_FLAG(Direction)
    Q_DECLARE_FLAGS(Directions, Direction)

    enum BaudRate {
        Baud1200 = 1200,
        Baud2400 = 2400,
        Baud4800 = 4800,
        Baud9600 = 9600,
        Baud19200 = 19200,
        Baud38400 = 38400,
        Baud57600 = 57600,
        Baud115200 = 115200
    };
    Q_ENUM(BaudRate)

    enum DataBits {
        Data5 = 5,
        Data6 = 6,
        Data7 = 7,
        Data8 = 8
    };
    Q_ENUM(DataBits)

    enum Parity {
        NoParity = 0,
        EvenParity = 2,
        OddParity = 3,
        SpaceParity = 4,
        MarkParity = 5
    };
    Q_ENUM(Parity)

    enum StopBits {
        OneStop = 1,
        OneAndHalfStop = 3,
        TwoStop = 2
    };
    Q_ENUM(StopBits)

    enum FlowControl {
        NoFlowControl,
        HardwareControl,
        SoftwareControl
    };
    Q_ENUM(FlowControl)

    enum PinoutSignal {
        NoSignal = 0x00,
        DataTerminalReadySignal = 0x04,
        DataCarrierDetectSignal = 0x08,
        DataSetReadySignal = 0x10,
        RingIndicatorSignal = 0x20,
        RequestToSendSignal = 0x40,
        ClearToSendSignal = 0x80,
        SecondaryTransmittedDataSignal = 0x100,
        SecondaryReceivedDataSignal = 0x200
    };
    Q_FLAG(PinoutSignal)
    Q_DECLARE_FLAGS(PinoutSignals, PinoutSignal)

    enum SerialPortError {
        NoError,
        DeviceNotFoundError,
        PermissionError,
        OpenError,
        WriteError,
        ReadError,
        ResourceError,
        UnsupportedOperationError,
        UnknownError,
        TimeoutError,
        NotOpenError
    };
    Q_ENUM(SerialPortError)

    explicit QSerialPort(QObject *parent = nullptr);
    explicit QSerialPort(const QString &name, QObject *parent = nullptr);
    explicit QSerialPort(const QSerialPortInfo &info, QObject *parent = nullptr);
    virtual ~QSerialPort();

    void setPortName(const QString &name);
    QString portName() const;

    void setPort(const QSerialPortInfo &info);

    bool open(OpenMode mode) override;
    void close() override;

    bool setBaudRate(qint32 baudRate, Directions directions = AllDirections);
    qint32 baudRate(Directions directions = AllDirections) const;

    bool setDataBits(DataBits dataBits);
    DataBits dataBits() const;

    bool setParity(Parity parity);
    Parity parity() const;

    bool setStopBits(StopBits stopBits);
    StopBits stopBits() const;

    bool setFlowControl(FlowControl flowControl);
    FlowControl flowControl() const;

    bool setDataTerminalReady(bool set);
    bool isDataTerminalReady();

    bool setRequestToSend(bool set);
    bool isRequestToSend();

    PinoutSignals pinoutSignals();

    bool flush();
    bool clear(Directions directions = AllDirections);

    SerialPortError error() const;
    void clearError();

    qint64 readBufferSize() const;
    void setReadBufferSize(qint64 size);

    qint64 writeBufferSize() const;
    void setWriteBufferSize(qint64 size);

    bool isSequential() const override;

    qint64 bytesAvailable() const override;
    qint64 bytesToWrite() const override;
    bool canReadLine() const override;

    bool waitForReadyRead(int msecs = 30000) override;
    bool waitForBytesWritten(int msecs = 30000) override;

    bool setBreakEnabled(bool set = true);
    bool isBreakEnabled() const;

    bool settingsRestoredOnClose() const;
    void setSettingsRestoredOnClose(bool restore);

    Handle handle() const;

Q_SIGNALS:
    void baudRateChanged(qint32 baudRate, QSerialPort::Directions directions);
    void dataBitsChanged(QSerialPort::DataBits dataBits);
    void parityChanged(QSerialPort::Parity parity);
    void stopBitsChanged(QSerialPort::StopBits stopBits);
    void flowControlChanged(QSerialPort::FlowControl flowControl);
    void dataTerminalReadyChanged(bool set);
    void requestToSendChanged(bool set);
    void errorOccurred(QSerialPort::SerialPortError error);
    void breakEnabledChanged(bool set);
    void settingsRestoredOnCloseChanged(bool restore);

protected:
    qint64 readData(char *data, qint64 maxSize) override;
    qint64 readLineData(char *data, qint64 maxSize) override;
    qint64 writeData(const char *data, qint64 maxSize) override;

private:
    Q_DISABLE_COPY(QSerialPort)

    // Was, on Windows:
    //   Q_PRIVATE_SLOT(d_func(), bool _q_startAsyncWrite())
    //   Q_PRIVATE_SLOT(d_func(), void _q_notified(quint32, quint32, OVERLAPPED*))
    // Those existed so QObjectPrivate::connect() could target them as
    // slots on a QObjectPrivate-derived Private instance. This class no
    // longer derives that way (see qserialport_p.h), and
    // qserialport_win.cpp's two connect() call sites now use ordinary
    // lambdas instead, so neither is a real slot anymore - both are just
    // plain member functions on QSerialPortPrivate, called directly or
    // from those lambdas.

    // Was QObject::d_ptr (populated by QIODevice(*new QSerialPortPrivate,
    // parent), the protected constructor this class no longer uses - see
    // qserialport.cpp). An ordinary member instead, which is what
    // d_func() above actually reaches into.
    std::unique_ptr<QSerialPortPrivate> d;
};

Q_DECLARE_OPERATORS_FOR_FLAGS(QSerialPort::Directions)
Q_DECLARE_OPERATORS_FOR_FLAGS(QSerialPort::PinoutSignals)

QT_END_NAMESPACE

#endif // QSERIALPORT_H
