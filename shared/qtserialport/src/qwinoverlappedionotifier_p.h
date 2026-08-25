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

#include <QtCore/private/qglobal_p.h>
#include <qobject.h>
#include <qdeadlinetimer.h>

typedef struct _OVERLAPPED OVERLAPPED;

QT_BEGIN_NAMESPACE

class QWinOverlappedIoNotifierPrivate;

class QWinOverlappedIoNotifier : public QObject
{
    Q_OBJECT
    Q_DISABLE_COPY(QWinOverlappedIoNotifier)
    Q_DECLARE_PRIVATE(QWinOverlappedIoNotifier)
    Q_PRIVATE_SLOT(d_func(), void _q_notified())
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
};

QT_END_NAMESPACE

#endif // QWINOVERLAPPEDIONOTIFIER_P_H
