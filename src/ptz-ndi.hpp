/* Pan Tilt Zoom over NDI
 *
 * Copyright 2025 Christian Mäder <mail@cmaeder.ch>
 *
 * SPDX-License-Identifier: GPLv2
 */

#pragma once

#include <util/base.h>
#include <QObject>
#include <QDebug>
#include <QStringListModel>
#include <QtGlobal>
#include <QHostInfo>

#include <__stddef_null.h>
#include <Processing.NDI.Lib.h>
#include <Processing.NDI.Lib.cplusplus.h>
#include <Processing.NDI.structs.h>
#include <Processing.NDI.Recv.h>
#include <Processing.NDI.Recv.ex.h>

#include "ptz-device.hpp"
#include "protocol-helpers.hpp"
#include "qt-wrappers.hpp"

#include "ndi.hpp"

/*
 * NDI PTZ Controller
 */
class PTZNDI : public PTZDevice {
	Q_OBJECT

public:
	static std::vector<std::string> NDISources;

private:
	const char* receiver_name;
	const char* source_name;
	NDIlib_recv_instance_t instance;

public:
	explicit PTZNDI(OBSData config);
	~PTZNDI() override;
	QString description() override;

	void set_config(OBSData ptz_data) override;
	OBSData get_config() override;
	obs_properties_t *get_obs_properties() override;

	void do_update() override;

	void pantilt_abs(double pan, double tilt) override;
	void pantilt_rel(double panSpeed, double tiltSpeed) override;
	void pantilt_home() override;
	void zoom_speed_set(double speed);
	void zoom_abs(double pos) override;
	void zoom_rel(double speed) const;
	void focus_abs(double pos) override;
	void focus_rel(double speed) const;
	void set_autofocus(bool enabled) override;
	void memory_set(int i) override;
	void memory_recall(int i) override;
};
