/* Pan Tilt Zoom Controls - detecting devices a driver can control
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */

#include "ptz-discovery.hpp"

/* Registered and used only on the main thread */
static QList<PTZDiscoveryFactory> ptz_discovery_factories;

void ptz_discovery_register(const PTZDiscoveryFactory &factory)
{
	ptz_discovery_factories.append(factory);
}

QList<PTZDiscovery *> ptz_discovery_create_all(QObject *parent)
{
	QList<PTZDiscovery *> discoveries;
	for (const auto &factory : ptz_discovery_factories) {
		PTZDiscovery *discovery = factory(parent);
		if (discovery)
			discoveries.append(discovery);
	}
	return discoveries;
}

void ptz_discovery_unregister_all()
{
	ptz_discovery_factories.clear();
}
