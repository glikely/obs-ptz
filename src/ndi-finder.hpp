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
#include <vector>
#include <string>
#include <thread>
#include <mutex>
#include <functional>
#include <chrono>
#include <Processing.NDI.Lib.h>
#include <Processing.NDI.Lib.cplusplus.h>
#include <Processing.NDI.structs.h>
#include <Processing.NDI.Lib.h>

#include <__stddef_null.h>
#include "ndi.hpp"

class NDIFinder {
public:
	using Callback = std::function<void(void *)>;
	static std::vector<std::string> getNDISourceList(Callback callback);

private:
	static std::vector<std::string> NDISourceList;
	static std::chrono::time_point<std::chrono::steady_clock> lastRefreshTime;
	static std::mutex listMutex;
	static bool isRefreshing;
	static void refreshNDISourceList(Callback callback);
	static void retrieveNDISourceList();
};
