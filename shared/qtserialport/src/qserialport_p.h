// Copyright (C) 2011-2012 Denis Shienkov <denis.shienkov@gmail.com>
// Copyright (C) 2011 Sergey Belyashov <Sergey.Belyashov@gmail.com>
// Copyright (C) 2012 Laszlo Papp <lpapp@kde.org>
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only
// Qt-Security score:significant reason:default

#ifndef QSERIALPORT_P_H
#define QSERIALPORT_P_H

//
//  W A R N I N G
//  -------------
//
// This file is not part of the Qt API.  It exists purely as an
// implementation detail.  This header file may change from version to
// version without notice, or even be removed.
//
// We mean it.
//

#include "qserialport.h"

#include <qdeadlinetimer.h>

// Local replacements for <private/qiodevice_p.h> (just QRingBuffer, the
// type of the buffer/writeBuffer fields below - this class no longer
// derives from QIODevicePrivate itself, see the class declaration below)
// and <private/qproperty_p.h> (not needed at all - see the BINDABLE note
// below). See qtserialport_ringbuffer_compat_p.h.
#include "qtserialport_ringbuffer_compat_p.h"

#include <memory>

#if defined(Q_OS_WIN32)
#  include <qt_windows.h>
#elif defined(Q_OS_UNIX)
#  include <QtCore/qlockfile.h>
#  include <QtCore/qfileinfo.h>
#  include <QtCore/qstringlist.h>
#  include <limits.h>
#  include <termios.h>
#  ifdef Q_OS_ANDROID
struct serial_struct {
    int     type;
    int     line;
    unsigned int    port;
    int     irq;
    int     flags;
    int     xmit_fifo_size;
    int     custom_divisor;
    int     baud_base;
    unsigned short  close_delay;
    char    io_type;
    char    reserved_char[1];
    int     hub6;
    unsigned short  closing_wait;
    unsigned short  closing_wait2;
    unsigned char   *iomem_base;
    unsigned short  iomem_reg_shift;
    unsigned int    port_high;
    unsigned long   iomap_base;
};
#    define ASYNC_SPD_CUST  0x0030
#    define ASYNC_SPD_MASK  0x1030
#    define PORT_UNKNOWN    0
#  elif defined(Q_OS_LINUX)
#    include <linux/serial.h>
#  endif
#else
#  error Unsupported OS
#endif

#ifndef QSERIALPORT_BUFFERSIZE
#define QSERIALPORT_BUFFERSIZE 32768
#endif

QT_BEGIN_NAMESPACE

class QWinOverlappedIoNotifier;
class QTimer;
class QSocketNotifier;

#if defined(Q_OS_UNIX)
QString serialPortLockFilePath(const QString &portName);
#endif

class QSerialPortErrorInfo
{
public:
    QSerialPortErrorInfo(QSerialPort::SerialPortError newErrorCode = QSerialPort::UnknownError,
                                  const QString &newErrorString = QString());
    QSerialPort::SerialPortError errorCode = QSerialPort::UnknownError;
    QString errorString;
};

// Deliberately *not* QIODevicePrivate-derived - that's what pulls in
// QIODevicePrivate's ABI-unstable layout (see ../README.md). d_func()/
// q_func() below are hand-written instead of Q_DECLARE_PUBLIC-generated,
// backed by an ordinary member (QSerialPort::d, see qserialport.h) rather
// than QObject's own d_ptr - but with the same names/signatures, so the
// Q_D(QSerialPort)/Q_Q(QSerialPort) call sites throughout qserialport.cpp/
// qserialport_unix.cpp/qserialport_win.cpp (which just expand to a call
// to d_func()/q_func()) needed no changes at all.
class QSerialPortPrivate
{
public:
    QSerialPortPrivate();

    QSerialPort *q_ptr = nullptr;
    inline QSerialPort *q_func() { return q_ptr; }
    inline const QSerialPort *q_func() const { return q_ptr; }

    bool open(QIODevice::OpenMode mode);
    void close();

    QSerialPort::PinoutSignals pinoutSignals();

    bool setDataTerminalReady(bool set);
    bool setRequestToSend(bool set);

    bool flush();
    bool clear(QSerialPort::Directions directions);

    bool sendBreak(int duration);
    bool setBreakEnabled(bool set);

    bool waitForReadyRead(int msec);
    bool waitForBytesWritten(int msec);

    bool setBaudRate();
    bool setBaudRate(qint32 baudRate, QSerialPort::Directions directions);
    bool setDataBits(QSerialPort::DataBits dataBits);
    bool setParity(QSerialPort::Parity parity);
    bool setStopBits(QSerialPort::StopBits stopBits);
    bool setFlowControl(QSerialPort::FlowControl flowControl);

    QSerialPortErrorInfo getSystemError(int systemErrorCode = -1) const;

    void setError(const QSerialPortErrorInfo &errorInfo);

    qint64 writeData(const char *data, qint64 maxSize);

    bool initialize(QIODevice::OpenMode mode);

    static QList<qint32> standardBaudRates();

    qint64 readBufferMaxSize = 0;
    qint64 writeBufferMaxSize = 0;

    // Upstream's QSerialPortPrivate has no buffer/writeBuffer fields of
    // its own - it uses the ones it inherits from QIODevicePrivate. This
    // class no longer inherits from QIODevicePrivate (see the class
    // declaration above), so these need to be declared explicitly here
    // instead. See qtserialport_ringbuffer_compat_p.h.
    QRingBuffer buffer;
    QRingBuffer writeBuffer;

    // Plain fields instead of Q_OBJECT_COMPAT_PROPERTY_WITH_ARGS-declared
    // QObjectCompatProperty<T> wrappers - those exist to back
    // QBindable<T> support (qproperty_p.h), which needs a real QObject
    // behind Q_OBJECT_BINDABLE_PROPERTY's metaobject wiring to be worth
    // having; nothing in obs-ptz uses Qt's declarative property-binding
    // system, so it's not carried over. Every setter below still updates
    // its field and emits the same *Changed signal by hand, same as
    // upstream's own setBindableX() callbacks did.
    QSerialPort::SerialPortError error = QSerialPort::NoError;

    QString systemLocation;
    qint32 inputBaudRate = QSerialPort::Baud9600;
    qint32 outputBaudRate = QSerialPort::Baud9600;

    QSerialPort::DataBits dataBits = QSerialPort::Data8;
    QSerialPort::Parity parity = QSerialPort::NoParity;
    QSerialPort::StopBits stopBits = QSerialPort::OneStop;
    QSerialPort::FlowControl flowControl = QSerialPort::NoFlowControl;

    bool settingsRestoredOnClose = true;

    bool isBreakEnabled = false;

    bool startAsyncRead();

    bool emittedReadyRead = false;

#if defined(Q_OS_WIN32)

    bool setDcb(DCB *dcb);
    bool getDcb(DCB *dcb);
    OVERLAPPED *waitForNotified(QDeadlineTimer deadline);

    qint64 queuedBytesCount(QSerialPort::Direction direction) const;

    bool completeAsyncCommunication(qint64 bytesTransferred);
    bool completeAsyncRead(qint64 bytesTransferred);
    bool completeAsyncWrite(qint64 bytesTransferred);

    bool startAsyncCommunication();
    bool _q_startAsyncWrite();
    void _q_notified(DWORD numberOfBytes, DWORD errorCode, OVERLAPPED *overlapped);

    void emitReadyRead();

    DCB restoredDcb;
    COMMTIMEOUTS currentCommTimeouts;
    COMMTIMEOUTS restoredCommTimeouts;
    HANDLE handle = INVALID_HANDLE_VALUE;
    QByteArray readChunkBuffer;
    QByteArray writeChunkBuffer;
    bool communicationStarted = false;
    bool writeStarted = false;
    bool readStarted = false;
    QWinOverlappedIoNotifier *notifier = nullptr;
    QTimer *startAsyncWriteTimer = nullptr;
    OVERLAPPED communicationOverlapped;
    OVERLAPPED readCompletionOverlapped;
    OVERLAPPED writeCompletionOverlapped;
    DWORD triggeredEventMask = 0;

#elif defined(Q_OS_UNIX)

    static qint32 settingFromBaudRate(qint32 baudRate);

    bool setTermios(const termios *tio);
    bool getTermios(termios *tio);

    bool setCustomBaudRate(qint32 baudRate, QSerialPort::Directions directions);
    bool setStandardBaudRate(qint32 baudRate, QSerialPort::Directions directions);

    bool isReadNotificationEnabled() const;
    void setReadNotificationEnabled(bool enable);
    bool isWriteNotificationEnabled() const;
    void setWriteNotificationEnabled(bool enable);

    bool waitForReadOrWrite(bool *selectForRead, bool *selectForWrite,
                            bool checkRead, bool checkWrite,
                            int msecs);

    qint64 readFromPort(char *data, qint64 maxSize);
    qint64 writeToPort(const char *data, qint64 maxSize);

#ifndef CMSPAR
    qint64 writePerChar(const char *data, qint64 maxSize);
#endif

    bool readNotification();
    bool startAsyncWrite();
    bool completeAsyncWrite();
    void handleException();

    struct termios restoredTermios;
    int descriptor = -1;

    QSocketNotifier *readNotifier = nullptr;
    QSocketNotifier *writeNotifier = nullptr;
    QSocketNotifier *exceptionNotifier = nullptr;

    bool readPortNotifierCalled = false;
    bool readPortNotifierState = false;
    bool readPortNotifierStateSet = false;

    bool emittedBytesWritten = false;

    qint64 pendingBytesWritten = 0;
    bool writeSequenceStarted = false;

    bool gotException = false;

    std::unique_ptr<QLockFile> lockFileScopedPointer;

#endif
};

QT_END_NAMESPACE

#endif // QSERIALPORT_P_H
