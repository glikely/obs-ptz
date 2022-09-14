#pragma once
#include <QMainWindow>
#include <obs-frontend-api.h>

inline const char *Str(const char *lookup)
{
	return lookup;
}
#define QTStr(lookupVal) QString::fromUtf8(Str(lookupVal))

class OBSApp {
public:
	inline QMainWindow *GetMainWindow() const
	{
		return static_cast<QMainWindow *>(obs_frontend_get_main_window());
	}

	inline bool IsThemeDark() const { return false; }
};

extern OBSApp fakeApp;

inline OBSApp *App()
{
	return &fakeApp;
}
