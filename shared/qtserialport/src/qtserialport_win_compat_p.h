// Local replacements for two small Windows-only Qt private headers used by
// qserialportinfo_win.cpp - <private/quniquehandle_types_p.h> (QUniqueHandle,
// QUniqueWin32Handle) and <private/qwinregistry_p.h> (QWinRegistryKey).
//
// Both are simplified down to exactly the interface qserialportinfo_win.cpp
// actually calls, not upstream's full API surface (upstream's QUniqueHandle
// additionally supports custom deleters, comparison operators, swap(),
// release()/reset() - none of which this file uses; upstream's
// QWinRegistryKey additionally has stringValue()/dwordValue() accessors and
// a default constructor - also unused here). Neither needed any Qt-private
// access to begin with; both are self-contained RAII wrappers around plain
// Win32 handles.
//
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <QString>

#include <qt_windows.h>

// Upstream's version supports a pluggable close()-failure Deleter and
// comparison/swap operators via QtPrivate::CompactStorage - unused by this
// fork's only two instantiations (DevInfoHandleTraits in
// qserialportinfo_win.cpp, and QUniqueWin32Handle below), so left out here.
template<typename HandleTraits>
class QUniqueHandle {
public:
	using Type = typename HandleTraits::Type;

	QUniqueHandle() noexcept : m_handle(HandleTraits::invalidValue()) {}
	explicit QUniqueHandle(const Type &handle) noexcept : m_handle(handle) {}
	QUniqueHandle(const QUniqueHandle &) = delete;
	QUniqueHandle &operator=(const QUniqueHandle &) = delete;
	QUniqueHandle(QUniqueHandle &&other) noexcept : m_handle(other.release()) {}

	~QUniqueHandle() { close(); }

	bool isValid() const noexcept { return m_handle != HandleTraits::invalidValue(); }
	explicit operator bool() const noexcept { return isValid(); }
	Type get() const noexcept { return m_handle; }

	void close() noexcept
	{
		if (isValid())
			HandleTraits::close(m_handle);
		m_handle = HandleTraits::invalidValue();
	}

	Type release() noexcept
	{
		Type h = m_handle;
		m_handle = HandleTraits::invalidValue();
		return h;
	}

private:
	Type m_handle;
};

namespace QtUniqueHandleTraits {

struct InvalidHandleTraits {
	using Type = HANDLE;
	static Type invalidValue() noexcept { return INVALID_HANDLE_VALUE; }
	static bool close(Type handle) noexcept { return ::CloseHandle(handle) != 0; }
};

} // namespace QtUniqueHandleTraits

using QUniqueWin32Handle = QUniqueHandle<QtUniqueHandleTraits::InvalidHandleTraits>;

// Matches the 4-argument RegOpenKeyExW-shaped constructor
// qserialportinfo_win.cpp calls (parentHandle, subKey, options, access) -
// upstream versions of this class have varied their exact constructor
// signature across Qt releases; this one is written directly against this
// fork's one call site rather than against any particular upstream version.
class QWinRegistryKey {
public:
	QWinRegistryKey() = default;
	QWinRegistryKey(HKEY parentHandle, const wchar_t *subKey, DWORD options, REGSAM access)
	{
		if (::RegOpenKeyExW(parentHandle, subKey, options, access, &m_key) != ERROR_SUCCESS)
			m_key = nullptr;
	}
	QWinRegistryKey(const QWinRegistryKey &) = delete;
	QWinRegistryKey &operator=(const QWinRegistryKey &) = delete;

	~QWinRegistryKey()
	{
		if (m_key)
			::RegCloseKey(m_key);
	}

	bool isValid() const noexcept { return m_key != nullptr; }
	operator HKEY() const noexcept { return m_key; }

private:
	HKEY m_key = nullptr;
};
