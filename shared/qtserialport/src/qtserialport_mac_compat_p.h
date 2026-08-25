// Local replacement for Qt's private <private/qcore_mac_p.h> (just the
// QCFType<T>/QCFString CoreFoundation RAII helpers qserialportinfo_osx.cpp
// uses), same rationale and same names as qtserialport_unix_compat_p.h -
// see that file's comment. Trivial retain/release wrappers around plain
// C pointers, nothing here depends on Qt-internal struct layout either.
//
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <QString>

#include <CoreFoundation/CoreFoundation.h>

template<typename T> class QCFType {
public:
	inline QCFType(const T &t = nullptr) : value(t) {}
	inline QCFType(const QCFType &other) : value(other.value)
	{
		if (value)
			CFRetain(static_cast<CFTypeRef>(value));
	}
	inline ~QCFType()
	{
		if (value)
			CFRelease(static_cast<CFTypeRef>(value));
	}
	QCFType &operator=(const QCFType &other)
	{
		if (other.value)
			CFRetain(static_cast<CFTypeRef>(other.value));
		if (value)
			CFRelease(static_cast<CFTypeRef>(value));
		value = other.value;
		return *this;
	}
	inline operator T() const { return value; }
	template<typename U> U as() const { return reinterpret_cast<U>(value); }

protected:
	T value;
};

class QCFString : public QCFType<CFStringRef> {
public:
	inline QCFString(const CFStringRef cfstr = nullptr) : QCFType<CFStringRef>(cfstr) {}
	inline QCFString(const QCFType<CFStringRef> &other) : QCFType<CFStringRef>(other) {}
};
