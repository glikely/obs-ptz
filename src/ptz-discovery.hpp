/* Pan Tilt Zoom Controls - detecting devices a driver can control
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */
#pragma once

#include <functional>
#include <QList>
#include <QMetaType>
#include <QObject>
#include <QString>
#include <obs.hpp>

/* A device a driver found, that a new device could be made for */
struct PTZDetectedDevice {
	/* Tells it apart from every other device found, found again or found
	 * by another way of looking: an ONVIF endpoint's address, say */
	QString id;
	/* What to call it, and where it is, for the user to recognise it */
	QString name;
	QString address;
	/* The settings to make a device for it with: "type", and whatever it
	 * takes to reach it ("host" and "port", say). Nothing else; the
	 * driver's defaults fill in the rest. */
	OBSData settings;
};
Q_DECLARE_METATYPE(PTZDetectedDevice)

/**
 * A driver's way of detecting devices, on the network or wherever else they
 * can be found. One is made each time something wants to look (the Add
 * Device dialog, when it opens), and is gone again with its parent.
 *
 * start() looks for a while, on the thread it was made on, reporting each
 * device it finds once with deviceFound(), and then says finished(). stop()
 * gives up early, and also says finished(), if it hadn't already.
 * start() can be called again to look again.
 */
class PTZDiscovery : public QObject {
	Q_OBJECT

public:
	using QObject::QObject;
	virtual void start() = 0;
	virtual void stop() = 0;

signals:
	void deviceFound(const PTZDetectedDevice &device);
	void finished();
};

using PTZDiscoveryFactory = std::function<PTZDiscovery *(QObject *parent)>;

/* A driver that can detect devices registers how to make its PTZDiscovery,
 * as it registers its filter (see ptz_load_devices()) */
void ptz_discovery_register(const PTZDiscoveryFactory &factory);
/* One of each driver's, made with `parent` */
QList<PTZDiscovery *> ptz_discovery_create_all(QObject *parent);
/* Forgets every driver's, at unload */
void ptz_discovery_unregister_all();
