// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only
// Qt-Security score:significant reason:default

#ifndef QWINOVERLAPPEDIONOTIFIER_P_H
#define QWINOVERLAPPEDIONOTIFIER_P_H

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

#include <qobject.h>
#include <qdeadlinetimer.h>

#include <memory>

typedef struct _OVERLAPPED OVERLAPPED;

QT_BEGIN_NAMESPACE

class QWinOverlappedIoNotifierPrivate;

class QWinOverlappedIoNotifier : public QObject
{
    Q_OBJECT
    Q_DISABLE_COPY(QWinOverlappedIoNotifier)

    // Was Q_DECLARE_PRIVATE(QWinOverlappedIoNotifier) - see
    // qserialport.h's own d_func()/d comment for why this is hand-written
    // instead, backed by the `d` member below rather than QObject::d_ptr.
    inline QWinOverlappedIoNotifierPrivate *d_func() { return d.get(); }
    inline const QWinOverlappedIoNotifierPrivate *d_func() const { return d.get(); }
    friend class QWinOverlappedIoNotifierPrivate;

    friend class QWinIoCompletionPort;
public:
    QWinOverlappedIoNotifier(QObject *parent = 0);
    ~QWinOverlappedIoNotifier();

    void setHandle(Qt::HANDLE h);
    Qt::HANDLE handle() const;

    void setEnabled(bool enabled);
    OVERLAPPED *waitForAnyNotified(QDeadlineTimer deadline);
    bool waitForNotified(QDeadlineTimer deadline, OVERLAPPED *overlapped);

Q_SIGNALS:
    void notified(quint32 numberOfBytes, quint32 errorCode, OVERLAPPED *overlapped);
#if !defined(Q_QDOC)
    void _q_notify();
#endif

private:
    // Was QObject::d_ptr (populated by QObject(*new
    // QWinOverlappedIoNotifierPrivate, parent), the protected constructor
    // this class no longer uses - see qwinoverlappedionotifier.cpp). An
    // ordinary member instead, which is what d_func() above actually
    // reaches into.
    std::unique_ptr<QWinOverlappedIoNotifierPrivate> d;
};

QT_END_NAMESPACE

#endif // QWINOVERLAPPEDIONOTIFIER_P_H
