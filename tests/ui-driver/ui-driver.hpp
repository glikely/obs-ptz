/* obs-ui-driver: adding a request to the vendor
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once

#include <QJsonObject>
#include <QString>

#include <functional>

/* What a request does: given its JSON, gives back its answer, on the main thread */
using Handler = std::function<QJsonObject(const QJsonObject &request)>;

/* Puts the request `name` on the vendor. Only from obs_module_post_load(). */
void addRequest(const char *name, Handler handler);

/* The answer to a request that could not be done: {"error": code, "message": message} */
QJsonObject error(const char *code, const QString &message);
