// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only
// Qt-Security score:significant reason:default

#include "qwinoverlappedionotifier_p.h"
#include <qdebug.h>
#include <qatomic.h>
#include <qelapsedtimer.h>
#include <qmutex.h>
#include <qpointer.h>
#include <qqueue.h>
#include <qset.h>
#include <qthread.h>
#include <qt_windows.h>
#include <private/qobject_p.h>
#include <private/qiodevice_p.h>

QT_BEGIN_NAMESPACE

/*!
    \class QWinOverlappedIoNotifier
    \inmodule QtCore
    \brief The QWinOverlappedIoNotifier class provides support for overlapped I/O notifications on Windows.
    \since 5.0
    \internal

    The QWinOverlappedIoNotifier class makes it possible to use efficient
    overlapped (asynchronous) I/O notifications on Windows by using an
    I/O completion port.

    Once you have obtained a file handle, you can use setHandle() to get
    notifications for I/O operations. Whenever an I/O operation completes,
    the notified() signal is emitted which will pass the number of transferred
    bytes, the operation's error code and a pointer to the operation's
    OVERLAPPED object to the receiver.

    Every handle that supports overlapped I/O can be used by
    QWinOverlappedIoNotifier. That includes file handles, TCP sockets
    and named pipes.

    Note that you must not use ReadFileEx() and WriteFileEx() together
    with QWinOverlappedIoNotifier. They are not supported as they use a
    different I/O notification mechanism.

    The hEvent member in the OVERLAPPED structure passed to ReadFile()
    or WriteFile() is ignored and can be used for other purposes.

    \warning This class is only available on Windows.

    Due to peculiarities of the Windows I/O completion port API, users of
    QWinOverlappedIoNotifier must pay attention to the following restrictions:
    \list
    \li File handles with a QWinOverlappedIoNotifer are assigned to an I/O
        completion port until the handle is closed. It is impossible to
        disassociate the file handle from the I/O completion port.
    \li There can be only one QWinOverlappedIoNotifer per file handle. Creating
        another QWinOverlappedIoNotifier for that file, even with a duplicated
        handle, will fail.
    \li Certain Windows API functions are unavailable for file handles that are
        assigned to an I/O completion port. This includes the functions
        \c{ReadFileEx} and \c{WriteFileEx}.
    \endlist
    See also the remarks in the MSDN documentation for the
    \c{CreateIoCompletionPort} function.
*/

struct IOResult
{
    IOResult(DWORD n = 0, DWORD e = 0, OVERLAPPED *p = nullptr)
        : numberOfBytes(n), errorCode(e), overlapped(p)
    {}

    DWORD numberOfBytes = 0;
    DWORD errorCode = 0;
    OVERLAPPED *overlapped = nullptr;
};


class QWinIoCompletionPort;

class QWinOverlappedIoNotifierPrivate : public QObjectPrivate
{
    Q_DECLARE_PUBLIC(QWinOverlappedIoNotifier)
public:
    OVERLAPPED *waitForAnyNotified(QDeadlineTimer deadline);
    void notify(DWORD numberOfBytes, DWORD errorCode, OVERLAPPED *overlapped);
    void _q_notified();
    OVERLAPPED *dispatchNextIoResult();

    static QWinIoCompletionPort *iocp;
    static HANDLE iocpInstanceLock;
    static unsigned int iocpInstanceRefCount;
    HANDLE hHandle = INVALID_HANDLE_VALUE;
    HANDLE hSemaphore = nullptr;
    HANDLE hResultsMutex = nullptr;
    QAtomicInt waiting;
    QAtomicInt signalSent;
    QQueue<IOResult> results;
};

QWinIoCompletionPort *QWinOverlappedIoNotifierPrivate::iocp = 0;
HANDLE QWinOverlappedIoNotifierPrivate::iocpInstanceLock = CreateMutex(NULL, FALSE, NULL);
unsigned int QWinOverlappedIoNotifierPrivate::iocpInstanceRefCount = 0;


class QWinIoCompletionPort : protected QThread
{
public:
    QWinIoCompletionPort()
        : finishThreadKey(reinterpret_cast<ULONG_PTR>(this)),
          drainQueueKey(reinterpret_cast<ULONG_PTR>(this + 1))
    {
        setObjectName(QLatin1String("I/O completion port thread"));
        HANDLE hIOCP = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
        if (!hIOCP) {
            qErrnoWarning("CreateIoCompletionPort failed.");
            return;
        }
        hPort = hIOCP;
        hQueueDrainedEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
        if (!hQueueDrainedEvent) {
            qErrnoWarning("CreateEvent failed.");
            return;
        }
    }

    ~QWinIoCompletionPort()
    {
        PostQueuedCompletionStatus(hPort, 0, finishThreadKey, NULL);
        QThread::wait();
        CloseHandle(hPort);
        CloseHandle(hQueueDrainedEvent);
    }

    void registerNotifier(QWinOverlappedIoNotifierPrivate *notifier)
    {
        const HANDLE hHandle = notifier->hHandle;
        HANDLE hIOCP = CreateIoCompletionPort(hHandle, hPort,
                                              reinterpret_cast<ULONG_PTR>(notifier), 0);
        if (!hIOCP) {
            qErrnoWarning("Can't associate file handle %x with I/O completion port.", hHandle);
            return;
        }
        mutex.lock();
        notifiers += notifier;
        mutex.unlock();
        if (!QThread::isRunning())
            QThread::start();
    }

    void unregisterNotifier(QWinOverlappedIoNotifierPrivate *notifier)
    {
        mutex.lock();
        notifiers.remove(notifier);
        mutex.unlock();
    }

    void drainQueue()
    {
        QMutexLocker locker(&drainQueueMutex);
        ResetEvent(hQueueDrainedEvent);
        PostQueuedCompletionStatus(hPort, 0, drainQueueKey, NULL);
        WaitForSingleObject(hQueueDrainedEvent, INFINITE);
    }

    using QThread::isRunning;

protected:
    void run() override
    {
        DWORD dwBytesRead = 0;
        ULONG_PTR pulCompletionKey = 0;
        OVERLAPPED *overlapped = nullptr;
        DWORD msecs = INFINITE;

        forever {
            BOOL success = GetQueuedCompletionStatus(hPort,
                                                     &dwBytesRead,
                                                     &pulCompletionKey,
                                                     &overlapped,
                                                     msecs);

            DWORD errorCode = success ? ERROR_SUCCESS : GetLastError();
            if (!success && !overlapped) {
                if (!msecs) {
                    // Time out in drain mode. The completion status queue is empty.
                    msecs = INFINITE;
                    SetEvent(hQueueDrainedEvent);
                    continue;
                }
                qErrnoWarning(errorCode, "GetQueuedCompletionStatus failed.");
                return;
            }

            if (pulCompletionKey == finishThreadKey)
                return;
            if (pulCompletionKey == drainQueueKey) {
                // Enter drain mode.
                Q_ASSERT(msecs == INFINITE);
                msecs = 0;
                continue;
            }

            QWinOverlappedIoNotifierPrivate *notifier
                    = reinterpret_cast<QWinOverlappedIoNotifierPrivate *>(pulCompletionKey);
            mutex.lock();
            if (notifiers.contains(notifier))
                notifier->notify(dwBytesRead, errorCode, overlapped);
            mutex.unlock();
        }
    }

private:
    const ULONG_PTR finishThreadKey;
    const ULONG_PTR drainQueueKey;
    HANDLE hPort = INVALID_HANDLE_VALUE;
    QSet<QWinOverlappedIoNotifierPrivate *> notifiers;
    QMutex mutex;
    QMutex drainQueueMutex;
    HANDLE hQueueDrainedEvent = nullptr;
};


QWinOverlappedIoNotifier::QWinOverlappedIoNotifier(QObject *parent)
    : QObject(*new QWinOverlappedIoNotifierPrivate, parent)
{
    Q_D(QWinOverlappedIoNotifier);
    WaitForSingleObject(d->iocpInstanceLock, INFINITE);
    if (!d->iocp)
        d->iocp = new QWinIoCompletionPort;
    d->iocpInstanceRefCount++;
    ReleaseMutex(d->iocpInstanceLock);

    d->hSemaphore = CreateSemaphore(NULL, 0, 255, NULL);
    d->hResultsMutex = CreateMutex(NULL, FALSE, NULL);
    connect(this, SIGNAL(_q_notify()), this, SLOT(_q_notified()), Qt::QueuedConnection);
}

QWinOverlappedIoNotifier::~QWinOverlappedIoNotifier()
{
    Q_D(QWinOverlappedIoNotifier);
    setEnabled(false);
    CloseHandle(d->hResultsMutex);
    CloseHandle(d->hSemaphore);

    WaitForSingleObject(d->iocpInstanceLock, INFINITE);
    if (!--d->iocpInstanceRefCount) {
        delete d->iocp;
        d->iocp = 0;
    }
    ReleaseMutex(d->iocpInstanceLock);
}

void QWinOverlappedIoNotifier::setHandle(Qt::HANDLE h)
{
    Q_D(QWinOverlappedIoNotifier);
    d->hHandle = h;
}

Qt::HANDLE QWinOverlappedIoNotifier::handle() const
{
    Q_D(const QWinOverlappedIoNotifier);
    return d->hHandle;
}

void QWinOverlappedIoNotifier::setEnabled(bool enabled)
{
    Q_D(QWinOverlappedIoNotifier);
    if (enabled)
        d->iocp->registerNotifier(d);
    else
        d->iocp->unregisterNotifier(d);
}

OVERLAPPED *QWinOverlappedIoNotifierPrivate::waitForAnyNotified(QDeadlineTimer deadline)
{
    if (!iocp->isRunning()) {
        qWarning("Called QWinOverlappedIoNotifier::waitForAnyNotified on inactive notifier.");
        return 0;
    }

    qint64 msecs = deadline.remainingTime();
    if (msecs == 0)
        iocp->drainQueue();
    if (msecs == -1)
        msecs = INFINITE;

    const DWORD wfso = WaitForSingleObject(hSemaphore, DWORD(msecs));
    switch (wfso) {
    case WAIT_OBJECT_0:
        return dispatchNextIoResult();
    case WAIT_TIMEOUT:
        return 0;
    default:
        qErrnoWarning("QWinOverlappedIoNotifier::waitForAnyNotified: WaitForSingleObject failed.");
        return 0;
    }
}

class QScopedAtomicIntIncrementor
{
public:
    QScopedAtomicIntIncrementor(QAtomicInt &i)
        : m_int(i)
    {
        ++m_int;
    }

    ~QScopedAtomicIntIncrementor()
    {
        --m_int;
    }

private:
    QAtomicInt &m_int;
};

/*!
 * \internal
 * Wait synchronously for any notified signal.
 *
 * The function returns a pointer to the OVERLAPPED object corresponding to the completed I/O
 * operation. In case no I/O operation was completed during the \a msec timeout, this function
 * returns a null pointer.
 */
OVERLAPPED *QWinOverlappedIoNotifier::waitForAnyNotified(QDeadlineTimer deadline)
{
    Q_D(QWinOverlappedIoNotifier);
    QScopedAtomicIntIncrementor saii(d->waiting);
    OVERLAPPED *result = d->waitForAnyNotified(deadline);
    return result;
}

/*!
 * \internal
 * Wait synchronously for the notified signal.
 *
 * The function returns true if the notified signal was emitted for
 * the I/O operation that corresponds to the OVERLAPPED object.
 */
bool QWinOverlappedIoNotifier::waitForNotified(QDeadlineTimer deadline, OVERLAPPED *overlapped)
{
    Q_D(QWinOverlappedIoNotifier);
    QScopedAtomicIntIncrementor saii(d->waiting);
    while (!deadline.hasExpired()) {
        OVERLAPPED *triggeredOverlapped = waitForAnyNotified(deadline);
        if (!triggeredOverlapped)
            return false;
        if (triggeredOverlapped == overlapped)
            return true;
    }
    return false;
}

/*
 * Note: This function runs in the I/O completion port thread.
 */
void QWinOverlappedIoNotifierPrivate::notify(DWORD numberOfBytes, DWORD errorCode,
        OVERLAPPED *overlapped)
{
    Q_Q(QWinOverlappedIoNotifier);
    WaitForSingleObject(hResultsMutex, INFINITE);
    results.enqueue(IOResult(numberOfBytes, errorCode, overlapped));
    ReleaseSemaphore(hSemaphore, 1, NULL);
    ReleaseMutex(hResultsMutex);
    // Do not send a signal if we didn't process the previous one.
    // This is done to prevent soft memory leaks when working in a completely
    // synchronous way.
    if (!waiting && !signalSent.loadAcquire()) {
        signalSent.storeRelease(1);
        emit q->_q_notify();
    }
}

void QWinOverlappedIoNotifierPrivate::_q_notified()
{
    Q_Q(QWinOverlappedIoNotifier);
    signalSent.storeRelease(0); // signal processed - ready for a new one
    if (WaitForSingleObject(hSemaphore, 0) == WAIT_OBJECT_0) {
        // As we do not queue signals anymore, we need to process the whole
        // queue at once.
        WaitForSingleObject(hResultsMutex, INFINITE);
        QQueue<IOResult> values;
        results.swap(values);
        // Decreasing the semaphore count to keep it in sync with the number
        // of messages in queue. As ReleaseSemaphore does not accept negative
        // values, this is sort of a recommended way to go:
        // https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-releasesemaphore#remarks
        int numToDecrease = values.size() - 1;
        while (numToDecrease > 0) {
            WaitForSingleObject(hSemaphore, 0);
            --numToDecrease;
        }
        ReleaseMutex(hResultsMutex);
        // 'q' can go out of scope if the user decides to close the serial port
        // while processing some answer. So we need to guard against that.
        QPointer<QWinOverlappedIoNotifier> qptr(q);
        while (!values.empty() && qptr) {
            IOResult ioresult = values.dequeue();
            emit qptr->notified(ioresult.numberOfBytes, ioresult.errorCode, ioresult.overlapped);
        }
    }
}

OVERLAPPED *QWinOverlappedIoNotifierPrivate::dispatchNextIoResult()
{
    Q_Q(QWinOverlappedIoNotifier);
    WaitForSingleObject(hResultsMutex, INFINITE);
    IOResult ioresult = results.dequeue();
    ReleaseMutex(hResultsMutex);
    emit q->notified(ioresult.numberOfBytes, ioresult.errorCode, ioresult.overlapped);
    return ioresult.overlapped;
}

QT_END_NAMESPACE

#include "moc_qwinoverlappedionotifier_p.cpp"
