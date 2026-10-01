/* Pan Tilt Zoom visca instance
 *
 * Copyright 2020 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */

#include <qt-wrappers.hpp>
#include "ptz-visca.hpp"
#include "ptz-visca-udp.hpp"
#include "ptz-visca-tcp.hpp"
#if defined(ENABLE_SERIALPORT)
#include "ptz-visca-uart.hpp"
#endif
#include <util/base.h>

/* How many update ticks (a second each) discover_limits() lets an axis take to
 * reach the end of its travel, and how many in a row it must have stood still
 * to have reached it */
static constexpr int VISCA_DISCOVER_MAX_TICKS = 60;
static constexpr int VISCA_DISCOVER_STABLE_TICKS = 2;

/* How long to wait for the reply to a request before sending it again. Cameras
 * take up to ~130ms to answer, so this must be well over that. */
static constexpr int VISCA_REPLY_TIMEOUT_MS = 250;

/* Error reply "command buffer full", and how to deal with it */
/* How long a power-on at startup waits for the camera to answer */
static constexpr int VISCA_POWER_ON_WAIT_MS = 60000;

static constexpr int VISCA_ERROR_SYNTAX = 0x02;
static constexpr int VISCA_ERROR_BUFFER_FULL = 0x03;
static constexpr int VISCA_BUSY_BACKOFF_MS = 50;
static constexpr unsigned int VISCA_BUSY_RETRIES_MAX = 20;

/*
 * PTZVisca Methods
 */
PTZVisca::PTZVisca(OBSData config, obs_source_t *source) : PTZDevice(config, source)
{
	for (int i = 0; i < 8; i++)
		active_cmd[i] = std::nullopt;
	connect(&timeout_timer, &QTimer::timeout, this, &PTZVisca::timeout);
	gap_timer.setSingleShot(true);
	connect(&gap_timer, &QTimer::timeout, this, &PTZVisca::send_pending);
	connect(&update_timer, &QTimer::timeout, this, &PTZVisca::update_timer_callback);

	update(config);
}

QString PTZVisca::description() const
{
	return transport ? transport->description(address) : QString();
}

void PTZVisca::reset()
{
	link_answered = false;
	cmd_get_camera_info();
}

void PTZVisca::setInterface(const QString &interface)
{
	if (transport && visca_interface == interface)
		return;

	if (transport) {
		transport->disconnect(this);
		delete transport;
		transport = nullptr;
	}

	visca_interface = interface;
	if (interface == "tcp")
		transport = new ViscaTCPTransport();
#if defined(ENABLE_SERIALPORT)
	else if (interface == "serial")
		transport = new ViscaSerialTransport();
#endif
	else
		transport = new ViscaUDPTransport();

	connect(transport, &ViscaTransport::receive, this, &PTZVisca::receive);
	connect(transport, &ViscaTransport::reset, this, &PTZVisca::reset);
	connect(transport, &ViscaTransport::refresh, this, &PTZVisca::cmd_get_camera_info);
	connect(transport, &ViscaTransport::statIncrement, this,
		[this](const QString &name) { incrementStatistic(qPrintable(name)); });
}

void PTZVisca::send_immediate(const QByteArray &msg)
{
	if (transport)
		transport->send(msg, address);
}

void PTZVisca::scan_commands()
{
	for (int i = 0; i < 0x7e; i++) {
		PTZInq inq("81090000ff");
		inq.cmd[3] = i;
		pending_cmds += inq;
	}
	for (int i = 0; i < 0x7e; i++) {
		PTZInq inq("81090400ff");
		inq.cmd[3] = i;
		pending_cmds += inq;
	}
	for (int i = 0; i < 0x7e; i++) {
		PTZInq inq("81090600ff");
		inq.cmd[3] = i;
		pending_cmds += inq;
	}
	for (int i = 0; i < 0x7e; i++) {
		PTZInq inq("81097e0100ff");
		inq.cmd[4] = i;
		pending_cmds += inq;
	}
	for (int i = 0; i < 0x7e; i++) {
		PTZInq inq("81097e7e00ff");
		inq.cmd[4] = i;
		pending_cmds += inq;
	}
}

void PTZVisca::write_replies_to_log()
{
	for (auto key : replyLast.keys())
		ptz_info("inq:%s count:%.4i reply:%s", key.toHex(':').data(), replyCount[key],
			 replyLast[key].toHex(':').data());
}

void PTZVisca::defaults(obs_data_t *cfg)
{
	PTZDevice::defaults(cfg);

	/* The transport is selected by device "type" (visca / visca-over-ip /
	 * visca-over-tcp), same as before the VISCA drivers were unified into
	 * a single class; keep using those type strings and their original
	 * "host"/"port"/"address" field names so the on-disk config format is
	 * unchanged. */
	obs_data_set_default_int(cfg, "address", 1);
	obs_data_set_default_int(cfg, "visca_pan_speed_max", 0x18);
	obs_data_set_default_int(cfg, "visca_tilt_speed_max", 0x14);
	obs_data_set_default_int(cfg, "visca_zoom_speed_max", 0x7);
	obs_data_set_default_int(cfg, "visca_focus_speed_max", 0x7);
	obs_data_set_default_bool(cfg, "visca_ranges_auto", true);
	obs_data_set_default_int(cfg, "visca_pan_range", VISCA_DEFAULT_PAN_RANGE);
	obs_data_set_default_int(cfg, "visca_tilt_range", VISCA_DEFAULT_TILT_RANGE);
	obs_data_set_default_int(cfg, "visca_zoom_range", VISCA_DEFAULT_ZOOM_RANGE);
	obs_data_set_default_int(cfg, "visca_focus_far", VISCA_DEFAULT_FOCUS_FAR);
	obs_data_set_default_int(cfg, "visca_focus_near", VISCA_DEFAULT_FOCUS_NEAR);
	obs_data_set_default_bool(cfg, "protocol_trace", false);
	obs_data_set_default_bool(cfg, "tally_auto", true);
	obs_data_set_default_bool(cfg, "power_on_at_startup", false);
	obs_data_set_default_bool(cfg, "power_off_at_shutdown", false);
	obs_data_set_default_string(cfg, "visca_profile", "auto");
}

void PTZVisca::update(OBSData cfg)
{
	PTZDevice::update(cfg);

	std::string cfg_type = obs_data_get_string(cfg, "type");
	type = cfg_type;
	QString interface = (cfg_type == "visca-over-ip") ? "udp" : (cfg_type == "visca-over-tcp") ? "tcp" : "serial";
	setInterface(interface);
	address = (visca_interface == "serial") ? std::clamp((int)obs_data_get_int(cfg, "address"), 1, 7) : 1;

	visca_pan_speed_max = (int)obs_data_get_int(cfg, "visca_pan_speed_max");
	visca_tilt_speed_max = (int)obs_data_get_int(cfg, "visca_tilt_speed_max");
	visca_zoom_speed_max = (int)obs_data_get_int(cfg, "visca_zoom_speed_max");
	visca_focus_speed_max = (int)obs_data_get_int(cfg, "visca_focus_speed_max");
	visca_pan_range = std::max(1, (int)obs_data_get_int(cfg, "visca_pan_range"));
	visca_tilt_range = std::max(1, (int)obs_data_get_int(cfg, "visca_tilt_range"));
	visca_zoom_range = std::max(1, (int)obs_data_get_int(cfg, "visca_zoom_range"));
	visca_focus_far = (int)obs_data_get_int(cfg, "visca_focus_far");
	visca_focus_near = (int)obs_data_get_int(cfg, "visca_focus_near");
	if (visca_focus_near == visca_focus_far)
		visca_focus_near = visca_focus_far + 1;
	/* A device from before the ranges could be the command set's has its
	 * own, if they were ever changed from the defaults */
	if (obs_data_has_user_value(cfg, "visca_ranges_auto"))
		visca_ranges_auto = obs_data_get_bool(cfg, "visca_ranges_auto");
	else
		visca_ranges_auto =
			visca_pan_range == VISCA_DEFAULT_PAN_RANGE && visca_tilt_range == VISCA_DEFAULT_TILT_RANGE &&
			visca_zoom_range == VISCA_DEFAULT_ZOOM_RANGE && visca_focus_far == VISCA_DEFAULT_FOCUS_FAR &&
			visca_focus_near == VISCA_DEFAULT_FOCUS_NEAR;
	protocol_trace = obs_data_get_bool(cfg, "protocol_trace");
	tally_auto = obs_data_get_bool(cfg, "tally_auto");
	power_on_at_startup = obs_data_get_bool(cfg, "power_on_at_startup");
	power_off_at_shutdown = obs_data_get_bool(cfg, "power_off_at_shutdown");
	profile_setting = obs_data_get_string(cfg, "visca_profile");
	choose_profile();
	apply_ranges();

	transport->update(cfg);
	transport->setSourceHost(parentSourceHost());
	unsupported_requests.clear();
}

void PTZVisca::onParentHostChanged(const QString &host)
{
	if (transport)
		transport->setSourceHost(host);
}

void PTZVisca::save(OBSData cfg) const
{
	PTZDevice::save(cfg);
	if (visca_interface == "serial")
		obs_data_set_int(cfg, "address", address);
	obs_data_set_int(cfg, "visca_pan_speed_max", visca_pan_speed_max);
	obs_data_set_int(cfg, "visca_tilt_speed_max", visca_tilt_speed_max);
	obs_data_set_int(cfg, "visca_zoom_speed_max", visca_zoom_speed_max);
	obs_data_set_int(cfg, "visca_focus_speed_max", visca_focus_speed_max);
	obs_data_set_bool(cfg, "visca_ranges_auto", visca_ranges_auto);
	obs_data_set_int(cfg, "visca_pan_range", visca_pan_range);
	obs_data_set_int(cfg, "visca_tilt_range", visca_tilt_range);
	obs_data_set_int(cfg, "visca_zoom_range", visca_zoom_range);
	obs_data_set_int(cfg, "visca_focus_far", visca_focus_far);
	obs_data_set_int(cfg, "visca_focus_near", visca_focus_near);
	obs_data_set_bool(cfg, "protocol_trace", protocol_trace);
	obs_data_set_bool(cfg, "tally_auto", tally_auto);
	obs_data_set_bool(cfg, "power_on_at_startup", power_on_at_startup);
	obs_data_set_bool(cfg, "power_off_at_shutdown", power_off_at_shutdown);
	obs_data_set_string(cfg, "visca_profile", QT_TO_UTF8(profile_setting));
	if (transport)
		transport->save(cfg);
}

/* Add the connection fields for one VISCA transport type ("visca" /
 * "visca-over-ip" / "visca-over-tcp", the same strings the "type" field has
 * always used). Only one transport's fields are ever present in the
 * properties dialog at a time, so they can reuse the original "host"/
 * "port"/"address" field names without colliding with each other. */
static void visca_add_interface_fields(obs_properties_t *props, const std::string &type)
{
	if (type == "visca-over-tcp")
		ViscaTCPTransport::add_obs_properties(props);
	else if (type == "visca-over-ip")
		ViscaUDPTransport::add_obs_properties(props);
#if defined(ENABLE_SERIALPORT)
	else
		ViscaSerialTransport::add_obs_properties(props);
#endif
}

static const char *visca_interface_description(const std::string &type)
{
	if (type == "visca-over-tcp")
		return obs_module_text("PTZ.Visca.TCP.Description");
	if (type == "visca-over-ip")
		return obs_module_text("PTZ.Visca.UDP.Description");
	return obs_module_text("PTZ.Visca.Serial.Description");
}

/* Swap the transport-specific fields (and their widget types, e.g. the
 * serial port combo vs the UDP/TCP numeric port spinner) when the user
 * picks a different "type" from the Protocol list. Remove whichever set is
 * currently present -- harmless no-op for any that aren't -- then add the
 * ones for the newly selected type. */
static bool visca_type_modified_cb(void *, obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
	obs_properties_remove_by_name(props, "host");
	obs_properties_remove_by_name(props, "udp_port");
	obs_properties_remove_by_name(props, "tcp_port");
	obs_properties_remove_by_name(props, "quirk_visca_udp_no_seq");
#if defined(ENABLE_SERIALPORT)
	obs_properties_remove_by_name(props, "serial_port");
	obs_properties_remove_by_name(props, "baud_rate");
	obs_properties_remove_by_name(props, "address");
#endif

	std::string type = obs_data_get_string(settings, "type");

	obs_property_t *iface_group = obs_properties_get(props, "interface");
	if (iface_group) {
		visca_add_interface_fields(obs_property_group_content(iface_group), type);
		obs_property_set_description(iface_group, visca_interface_description(type));
	}

	return true;
}

obs_properties_t *PTZVisca::get_obs_properties()
{
	auto *ptz_props = PTZDevice::get_obs_properties();

	obs_property_t *iface_group = obs_properties_get(ptz_props, "interface");
	obs_properties_t *iface_props = obs_property_group_content(iface_group);
	obs_property_set_description(iface_group, visca_interface_description(type));

	obs_property_t *type_list = obs_properties_add_list(iface_props, "type", obs_module_text("PTZ.Visca.Interface"),
							    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
#if defined(ENABLE_SERIALPORT)
	obs_property_list_add_string(type_list, obs_module_text("PTZ.Visca.Serial.Name"), "visca");
#endif
	obs_property_list_add_string(type_list, obs_module_text("PTZ.Visca.UDP.Name"), "visca-over-ip");
	obs_property_list_add_string(type_list, obs_module_text("PTZ.Visca.TCP.Name"), "visca-over-tcp");
	obs_property_set_modified_callback2(type_list, visca_type_modified_cb, nullptr);

	visca_add_interface_fields(iface_props, type);

	obs_properties_add_bool(ptz_props, "tally_auto", obs_module_text("PTZ.Visca.TallyAuto"));
	obs_properties_add_bool(ptz_props, "power_on_at_startup", obs_module_text("PTZ.Visca.PowerOnAtStartup"));
	obs_properties_add_bool(ptz_props, "power_off_at_shutdown", obs_module_text("PTZ.Visca.PowerOffAtShutdown"));

	auto visca_grp = obs_properties_create();
	obs_properties_add_group(ptz_props, "visca_advanced", obs_module_text("PTZ.Settings.Advanced"),
				 OBS_GROUP_CHECKABLE, visca_grp);
	obs_property_t *profile_list = obs_properties_add_list(visca_grp, "visca_profile",
							       obs_module_text("PTZ.Visca.Profile"),
							       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(profile_list, obs_module_text("PTZ.Visca.Profile.Auto"), "auto");
	for (const auto &p : visca_profiles())
		obs_property_list_add_string(profile_list, QT_TO_UTF8(p->name), QT_TO_UTF8(p->id));
	obs_properties_add_int_slider(visca_grp, "visca_pan_speed_max", obs_module_text("PTZ.Visca.PanMaxSpeed"), 1,
				      0x7f, 1);
	obs_properties_add_int_slider(visca_grp, "visca_tilt_speed_max", obs_module_text("PTZ.Visca.TiltMaxSpeed"), 1,
				      0x7f, 1);
	obs_properties_add_int_slider(visca_grp, "visca_zoom_speed_max", obs_module_text("PTZ.Visca.ZoomMaxSpeed"), 0,
				      7, 1);
	obs_properties_add_int_slider(visca_grp, "visca_focus_speed_max", obs_module_text("PTZ.Visca.FocusMaxSpeed"), 0,
				      7, 1);
	/* What 1.0 is in the movement API. A camera that is not one of these
	 * would have its position shown stuck at an end, and absolute moves
	 * go to the wrong place; "Discover Movement Limits" in the status
	 * view finds them. Unless they are the command set's. */
	obs_properties_add_bool(visca_grp, "visca_ranges_auto", obs_module_text("PTZ.Visca.RangesAuto"));
	obs_properties_add_int(visca_grp, "visca_pan_range", obs_module_text("PTZ.Visca.PanRange"), 1, 0xffff, 1);
	obs_properties_add_int(visca_grp, "visca_tilt_range", obs_module_text("PTZ.Visca.TiltRange"), 1, 0xffff, 1);
	obs_properties_add_int(visca_grp, "visca_zoom_range", obs_module_text("PTZ.Visca.ZoomRange"), 1, 0xffff, 1);
	obs_properties_add_int(visca_grp, "visca_focus_far", obs_module_text("PTZ.Visca.FocusFar"), 0, 0xffff, 1);
	obs_properties_add_int(visca_grp, "visca_focus_near", obs_module_text("PTZ.Visca.FocusNear"), 0, 0xffff, 1);
	obs_properties_add_bool(visca_grp, "protocol_trace", obs_module_text("PTZ.Device.ProtocolTraceToLog"));

	return ptz_props;
}

/* The camera's position on a pan or tilt axis that is centred on 0, at
 * `pos` in [-1, 1] of the movement API: -1 is `ends.first`, 1 the second.
 * Either can be on either side of 0. */
static int visca_from_unit(double pos, QPair<int, int> ends)
{
	pos = std::clamp(pos, -1.0, 1.0);
	return pos >= 0 ? pos * ends.second : -pos * ends.first;
}

static double visca_to_unit(int pos, QPair<int, int> ends)
{
	if (pos == 0)
		return 0;
	if ((pos > 0) == (ends.second > 0))
		return ends.second ? (double)pos / ends.second : 0;
	return ends.first ? -(double)pos / ends.first : 0;
}

/* ...and on a zoom or focus axis, from one end at 0 to the other at 1 */
static int visca_from_span(double pos, QPair<int, int> ends)
{
	return ends.first + std::clamp(pos, 0.0, 1.0) * (ends.second - ends.first);
}

static double visca_to_span(int pos, QPair<int, int> ends)
{
	return (pos - ends.first) / (double)(ends.second - ends.first);
}

/* Where the ends of each axis are: the command set's, if the settings say
 * to use them and it has them, or the settings', the way round the command
 * set has them, for a camera whose positions run the other way */
void PTZVisca::apply_ranges()
{
	auto setting = [this](const char *axis, QPair<int, int> fromSettings) {
		auto ends = profile->ranges.constFind(axis);
		if (ends == profile->ranges.constEnd())
			return fromSettings;
		if (visca_ranges_auto)
			return *ends;
		/* settings are magnitudes, centred on 0 */
		if (ends->second < 0 && fromSettings.first < 0)
			return QPair<int, int>{-fromSettings.first, -fromSettings.second};
		return fromSettings;
	};
	pan_ends = setting("pan", {-visca_pan_range, visca_pan_range});
	tilt_ends = setting("tilt", {-visca_tilt_range, visca_tilt_range});
	zoom_ends = setting("zoom", {0, visca_zoom_range});
	focus_ends = setting("focus", {visca_focus_far, visca_focus_near});
	if (zoom_ends.first == zoom_ends.second)
		zoom_ends.second = zoom_ends.first + 1;
	if (focus_ends.first == focus_ends.second)
		focus_ends.second = focus_ends.first + 1;
	/* what the last reads were of, in the units the ranges were then */
	stale_state += "pan_pos";
	stale_state += "zoom_pos";
	stale_state += "focus_pos";
}

/* Turn what a reply read back from the camera into the position state, in
 * the units of the movement API. The scales are the ones the absolute move
 * commands use, so a position that is shown is one that can be moved back
 * to. Every camera has its own range, so these are the plugin's
 * assumptions, and clamped. */
void PTZVisca::update_position(OBSData decoded)
{
	auto has = [decoded](const char *key) {
		return obs_data_has_user_value(decoded, key);
	};
	if (has("pan_pos"))
		setPosition("pan", visca_to_unit((int)obs_data_get_int(state, "pan_pos"), pan_ends));
	if (has("tilt_pos"))
		setPosition("tilt", visca_to_unit((int)obs_data_get_int(state, "tilt_pos"), tilt_ends));
	if (has("zoom_pos"))
		setPosition("zoom", visca_to_span((int)obs_data_get_int(state, "zoom_pos"), zoom_ends));
	if (has("focus_pos"))
		setPosition("focus", visca_to_span((int)obs_data_get_int(state, "focus_pos"), focus_ends));
}

void PTZVisca::send(PTZCmd cmd)
{
	pending_cmds.append(cmd);
	send_pending();
}

void PTZVisca::send(PTZCmd cmd, QList<int> args)
{
	cmd.encode(args);
	send(cmd);
}

/* Queues the command the camera has for an action, if it has one */
void PTZVisca::queue_action(const QString &name, QList<int> args)
{
	auto action = profile->actions.constFind(name);
	if (action == profile->actions.constEnd())
		return;
	PTZCmd cmd = *action;
	cmd.encode(args);
	pending_cmds += cmd;
}

void PTZVisca::send_action(const QString &name, QList<int> args)
{
	queue_action(name, args);
	send_pending();
}

void PTZVisca::send_packet(const QByteArray &packet)
{
	ptz_debug_trace("--> %s", packet.toHex(':').data());
	incrementStatistic("visca_sent_count");
	/* as opposed to the inquiries the device sends on its own, to see what
	 * the camera is doing */
	if (packet.size() > 1 && packet[1] != 0x09)
		incrementStatistic("visca_command_count");
	send_immediate(packet);
	timeout_timer.setSingleShot(true);
	timeout_timer.start(VISCA_REPLY_TIMEOUT_MS);
}

void PTZVisca::timeout()
{
	/* Only a request waiting for its first reply can time out. An ACKed
	 * command is waiting on its completion, which can take seconds. */
	if (!active_cmd[0].has_value())
		return;
	if (isConnected() && active_cmd[0].has_value() && (timeout_retry < 3)) {
		send_packet(active_cmd[0].value().cmd);
		timeout_retry++;
	} else {
		setConnected(false);
		active_cmd[0] = std::nullopt;
		send_pending();
	}
}

/* One property for each of the command set's different inquiries, in the order
 * they are polled in. The reply to an inquiry marks all of the properties it
 * reads as up to date, so asking for one of them is asking for them all. */
QStringList PTZVisca::inquiry_poll_list() const
{
	QStringList props;
	QSet<QByteArray> seen;
	for (const auto &control : profile->controls) {
		if (control.reads.isEmpty() || seen.contains(control.reads.first().cmd))
			continue;
		seen.insert(control.reads.first().cmd);
		props += control.key;
	}
	return props;
}

void PTZVisca::update_timer_callback()
{
	/* The camera can be moved by something other than this plugin (an IR
	 * remote, another controller), and tells nobody when it is, so the
	 * position is read again on every tick, moving or not. */
	stale_state += "pan_pos";
	stale_state += "zoom_pos";
	stale_state += "focus_pos";

	/* The rest of what is known about the camera is read again too, but
	 * slowly: one inquiry a tick (a block inquiry reads many properties),
	 * and not while the camera is being moved, so the controls never wait
	 * behind more than the one request on the wire. */
	if (!pan_speed && !tilt_speed && !zoom_speed && !focus_speed && pending_cmds.isEmpty()) {
		const QStringList props = inquiry_poll_list();
		if (!props.isEmpty())
			stale_state += props[poll_next++ % props.size()];
	}
	discover_tick();
	send_pending();
}

/* What discover_limits() drives, in order, each to one end of its travel and
 * then the other: the position it watches, and the speed to drive it at */
struct DiscoverAxis {
	const char *pos;
	double dir;
};
static const DiscoverAxis visca_discover_steps[] = {
	{"pan_pos", -1},  {"pan_pos", 1},  {"tilt_pos", -1},  {"tilt_pos", 1},
	{"zoom_pos", -1}, {"zoom_pos", 1}, {"focus_pos", -1}, {"focus_pos", 1},
};
static constexpr int VISCA_DISCOVER_STEPS = sizeof(visca_discover_steps) / sizeof(visca_discover_steps[0]);

/* Finds how far the camera goes by driving each axis to both of its ends at
 * full speed, watching (on the update timer's reads of the position) where it
 * stops, then putting it back where it was. This moves the camera all over,
 * so it only runs when asked for. The ranges the camera was found to have
 * are kept as settings. The camera won't take focus commands while it is
 * focusing by itself, so autofocus is turned off for the duration, and back
 * on after. */
void PTZVisca::discover_limits()
{
	if (discover_step >= 0 || !isConnected())
		return;
	ptz_info("discovering movement limits");
	discover_extent.clear();
	discover_start.clear();
	for (auto key : {"pan_pos", "tilt_pos", "zoom_pos", "focus_pos"}) {
		if (!obs_data_has_user_value(state, key))
			continue;
		int pos = (int)obs_data_get_int(state, key);
		if (!strcmp(key, "focus_pos"))
			pos &= 0xffff;
		discover_start[key] = pos;
		discover_extent[key] = {pos, pos};
	}
	discover_af_was_on = obs_data_get_bool(state, "focus_af_enabled");
	if (discover_af_was_on)
		set_autofocus(false);
	discover_step = 0;
	discover_begin_step();
}

void PTZVisca::discover_begin_step()
{
	while (discover_step < VISCA_DISCOVER_STEPS) {
		const DiscoverAxis &axis = visca_discover_steps[discover_step];
		if (discover_start.contains(axis.pos))
			break;
		discover_step++;
	}
	if (discover_step >= VISCA_DISCOVER_STEPS) {
		discover_finish();
		return;
	}
	const DiscoverAxis &axis = visca_discover_steps[discover_step];
	discover_ticks = 0;
	discover_stable = 0;
	discover_last = (int)obs_data_get_int(state, axis.pos);
	if (!strcmp(axis.pos, "focus_pos"))
		discover_last &= 0xffff;
	/* pantilt() and friends apply the axis' inversion, which only
	 * swaps which end is first */
	if (!strcmp(axis.pos, "pan_pos"))
		pantilt(axis.dir, 0);
	else if (!strcmp(axis.pos, "tilt_pos"))
		pantilt(0, axis.dir);
	else if (!strcmp(axis.pos, "zoom_pos"))
		zoom(axis.dir);
	else
		focus(axis.dir);
}

void PTZVisca::discover_tick()
{
	if (discover_step < 0)
		return;
	const DiscoverAxis &axis = visca_discover_steps[discover_step];
	int pos = (int)obs_data_get_int(state, axis.pos);
	/* focus is unsigned, though a block inquiry reads it as if it weren't */
	if (!strcmp(axis.pos, "focus_pos"))
		pos &= 0xffff;
	auto &extent = discover_extent[axis.pos];
	extent.first = std::min(extent.first, pos);
	extent.second = std::max(extent.second, pos);

	/* The first read after a drive command may be from before the camera
	 * started, so it isn't stopped until it has been seen to stay put */
	discover_ticks++;
	discover_stable = (discover_ticks > 1 && pos == discover_last) ? discover_stable + 1 : 0;
	discover_last = pos;
	if (discover_stable >= VISCA_DISCOVER_STABLE_TICKS || discover_ticks >= VISCA_DISCOVER_MAX_TICKS ||
	    !isConnected()) {
		stop();
		discover_step++;
		discover_begin_step();
	}
}

void PTZVisca::discover_finish()
{
	discover_step = -1;
	stop();
	auto extent = [this](const char *key, int &lo, int &hi) {
		if (!discover_extent.contains(key) || discover_extent[key].first == discover_extent[key].second)
			return false;
		lo = discover_extent[key].first;
		hi = discover_extent[key].second;
		return true;
	};
	int lo, hi;
	if (extent("pan_pos", lo, hi))
		visca_pan_range = std::max(std::abs(lo), std::abs(hi));
	if (extent("tilt_pos", lo, hi))
		visca_tilt_range = std::max(std::abs(lo), std::abs(hi));
	if (extent("zoom_pos", lo, hi))
		visca_zoom_range = std::max(1, hi);
	bool focused = extent("focus_pos", lo, hi);
	if (focused) {
		visca_focus_far = lo;
		visca_focus_near = hi;
	}
	ptz_info("movement limits: pan %d tilt %d zoom %d focus %d to %d", visca_pan_range, visca_tilt_range,
		 visca_zoom_range, visca_focus_far, visca_focus_near);
	/* what it found, not what the command set says */
	visca_ranges_auto = false;
	apply_ranges();

	/* Back where it was, in the new ranges */
	auto start = [this](const char *key) {
		return discover_start.value(key, 0);
	};
	if (discover_start.contains("pan_pos") && discover_start.contains("tilt_pos"))
		pantilt_abs(visca_to_unit(start("pan_pos"), pan_ends), visca_to_unit(start("tilt_pos"), tilt_ends));
	if (focused && discover_start.contains("zoom_pos"))
		send_action("zoom_focus_abs", {start("zoom_pos"), start("focus_pos") & 0xffff});
	else if (discover_start.contains("zoom_pos"))
		zoom_abs(visca_to_span(start("zoom_pos"), zoom_ends));

	if (discover_af_was_on)
		set_autofocus(true);
	discover_af_was_on = false;
	announceSettingsChanged();
}

/* Everything the camera can be asked for is to be read again */
void PTZVisca::mark_all_stale()
{
	for (const auto &control : profile->controls) {
		if (!control.reads.isEmpty())
			stale_state += control.key;
	}
}

/* Switches the camera to another command set. What it didn't have in the
 * last one, it may have in this one, and what is known about it may have
 * been read with what this one reads differently: start again. */
void PTZVisca::set_profile(std::shared_ptr<const ViscaProfile> new_profile)
{
	if (!new_profile || new_profile == profile)
		return;
	ptz_info("using the %s command set", QT_TO_UTF8(new_profile->id));
	profile = new_profile;
	apply_ranges();
	unsupported_requests.clear();
	mark_all_stale();
	featuresChanged();
}

/* What the command set has the commands for */
PTZDevice::Features PTZVisca::features() const
{
	Features features = Diagnostics;
	const QList<QPair<const char *, Feature>> actions = {
		{"pantilt_drive", PanTilt},  {"pantilt_abs", PanTiltAbs},
		{"pantilt_rel", PanTiltRel}, {"pantilt_home", Home},
		{"zoom_drive", Zoom},        {"zoom_abs", ZoomAbs},
		{"focus_drive", Focus},      {"focus_onetouch", FocusOneTouch},
		{"memory_recall", Presets},
	};
	for (const auto &[action, feature] : actions) {
		if (profile->actions.contains(action))
			features |= feature;
	}
	auto settable = [this](const char *key) {
		const ViscaControl *control = profile->control(key);
		return control && control->settable();
	};
	if (settable("focus_af_enabled"))
		features |= AutoFocus;
	if (settable("power_on"))
		features |= Power;
	if (profile->triggers.contains("wb_onepush"))
		features |= WhiteBalanceOnePush;
	return features;
}

/* The command set the setting asks for, or the one for the camera's model,
 * once it has said what it is */
void PTZVisca::choose_profile()
{
	if (profile_setting != "auto") {
		auto chosen = visca_profile(profile_setting);
		if (!chosen)
			ptz_log(LOG_WARNING, "no %s command set, using the generic one", QT_TO_UTF8(profile_setting));
		set_profile(chosen ? chosen : visca_generic_profile());
	} else if (obs_data_has_user_value(state, "vendor_id") && obs_data_has_user_value(state, "model_id")) {
		set_profile(visca_profile_for_model((int)obs_data_get_int(state, "vendor_id"),
						    (int)obs_data_get_int(state, "model_id")));
	} else {
		set_profile(visca_generic_profile());
	}
}

void PTZVisca::cmd_get_camera_info()
{
	setConnected(true);
	mark_all_stale();
	update_timer.start(1000);
	send_pending();
}

/* The state a command sets that can't be read back is what it was set to,
 * from when the camera ACKs the command: some cameras never say it has
 * completed. */
void PTZVisca::apply_assumed(const PTZCmd &cmd)
{
	if (cmd.assumes.isEmpty())
		return;
	OBSData assumed = variantMapToOBSData(cmd.assumes);
	obs_data_apply(state, assumed);
	obs_data_apply(stateChanged, assumed);
	notifyStateChanged();
}

void PTZVisca::receive(const QByteArray &msg)
{
	if (VISCA_PACKET_SENDER(msg) != address || (msg.size() < 3))
		return;
	ptz_debug_trace("<-- %s", msg.toHex(':').data());
	incrementStatistic("visca_recv_count");
	int slot = msg[1] & 0x7;
	QByteArray inq;
	since_last_rx.start();
	link_answered = true;

	switch (msg[1] & 0xf0) {
	case VISCA_RESPONSE_ACK:
		setConnected(true);
		if (active_cmd[0].has_value())
			apply_assumed(*active_cmd[0]);
		busy_retries = 0;
		busy_backoff_ms = 0;
		if (slot != 0) {
			active_cmd[slot] = active_cmd[0];
			active_cmd[0] = std::nullopt;
		}
		break;
	case VISCA_RESPONSE_COMPLETED:
		setConnected(true);
		busy_retries = 0;
		busy_backoff_ms = 0;
		if (!active_cmd[slot].has_value()) {
			if (active_cmd[0].has_value()) {
				// Slot is empty, but some cameras reply without an ack first. Handle that case
				active_cmd[slot] = active_cmd[0];
				active_cmd[0] = std::nullopt;
			} else {
				ptz_debug("spurious reply: %s", msg.toHex(':').data());
				break;
			}
		}

		/* What the command changed is only where it is going until it
		 * has completed, so read it again now */
		for (const auto &key : active_cmd[slot]->affects)
			stale_state += key;

		apply_assumed(*active_cmd[slot]);

		/* Log Inquiry Replies */
		inq = active_cmd[slot]->cmd;
		if (active_cmd[slot]->isInquiry()) {
			replyLast[inq] = msg;
			replyCount[inq]++;
		}

		/* Slot 0 responses are inquiries that need to be parsed */
		if (slot == 0 && msg.size() > 3) {
			/* Some devices (e.g. cicso) don't use slots and
			 * commands complete immediately. Only decode
			 * response if the payload size is non-zero */
			obs_data_t *rslt_props = active_cmd[0].value().decode(msg);
			obs_data_apply(state, rslt_props);
			obs_data_apply(stateChanged, rslt_props);
			update_position(rslt_props);

			/* The camera says what it is: use its command set */
			if (obs_data_has_user_value(rslt_props, "model_id"))
				choose_profile();

			/* Mark returned properties as clean */
			for (auto item = obs_data_first(rslt_props); item; obs_data_item_next(&item))
				stale_state -= obs_data_item_get_name(item);

			/* Data has been updated */
			notifyStateChanged();
			obs_data_release(rslt_props);
		}

		active_cmd[slot] = std::nullopt;
		break;
	case VISCA_RESPONSE_ERROR:
		timeout_timer.stop();
		/* A camera has only a couple of command sockets; when they are
		 * all busy it answers "command buffer full" to anything sent,
		 * inquiries included. That is not a failure: send the request
		 * again once it has had a moment to free one up. */
		if (slot == 0 && msg.size() > 2 && msg[2] == VISCA_ERROR_BUFFER_FULL && active_cmd[0].has_value() &&
		    busy_retries < VISCA_BUSY_RETRIES_MAX) {
			busy_retries++;
			busy_backoff_ms = VISCA_BUSY_BACKOFF_MS;
			pending_cmds.prepend(active_cmd[0].value());
			ptz_debug("rx busy, retrying: %s", msg.toHex(':').data());
			active_cmd[0] = std::nullopt;
			break;
		}
		/* This command failed, don't generate it again */
		if (active_cmd[0].has_value()) {
			for (auto rslt : active_cmd[0].value().results)
				stale_state -= rslt->name;
			/* An inquiry the camera doesn't have: read what it
			 * covers with the next inquiry for each, if there is one */
			if (!active_cmd[0]->assumes.isEmpty() && msg.size() > 2 && msg[2] == VISCA_ERROR_SYNTAX)
				unsupported_requests.insert(active_cmd[0]->cmd);
			if (active_cmd[0]->isInquiry() && msg.size() > 2 && msg[2] == VISCA_ERROR_SYNTAX) {
				const QByteArray unsupported = active_cmd[0]->cmd;
				unsupported_requests.insert(unsupported);
				for (const auto &control : profile->controls) {
					for (const auto &read : control.reads) {
						if (read.cmd == unsupported)
							stale_state += control.key;
					}
				}
			}
		}
		ptz_debug("rx error: %s", msg.toHex(':').data());
		active_cmd[0] = std::nullopt;
		active_cmd[slot] = std::nullopt;
		break;
	default:
		ptz_debug("rx unknown: %s", msg.toHex(':').data());
		break;
	}
	/* The timer guards the request in slot 0; once that has been answered
	 * (ACK, completion, or error) there is nothing left to time out. */
	if (!active_cmd[0].has_value())
		timeout_timer.stop();
	powerOnAtStartup();
	send_pending();
}

/* An on/off value can be asked for as a number, and a number as a bool, by
 * whoever doesn't know which it is */
static int visca_value(obs_data_t *data, const char *key)
{
	obs_data_item_t *item = obs_data_item_byname(data, key);
	if (!item)
		return 0;
	int value = obs_data_item_gettype(item) == OBS_DATA_BOOLEAN ? obs_data_item_get_bool(item)
								    : (int)obs_data_item_get_int(item);
	obs_data_item_release(&item);
	return value;
}

void PTZVisca::requestState(OBSData requested)
{
	/* What is left for PTZDevice to act on */
	OBSDataAutoRelease rest = obs_data_create();
	obs_data_apply(rest, requested);
	/* A command that sets more than one value is sent once for them all */
	QSet<QByteArray> sent;
	for (const auto &control : profile->controls) {
		if (!control.settable() || !obs_data_has_user_value(requested, QT_TO_UTF8(control.key)))
			continue;
		obs_data_erase(rest, QT_TO_UTF8(control.key));
		if (control.set && control.set->args.size() > 1) {
			if (sent.contains(control.set->cmd))
				continue;
			sent += control.set->cmd;
		}
		set_control(control, visca_value(requested, QT_TO_UTF8(control.key)), requested);
	}
	PTZDevice::requestState(rest.Get());
}

/* What a state value will be once the commands waiting to be sent, or for
 * the camera to take them, have been taken: what the last of them that says
 * it sets it does, or else what it is */
int PTZVisca::expected_value(const char *key) const
{
	for (auto cmd = pending_cmds.crbegin(); cmd != pending_cmds.crend(); cmd++) {
		if (cmd->assumes.contains(key))
			return cmd->assumes.value(key).toInt();
	}
	for (const auto &cmd : active_cmd) {
		if (cmd && cmd->assumes.contains(key))
			return cmd->assumes.value(key).toInt();
	}
	return visca_value(state, key);
}

/* Sends what sets a state value to `value`, if it can be set to that. Not
 * if the camera has said it doesn't have the command. A command that sets
 * other values too sets them to what is asked for in `requested`, or keeps
 * them where the camera has them. A value that nothing reads is what it was
 * last set to, as a tally lamp's is. */
void PTZVisca::set_control(const ViscaControl &control, int value, OBSData requested)
{
	if (control.set) {
		PTZCmd cmd = *control.set;
		QList<int> args;
		for (const auto &field : cmd.args) {
			if (control.key == field->name)
				args += value;
			else if (requested && obs_data_has_user_value(requested, field->name))
				args += visca_value(requested, field->name);
			else
				args += expected_value(field->name);
			if (control.reads.isEmpty())
				cmd.assumes.insert(field->name, field->isBool() ? QVariant(args.last() != 0)
										: QVariant(args.last()));
		}
		send(cmd, args);
	} else if (control.setTo.contains(value) && !unsupported_requests.contains(control.setTo.constFind(value)->cmd))
		send(*control.setTo.constFind(value));
}

void PTZVisca::set_control(const QString &key, int value)
{
	if (const ViscaControl *control = profile->control(key))
		set_control(*control, value);
}

/* Lights the red tally lamp while the source is in the program scene, and
 * the green one while it is in the preview scene and not in the program
 * scene, unless "tally_auto" is off for whoever already drives them another
 * way. A manual request (requestState() above) still goes through, but the
 * next time the source changes scene, this overrides it again. */
void PTZVisca::onSceneChanged()
{
	bool was_red = live, was_green = preview && !live;
	PTZDevice::onSceneChanged();
	if (!tally_auto)
		return;
	if (live != was_red)
		set_control("tally_on", live);
	if ((preview && !live) != was_green)
		set_control("tally_preview", preview && !live);
}

/* Powers the camera on once OBS itself has finished loading, for anyone
 * who'd rather it not sit in standby whenever OBS isn't the one running it.
 * The link to the camera may not be up yet: a TCP connection can still be
 * being made, or a host name looked up, and what is sent then goes nowhere.
 * So wait for the camera to answer something, for up to
 * VISCA_POWER_ON_WAIT_MS. */
void PTZVisca::onOBSStartup()
{
	if (!power_on_at_startup)
		return;
	power_on_pending = true;
	power_on_requested.start();
	if (link_answered)
		powerOnAtStartup();
}

void PTZVisca::powerOnAtStartup()
{
	if (!power_on_pending)
		return;
	power_on_pending = false;
	if (power_on_requested.elapsed() < VISCA_POWER_ON_WAIT_MS)
		set_control("power_on", 1);
}

/* Sent as OBS is closing, with the transport about to go away along with
 * everything else, so it can't go through the queue, which sends when the
 * camera has answered what is in front of it and the event loop has run.
 * Send it now. Best-effort still: nothing waits for the camera to take it,
 * and a serial port has no way to be flushed. */
void PTZVisca::onOBSShutdown()
{
	if (!power_off_at_shutdown)
		return;
	const ViscaControl *power = profile->control("power_on");
	if (!power || !power->set)
		return;
	PTZCmd cmd = *power->set;
	cmd.encode({0});
	send_immediate(cmd.cmd);
	if (transport)
		transport->flush();
}

bool PTZVisca::runTrigger(const QString &name)
{
	if (profile->triggers.contains(name))
		send(*profile->triggers.constFind(name));
	/* Diagnostics, for working out what a camera supports */
	else if (name == "scan_inquiries")
		scan_commands();
	else if (name == "replies_to_log")
		write_replies_to_log();
	else if (name == "discover_limits")
		discover_limits();
	else
		return PTZDevice::runTrigger(name);
	return true;
}

void PTZVisca::send_pending()
{
	if (active_cmd[0].has_value())
		return;

	/* Give the camera time to settle after its last reply before sending
	 * the next request; it may drop the request otherwise */
	if (transport && since_last_rx.isValid()) {
		int wait = std::max(transport->minRequestGapMs(), busy_backoff_ms) - (int)since_last_rx.elapsed();
		if (wait > 0) {
			if (!gap_timer.isActive())
				gap_timer.start(wait);
			return;
		}
	}

	if (pending_cmds.isEmpty()) {
		if (pantilt_changed) {
			pantilt_changed = false;
			int p = scale_speed(pan_speed, visca_pan_speed_max);
			int t = -scale_speed(tilt_speed, visca_tilt_speed_max);
			queue_action("pantilt_drive", {p, t});
		} else if (zoom_changed) {
			zoom_changed = false;
			queue_action("zoom_drive", {scale_speed(zoom_speed, visca_zoom_speed_max + 1)});
		} else if (focus_changed) {
			focus_changed = false;
			queue_action("focus_drive", {scale_speed(focus_speed, visca_focus_speed_max + 1)});
		} else if (isConnected()) {
			/* What the camera is decides what else to ask it for. One
			 * that can't be asked for everything in standby is asked
			 * whether it is first, and only for what it can be while
			 * it is; the rest waits for it to wake. */
			const auto &standby_reads = profile->standby_reads;
			bool standby = standby_reads && obs_data_has_user_value(state, "power_on") &&
				       !obs_data_get_bool(state, "power_on");
			QStringList stale = stale_state.values();
			if (standby_reads && stale_state.contains("power_on"))
				stale.prepend("power_on");
			if (stale_state.contains("vendor_id"))
				stale.prepend("vendor_id");
			for (const QString &prop : stale) {
				if (standby && !standby_reads->contains(prop))
					continue;
				const ViscaControl *control = profile->control(prop);
				if (!control || control->reads.isEmpty())
					continue;
				const PTZInq *inq = nullptr;
				for (const auto &read : control->reads) {
					if (!unsupported_requests.contains(read.cmd)) {
						inq = &read;
						break;
					}
				}
				if (!inq) {
					/* Nothing to read it with */
					stale_state -= prop;
					continue;
				}
				pending_cmds += *inq;
				break;
			}
		}
	}

	if (pending_cmds.isEmpty())
		return;

	active_cmd[0] = pending_cmds.takeFirst();
	for (const auto &key : active_cmd[0]->affects)
		stale_state += key;
	send_packet(active_cmd[0].value().cmd);
	timeout_retry = 0;
}

void PTZVisca::do_update(void)
{
	send_pending();
}

void PTZVisca::pantilt_rel(double pan_, double tilt_)
{
	/* 1.0 is all the way from one end to the other */
	int pan = std::clamp(pan_, -1.0, 1.0) * (pan_ends.second - pan_ends.first);
	int tilt = std::clamp(tilt_, -1.0, 1.0) * (tilt_ends.second - tilt_ends.first);
	send_action("pantilt_rel", {0x14, 0x14, pan, tilt});
}

void PTZVisca::pantilt_abs(double pan_, double tilt_)
{
	send_action("pantilt_abs", {0x0f, 0x0f, visca_from_unit(pan_, pan_ends), visca_from_unit(tilt_, tilt_ends)});
}

void PTZVisca::pantilt_home()
{
	send_action("pantilt_home");
}

void PTZVisca::zoom_abs(double pos_)
{
	send_action("zoom_abs", {visca_from_span(pos_, zoom_ends)});
}

void PTZVisca::set_autofocus(bool enabled)
{
	set_control("focus_af_enabled", enabled);
}

void PTZVisca::focus_onetouch()
{
	send_action("focus_onetouch");
}

void PTZVisca::memory_reset(int i)
{
	send_action("memory_reset", {i});
}

void PTZVisca::memory_set(int i)
{
	send_action("memory_set", {i});
}

void PTZVisca::memory_recall(int i)
{
	send_action("memory_recall", {i});
}

void ptz_visca_register_filter()
{
	struct obs_source_info info = {};
	info.id = "ca.secretlab.obs-ptz.visca";
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_DO_NOT_DUPLICATE;
	info.get_name = [](void *) -> const char * {
		return "VISCA PTZ Control";
	};
	info.create = [](obs_data_t *settings, obs_source_t *source) -> void * {
		return ptz_filter_create([&]() -> PTZDevice * { return new PTZVisca(settings, source); });
	};
	info.destroy = ptz_filter_destroy;
	info.get_defaults = [](obs_data_t *settings) {
		obs_data_set_default_string(settings, "type", "visca-over-ip");
		PTZVisca::defaults(settings);
	};
	info.get_properties = ptz_filter_get_properties;
	info.update = ptz_filter_update;
	info.save = ptz_filter_save;
	info.filter_remove = ptz_filter_remove;
	info.icon_type = OBS_ICON_TYPE_CAMERA;
	info.filter_add = ptz_filter_add;
	obs_register_source(&info);
}
