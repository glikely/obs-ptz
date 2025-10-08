/******************************************************************************
	Copyright (C) 2016-2024 DistroAV <contact@distroav.org>
	Copyright (C) 2025 Christian Mäder <mail@cmaeder.ch>

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

#include "ndi-finder.hpp"

std::vector<std::string> NDIFinder::NDISourceList;
std::chrono::time_point<std::chrono::steady_clock> NDIFinder::lastRefreshTime;
std::mutex NDIFinder::listMutex;
bool NDIFinder::isRefreshing = false;

std::vector<std::string> NDIFinder::getNDISourceList(Callback callback)
{
	std::lock_guard lock(listMutex);
	auto now = std::chrono::steady_clock::now();
	auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - lastRefreshTime).count();

	if (duration > 5 && !isRefreshing) {
		isRefreshing = true;
		std::thread(refreshNDISourceList, callback).detach();
	}

	return NDISourceList;
}

void NDIFinder::refreshNDISourceList(Callback callback)
{
	retrieveNDISourceList();
	std::lock_guard lock(listMutex);
	lastRefreshTime = std::chrono::steady_clock::now();
	if (callback != nullptr) {
		callback(&NDISourceList);
	}
	isRefreshing = false;
}

void NDIFinder::retrieveNDISourceList()
{
	NDIlib_find_create_t find_desc = {0};
	find_desc.show_local_sources = true;
	find_desc.p_groups = nullptr;
	find_desc.p_extra_ips = qEnvironmentVariable("OBS_PTZ_NDI_EXTRA_IPS", "").toStdString().c_str();
	const NDIlib_find_instance_t ndi_find = ndi->lib->find_create_v2(&find_desc);

	if (!ndi_find) {
		return;
	}

	uint32_t n_sources = 0;
	uint32_t last_n_sources = 0;
	const NDIlib_source_t *sources = nullptr;
	do {
		ndi->lib->find_wait_for_sources(ndi_find, 1000);
		last_n_sources = n_sources;
		sources = ndi->lib->find_get_current_sources(ndi_find, &n_sources);
	} while (n_sources > last_n_sources);

	std::vector<std::string> newList;
	for (uint32_t i = 0; i < n_sources; ++i) {
		newList.push_back(sources[i].p_ndi_name);
	}

	{
		std::lock_guard lock(listMutex);
		NDISourceList = newList;
	}

	ndi->lib->find_destroy(ndi_find);
}
