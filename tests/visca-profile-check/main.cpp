/* Checks the VISCA command sets shipped with the plugin
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Reads each command set in the directories given (src/visca-profiles, by
 * default) with the plugin's own reader, as the plugin reads the ones shipped
 * with it, which leaves out one it can't read with only a warning in the log.
 * Then checks what a shipped one must be besides: in a file named for its
 * id, saying where it came from, and the only one for the cameras it is
 * chosen for. Says what is wrong with each that isn't, and fails if any is.
 *
 *     visca-profile-check [DIR...] [--expect-errors DIR]
 *
 * A directory after --expect-errors is of command sets that are wrong, each
 * named "bad-...", alongside ones that aren't, to check the checks with.
 */
#include <cstdio>
#include <utility>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <obs-module.h>
#include <util/base.h>
#include <util/bmem.h>

#include "ptz-visca-commands.hpp"

/* What OBS_DECLARE_MODULE() gives the plugin, which the reader asks for:
 * the generic command set's name, and the module, for where a user's
 * command sets are, which aren't read here */
extern "C" {
const char *obs_module_text(const char *lookup)
{
	return lookup;
}

obs_module_t *obs_current_module(void)
{
	return nullptr;
}
}

/* The reader logs what it reads; what is wrong is said below */
static void quiet(int, const char *, va_list, void *) {}

/* Whether what is wrong in `path` is all that should be: everything, or
 * if `expected`, just the files named "bad-..." */
static int check(const QString &path, bool expected)
{
	const QDir dir(path);
	if (!dir.exists()) {
		fprintf(stderr, "%s: no such directory\n", qUtf8Printable(path));
		return 1;
	}
	QMap<QString, QString> errors;
	const auto profiles = visca_load_shipped_profiles(dir, &errors);

	/* each file, by the id in it */
	QMap<QString, QString> files;
	QMap<QString, QJsonObject> json;
	for (const auto &file : dir.entryList({"*.json"}, QDir::Files, QDir::Name)) {
		QFile source(dir.filePath(file));
		if (!source.open(QIODevice::ReadOnly))
			continue;
		json[file] = QJsonDocument::fromJson(source.readAll()).object();
		files.insert(json[file]["id"].toString(), file);
	}

	QMap<int, QString> models;
	for (const auto &profile : profiles) {
		const QString file = files.value(profile->id);
		if (file != profile->id + ".json") {
			errors.insert(file, QString("isn't named for its id: %1.json").arg(profile->id));
			continue;
		}
		if (json[file]["source"].toString().trimmed().isEmpty())
			errors.insert(file, "doesn't say where it came from, in its \"source\"");
		/* which would be chosen for a camera would be down to the order
		 * they are read in */
		for (int model : profile->models) {
			if (models.contains(model))
				errors.insert(file, QString("is for %1:%2, as %3 is")
							    .arg(model >> 16, 4, 16, QChar('0'))
							    .arg(model & 0xffff, 4, 16, QChar('0'))
							    .arg(models[model]));
			models.insert(model, file);
		}
	}

	if (expected) {
		int failed = 0;
		for (const auto &file : dir.entryList({"*.json"}, QDir::Files, QDir::Name)) {
			bool bad = file.startsWith("bad-");
			if (bad && !errors.contains(file)) {
				fprintf(stderr, "%s/%s: is fine, but shouldn't be\n", qUtf8Printable(path),
					qUtf8Printable(file));
				failed = 1;
			} else if (!bad && errors.contains(file)) {
				fprintf(stderr, "%s/%s: %s\n", qUtf8Printable(path), qUtf8Printable(file),
					qUtf8Printable(errors[file]));
				failed = 1;
			}
		}
		if (!failed)
			printf("%s: every bad- command set has what is wrong with it found\n", qUtf8Printable(path));
		return failed;
	}
	for (auto i = errors.begin(); i != errors.end(); i++)
		fprintf(stderr, "%s/%s: %s\n", qUtf8Printable(path), qUtf8Printable(i.key()),
			qUtf8Printable(i.value()));
	if (errors.isEmpty())
		printf("%s: %lld command sets, all fine\n", qUtf8Printable(path), (long long)profiles.size());
	return errors.isEmpty() ? 0 : 1;
}

int main(int argc, char **argv)
{
	base_set_log_handler(quiet, nullptr);
	/* directories, each after --expect-errors to be one of bad command
	 * sets, which must be found to be */
	QList<QPair<QString, bool>> dirs;
	bool expected = false;
	for (int i = 1; i < argc; i++) {
		if (QString(argv[i]) == "--expect-errors")
			expected = true;
		else
			dirs.append({QString::fromLocal8Bit(argv[i]), std::exchange(expected, false)});
	}
	if (dirs.isEmpty())
		dirs.append({"src/visca-profiles", false});
	int failed = 0;
	for (const auto &[dir, bad] : dirs)
		failed |= check(dir, bad);
	return failed;
}
