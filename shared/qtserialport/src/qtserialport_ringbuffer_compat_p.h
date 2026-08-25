// Local replacement for Qt's private <private/qringbuffer_p.h>. Real
// QIODevicePrivate::buffer/writeBuffer are QRingBuffer (a chunked ring
// buffer, for allocation efficiency) - this fork has no access to those
// fields at all (see qserialport_p.h's comment on QSerialPortPrivate no
// longer deriving from QIODevicePrivate), so QSerialPortPrivate below
// declares its own buffer/writeBuffer fields of this type instead.
//
// This is a plain QByteArray-backed stand-in, not upstream's actual
// chunked implementation - the performance characteristics differ, but
// the observable behavior is otherwise the same.
//
// Three methods are named differently from upstream's QRingBuffer -
// reserve()/chop()/free() became reserveBytes()/chopBytes()/freeBytes()
// - the other names below (size, isEmpty, clear, append, readPointer,
// nextDataBlockSize, indexOf, read) are unchanged from upstream and so
// needed no edits at qserialport_unix.cpp's call sites. Those three
// specifically had to be renamed: with the plain names, AppleClang
// silently miscompiled the call sites in qserialport_unix.cpp - the
// calls apparently got treated as if they were the C standard library
// functions of the same/similar name (reserve/free in particular) rather
// than these member functions, producing code that looked like it ran
// but never actually mutated `data` (confirmed by instrumenting each
// method: size()/append()/etc. all fired as expected, but reserve()/
// chop()/free() never did, despite their compiled code being present in
// the binary). Renaming them made the problem disappear immediately and
// reproducibly. However exactly the compiler decided these three names
// were special, giving them ordinary, unambiguous names was the fix.
//
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <QByteArray>

class QRingBuffer {
public:
	bool isEmpty() const { return data.isEmpty(); }
	qint64 size() const { return data.size(); }
	void clear() { data.clear(); }

	void appendBytes(const char *buf, qint64 len) { data.append(buf, int(len)); }

	// Appends `size` uninitialized bytes and returns a pointer to the
	// start of that new region, for the caller to fill in directly (see
	// readNotification()'s reserveBytes()-then-chopBytes() pattern).
	char *reserveBytes(qint64 size)
	{
		const qint64 oldSize = data.size();
		data.resize(int(oldSize + size));
		return data.data() + oldSize;
	}

	// Removes `size` bytes from the end - reserveBytes()'s undo, for
	// when fewer bytes were actually read than were reserved for.
	void chopBytes(qint64 size) { data.chop(int(size)); }

	// The whole buffered range in one contiguous span - upstream's
	// version may need multiple (block, size) pairs internally since its
	// backing store is chunked; this one never does, since it's just one
	// QByteArray.
	const char *readPointer() const { return data.constData(); }
	qint64 nextDataBlockSize() const { return data.size(); }

	// Used by QSerialPort::canReadLine() - qserialport.cpp can no longer
	// delegate that to QIODevice::canReadLine() for the same reason it
	// can't delegate bytesAvailable(), see there.
	qint64 indexOf(char c) const { return data.indexOf(c); }

	// Removes `size` bytes from the front (what readPointer() pointed
	// at), advancing past data actually consumed (e.g. actually written
	// to the port).
	void freeBytes(qint64 size) { data.remove(0, int(size)); }

	// Returns everything currently buffered and empties the buffer -
	// used where a whole pending write is handed off in one call
	// (qserialport_win.cpp's writeChunkBuffer = writeBuffer.read()).
	QByteArray read()
	{
		QByteArray result = data;
		data.clear();
		return result;
	}

private:
	QByteArray data;
};
