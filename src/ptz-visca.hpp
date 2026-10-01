/* Pan Tilt Zoom camera instance
 *
 * Copyright 2020 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */
#pragma once

#include <optional>
#include <QObject>
#include <QTimer>
#include <QElapsedTimer>
#include "protocol-helpers.hpp"
#include "ptz-device.hpp"

#define VISCA_RESPONSE_ADDRESS 0x30
#define VISCA_RESPONSE_ACK 0x40
#define VISCA_RESPONSE_COMPLETED 0x50
#define VISCA_RESPONSE_ERROR 0x60
#define VISCA_PACKET_SENDER(pkt) ((unsigned)((pkt)[0] & 0x70) >> 4)

extern const PTZCmd VISCA_ENUMERATE;

/* How far a camera moves and zooms, as the plugin assumes it until told
 * otherwise (the "visca_*_range" settings, or discover_limits()): what 1.0
 * in the movement API stands for. Pan and tilt are symmetric about the
 * centre, zoom runs from 0 (wide) to its range. Focus is 0x1000 at far
 * focus and 0xf000 at near, as VISCA cameras usually have it. */
#define VISCA_DEFAULT_PAN_RANGE 0x1400
#define VISCA_DEFAULT_TILT_RANGE 0x500
#define VISCA_DEFAULT_ZOOM_RANGE 0x7ac0
#define VISCA_DEFAULT_FOCUS_FAR 0x1000
#define VISCA_DEFAULT_FOCUS_NEAR 0xf000

/*
 * Abstract transport that carries VISCA datagrams to and from a camera.
 * The wire protocol (command encoding, ack/completion handling, etc) is
 * identical no matter how the bytes get to the camera, so PTZVisca owns
 * exactly one ViscaTransport at a time and everything protocol-specific
 * lives here. Which concrete transport is instantiated is derived from
 * the device's "type" (visca/visca-over-ip/visca-over-tcp, see
 * PTZVisca::setInterface()), which is what lets a single PTZVisca class
 * represent a serial, UDP, or TCP connected camera.
 */
class ViscaTransport : public QObject {
	Q_OBJECT

public:
	~ViscaTransport() override = default;

	/* "address" is the VISCA bus/camera address; it is owned by PTZVisca
	 * (shared with its receive-side filtering) rather than duplicated
	 * here, so transports that need it (serial) take it as a parameter */
	virtual QString description(unsigned int address) const = 0;
	virtual void update(OBSData config) = 0;
	virtual void save(OBSData config) const = 0;
	virtual void send(const QByteArray &msg, unsigned int address) = 0;
	/* Least time, in ms, the camera needs between its last reply and the
	 * next request. Some cameras silently drop a request that comes too
	 * soon after a reply. */
	virtual int minRequestGapMs() const { return 0; }
	/* Gets what has been sent on its way now, rather than when the event
	 * loop next runs, for a last message before the transport goes away */
	virtual void flush() {}
	/* The host the device's parent source reports ("" for none), used when
	 * the transport isn't configured with a host of its own. */
	virtual void setSourceHost(const QString &host) { Q_UNUSED(host); }

signals:
	/* A decoded VISCA reply datagram, with transport framing removed */
	void receive(const QByteArray &msg);
	/* The link was (re-)established; camera state should be re-queried
	 * from scratch */
	void reset();
	/* The link is known-good and pending inquiries can resume, without
	 * needing a full reset */
	void refresh();
	void statIncrement(const QString &name);
};

/*
 * VISCA camera instance. A single PTZVisca object represents the camera
 * regardless of transport; see ViscaTransport above for how serial/UDP/TCP
 * are selected.
 */
class PTZVisca : public PTZDevice {
	Q_OBJECT

public:
	static const QMap<int, std::string> viscaVendors;
	static const QMap<int, std::string> viscaModels;
	static const QMap<QString, PTZInq> inquires;
	static const QMap<QString, PTZInq> inquiresFallback;

protected:
	unsigned int timeout_retry = 0;
	unsigned int busy_retries = 0;
	int busy_backoff_ms = 0;
	unsigned int address = 1;
	bool protocol_trace = false;
	/* Inquiries, and tally lamp commands, the camera answered with a syntax
	 * error: it doesn't have them */
	QSet<QByteArray> unsupported_requests;
	QMap<QByteArray, QByteArray> replyLast;
	QMap<QByteArray, int> replyCount;
	QList<PTZCmd> pending_cmds;
	std::optional<PTZCmd> active_cmd[8];
	QTimer timeout_timer;
	QTimer gap_timer;
	QElapsedTimer since_last_rx;
	QTimer update_timer;
	unsigned int poll_next = 0;
	static const QStringList &inquiry_poll_list();

	QString visca_interface;
	ViscaTransport *transport = nullptr;
	void setInterface(const QString &interface);

	int visca_pan_range = VISCA_DEFAULT_PAN_RANGE;
	int visca_tilt_range = VISCA_DEFAULT_TILT_RANGE;
	int visca_zoom_range = VISCA_DEFAULT_ZOOM_RANGE;
	int visca_focus_far = VISCA_DEFAULT_FOCUS_FAR;
	int visca_focus_near = VISCA_DEFAULT_FOCUS_NEAR;

	/* Finding out how far the camera goes, by driving each axis to both of
	 * its ends and watching where it stops; see discover_limits(). A step
	 * is one axis in one direction, -1 when none is running. */
	int discover_step = -1;
	int discover_ticks = 0;
	int discover_stable = 0;
	int discover_last = 0;
	bool discover_af_was_on = false;
	QMap<QString, QPair<int, int>> discover_extent;
	QMap<QString, int> discover_start;
	void discover_limits();
	void discover_begin_step();
	void discover_tick();
	void discover_finish();

	unsigned int visca_pan_speed_max = 0x18;
	unsigned int visca_tilt_speed_max = 0x14;
	unsigned int visca_zoom_speed_max = 7;
	unsigned int visca_focus_speed_max = 7;
	bool tally_auto = true;
	bool power_on_at_startup = false;
	bool power_off_at_shutdown = false;
	/* Whether the camera has answered since the link came up, and a
	 * power-on that is waiting for it to (see onOBSStartup()) */
	bool link_answered = false;
	bool power_on_pending = false;
	QElapsedTimer power_on_requested;
	void powerOnAtStartup();

	void sendTally(bool green, bool on);
	void update_tally_state(const QByteArray &cmd);
	bool send_pantilt();
	void send_immediate(const QByteArray &msg);
	void send_packet(const QByteArray &msg);
	void send(PTZCmd cmd);
	void send(PTZCmd cmd, QList<int> args);
	void send_pending();
	void timeout();
	void update_timer_callback();
	void scan_commands();
	void write_replies_to_log();
	void update_position(OBSData decoded);
	void reset();

protected slots:
	void receive(const QByteArray &msg);
	bool runTrigger(const QString &name) override;

public:
	PTZVisca(OBSData config, obs_source_t *source = nullptr);
	QString description() const override;
	obs_properties_t *get_obs_properties() override;
	bool supportsDiagnostics() const override { return true; }
	void requestState(OBSData requested) override;
	void onSceneChanged() override;
	void onOBSStartup() override;
	void onOBSShutdown() override;

	static void defaults(obs_data_t *config);
	void update(OBSData config) override;
	void save(OBSData config) const override;

	void cmd_get_camera_info();

	void do_update() override;
	void onParentHostChanged(const QString &host) override;
	void pantilt_rel(double pan, double tilt) override;
	void pantilt_abs(double pan, double tilt) override;
	void pantilt_home() override;
	void zoom_abs(double pos) override;
	void set_autofocus(bool enabled) override;
	void focus_onetouch() override;
	void memory_reset(int i) override;
	void memory_set(int i) override;
	void memory_recall(int i) override;
};

void ptz_visca_register_filter();
