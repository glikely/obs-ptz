
#pragma once
#include <obs-frontend-api.h>

inline const char *Str(const char *lookup)
{
	return lookup;
}
#define QTStr(lookupVal) QString::fromUtf8(Str(lookupVal))

inline QWidget *GetMainWindow()
{
	return static_cast<QWidget *>(obs_frontend_get_main_window());
}
