// Local replacement for Qt's private <private/qcore_unix_p.h> (the
// qt_safe_* EINTR-retry wrappers) and <private/qiodevice_p.h>'s
// qt_subtract_from_timeout(), used unmodified by qserialport_unix.cpp.
//
// Not an ABI concern the way QIODevicePrivate/QObjectPrivate are - these
// are (in Qt's own header) plain inline functions plus one exported
// utility function, nothing here depends on any Qt-internal struct
// layout. They're reimplemented locally instead of included from Qt's
// private headers purely because those headers/symbols aren't installed
// as part of a normal Qt6 SDK (only obs-deps' particular Qt6 build happens
// to ship them under a version-numbered private/ subdirectory) and Qt
// makes no promise they'll still exist, unrenamed, after the next
// buildspec.json Qt6 version bump - exactly the kind of version-coupled
// breakage this fork exists to get away from. Same names/signatures as
// upstream throughout, so qserialport_unix.cpp needed no changes to keep
// calling them.
//
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <QDeadlineTimer>
#include <QEvent>
#include <QElapsedTimer>

#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

static inline int qt_safe_open(const char *pathname, int flags, mode_t mode = 0777)
{
	int fd;
	do {
		fd = ::open(pathname, flags, mode);
	} while (fd == -1 && errno == EINTR);
	if (fd != -1)
		::fcntl(fd, F_SETFD, FD_CLOEXEC);
	return fd;
}

static inline qint64 qt_safe_read(int fd, void *data, qint64 maxlen)
{
	qint64 ret;
	do {
		ret = ::read(fd, data, size_t(maxlen));
	} while (ret == -1 && errno == EINTR);
	return ret;
}

static inline qint64 qt_safe_write(int fd, const void *data, qint64 len)
{
	qint64 ret;
	do {
		ret = ::write(fd, data, size_t(len));
	} while (ret == -1 && errno == EINTR);
	return ret;
}

static inline int qt_safe_close(int fd)
{
	int ret;
	do {
		ret = ::close(fd);
	} while (ret == -1 && errno == EINTR);
	return ret;
}

static inline struct pollfd qt_make_pollfd(int fd, short events)
{
	struct pollfd pfd = {fd, events, 0};
	return pfd;
}

// Upstream's is Q_CORE_EXPORT (compiled into Qt6Core, not header-only) -
// this is a from-scratch reimplementation matching its documented
// behavior (poll(), retried against the deadline on EINTR), not a copy
// of Qt's own implementation.
static inline int qt_safe_poll(struct pollfd *fds, nfds_t nfds, QDeadlineTimer deadline)
{
	QElapsedTimer timer;
	const bool hasTimeout = !deadline.isForever();
	if (hasTimeout)
		timer.start();

	for (;;) {
		qint64 msecs = hasTimeout ? deadline.remainingTime() : -1;
		if (hasTimeout && msecs < 0)
			msecs = 0;
		const int ret = ::poll(fds, nfds, int(msecs));
		if (ret != -1 || errno != EINTR)
			return ret;
	}
}

// Upstream's is also Q_CORE_EXPORT; same reimplementation rationale.
static inline int qt_subtract_from_timeout(int timeout, int elapsed)
{
	if (timeout < 0)
		return -1;
	const int remaining = timeout - elapsed;
	return remaining < 0 ? 0 : remaining;
}
