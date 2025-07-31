/******************************************************************************
	Copyright (C) 2016-2024 DistroAV <contact@distroav.org>

	This program is free software; you can redistribute it and/or
	modify it under the terms of the GNU General Public License
	as published by the Free Software Foundation; either version 2
	of the License, or (at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program; if not, see <https://www.gnu.org/licenses/>.
******************************************************************************/

#pragma once

#include <__stddef_null.h>
#include <Processing.NDI.Lib.h>

#define NDI_OFFICIAL_REDIST_URL NDILIB_REDIST_URL
#define NDI_OFFICIAL_TOOLS_URL "https://ndi.video/tools/download"
#define NDI_OFFICIAL_CPU_REQUIREMENTS_URL "https://docs.ndi.video/docs/sdk/cpu-requirements"
// Required by NDI license:
// Per https://github.com/DistroAV/DistroAV/blob/master/lib/ndi/NDI%20SDK%20Documentation.pdf
// "3 Licensing"
// "Your application must provide a link to https://ndi.video/ in a location close to all locations
// where NDI is used / selected within the product, on your web site, and in its documentation."
#define NDI_OFFICIAL_WEB_URL "https://ndi.video"
// "Your application’s About Box and any other locations where trademark attribution is provided
// should also specifically indicate that “NDI® is a registered trademark of Vizrt NDI AB”."
#define NDI_IS_A_REGISTERED_TRADEMARK_TEXT "PTZ.NDI.Trademark"
#define NDI_MIN_VERSION "6.0.0"

#include <util/base.h>
#include <obs-module.h>
#include <QString>
#include <QDir>
#include <QLibrary>
#include <QTimer>
#include <QRegularExpression>
#include <Processing.NDI.Lib.h>
#include <Processing.NDI.DynamicLoad.h>

#include "qt-wrappers.hpp"

typedef NDIlib_v6 *(*NDIlib_v6_load_)(void);

class NDI {
public:
	bool initNdiLib();
	bool isInitialized() const;
	NDIlib_v6 *lib = nullptr;

private:
	bool initialized = false;
	QLibrary *loaded_lib = nullptr;
	NDIlib_v6 *ndiLib = nullptr;

	NDIlib_v6 *loadNdiLib();
	static QString makeLink(const char *url);
	static bool isVersionSupported(const char *version, const char *min_version);
};

inline NDI *ndi;
