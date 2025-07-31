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

#include "ndi.hpp"


QString NDI::makeLink(const char *url)
{
	return QString("<a href=\"%1\">%2</a>").arg(url, QString::fromUtf8(url));
}

NDIlib_v6* NDI::loadNdiLib()
{
	auto locations = QStringList();
	auto temp_path = QString(qgetenv(NDILIB_REDIST_FOLDER));
	if (!temp_path.isEmpty()) {
		locations << temp_path;
	}
#if defined(Q_OS_LINUX) || defined(Q_OS_MACOS)
	// Linux, MacOS
	// https://github.com/DistroAV/DistroAV/blob/master/lib/ndi/NDI%20SDK%20Documentation.pdf
	// "6.1 LOCATING THE LIBRARY
	// ... the redistributable on MacOS is installed within `/usr/local/lib` ..."
	// Flatpak install will look for the NDI lib in /app/plugins/DistroAV/extra/lib
	locations << "/usr/lib";
	locations << "/usr/local/lib";
#if defined(Q_OS_LINUX)
	locations << "/app/plugins/DistroAV/extra/lib";
#endif
#endif
	auto lib_path = QString();
#if defined(Q_OS_LINUX)
	// Linux
	auto regex = QRegularExpression("libndi\\.so\\.(\\d+)");
	int max_version = 0;
#endif
	for (const auto &location : locations) {
		auto dir = QDir(location);
#if defined(Q_OS_LINUX)
		// Linux
		auto filters = QStringList("libndi.so.*");
		dir.setNameFilters(filters);
		auto file_names = dir.entryList(QDir::Files);
		for (const auto &file_name : file_names) {
			auto match = regex.match(file_name);
			if (match.hasMatch()) {
				int version = match.captured(1).toInt();
				if (version > max_version) {
					max_version = version;
					lib_path = dir.absoluteFilePath(file_name);
				}
			}
		}
#else
		// MacOS, Windows
		temp_path = QDir::cleanPath(dir.absoluteFilePath(NDILIB_LIBRARY_NAME));
		blog(LOG_DEBUG, "loadNdiLib: Trying '%s'", QT_TO_UTF8(QDir::toNativeSeparators(temp_path)));
		auto file_info = QFileInfo(temp_path);
		if (file_info.exists() && file_info.isFile()) {
			lib_path = temp_path;
			break;
		}
#endif
	}
	if (!lib_path.isEmpty()) {
		blog(LOG_DEBUG, "loadNdiLib: Found '%s'; attempting to load NDI library...",
			QT_TO_UTF8(QDir::toNativeSeparators(lib_path)));
		loaded_lib = new QLibrary(lib_path, nullptr);
		if (loaded_lib->load()) {
			blog(LOG_DEBUG, "loadNdiLib: NDI library loaded successfully");
			const auto lib_load =
				reinterpret_cast<NDIlib_v6_load_>(loaded_lib->resolve("NDIlib_v6_load"));
			if (lib_load != nullptr) {
				blog(LOG_DEBUG, "loadNdiLib: NDIlib_v6_load found");
				return lib_load();
			}
			blog(LOG_ERROR, "ERR-405 - Error loading the NDI Library from path: '%s'",
			     QT_TO_UTF8(QDir::toNativeSeparators(lib_path)));
			blog(LOG_DEBUG, "loadNdiLib: ERROR: NDIlib_v6_load not found in loaded library");
		} else {
			blog(LOG_ERROR, "ERR-402 - Error loading QLibrary with error: '%s'",
				QT_TO_UTF8(loaded_lib->errorString()));
			blog(LOG_DEBUG, "loadNdiLib: ERROR: QLibrary returned the following error: '%s'",
				QT_TO_UTF8(loaded_lib->errorString()));
			delete loaded_lib;
			loaded_lib = nullptr;
		}
	}

	blog(LOG_ERROR,
		"ERR-404 - NDI library not found, obs-ptz cannot continue. Read the wiki and install the NDI Libraries.");
	blog(LOG_DEBUG, "loadNdiLib: ERROR: Can't find the NDI library");
	return nullptr;
}

bool NDI::isInitialized() const
{
	return initialized;
}

bool NDI::initNdiLib()
{
#if 0
	// For testing purposes only
	ndiLib = nullptr;
#else
	lib = loadNdiLib();
#endif
	if (!lib) {
		auto message = "Error-401: " + QString::fromUtf8(obs_module_text("NDIPlugin.LibError.Message")) + "<br>";
#ifdef NDI_OFFICIAL_REDIST_URL
		message += makeLink(NDI_OFFICIAL_REDIST_URL);
#endif
		blog(LOG_ERROR, "ERR-401 - NDI library failed to load with message: '%s'", QT_TO_UTF8(message));
		blog(LOG_DEBUG, "obs_module_load: ERROR - loadNdiLib() failed; message=%s", QT_TO_UTF8(message));
		return false;
	}

#if 0
	// for testing purposes only
	auto initialized = false;
#else
	auto initialized = lib->initialize();
#endif
	if (!initialized) {
		blog(LOG_ERROR, "ERR-406 - NDI library could not initialize due to unsupported CPU.");
		blog(LOG_DEBUG,
			"obs_module_load: ndiLib->initialize() failed; CPU unsupported by NDI library. Module won't load.");
		return false;
	}

	blog(LOG_INFO, "obs_module_load: NDI library detected ('%s')", lib->version());

	// Check if the minimum NDI Runtime/SDK required by this plugin is used
	const QString ndi_version_short =
		QRegularExpression(R"((\d+\.\d+(\.\d+)?(\.\d+)?$))").match(lib->version()).captured(1);
	blog(LOG_INFO, "NDI Version detected: %s", QT_TO_UTF8(ndi_version_short));

	if (!isVersionSupported(QT_TO_UTF8(ndi_version_short), NDI_MIN_VERSION)) {
		blog(LOG_ERROR,
			"ERR-425 - %s requires at least NDI version %s. NDI Version detected: %s. Plugin will unload.",
			"obs-ptz", NDI_MIN_VERSION, QT_TO_UTF8(ndi_version_short));
		blog(LOG_DEBUG, "obs_module_load: NDI minimum version not met (%s). NDI version detected: %s.",
			NDI_MIN_VERSION, lib->version());
		return false;
	}

	initialized = true;
	blog(LOG_INFO, "obs_module_load: NDI library initialized successfully");
	return true;
}

bool NDI::isVersionSupported(const char *version, const char *min_version)
{
	if (version == nullptr || min_version == nullptr) {
		return false;
	}

	auto version_parts = QString::fromUtf8(version).split(".");
	auto min_version_parts = QString::fromUtf8(min_version).split(".");

	for (int i = 0; i < qMax(version_parts.size(), min_version_parts.size()); ++i) {
		const auto version_part = i < version_parts.size() ? version_parts[i].toInt() : 0;
		const auto min_version_part = i < min_version_parts.size() ? min_version_parts[i].toInt() : 0;

		if (version_part > min_version_part) {
			return true;
		}
		if (version_part < min_version_part) {
			return false;
		}
	}
	return true;
}
