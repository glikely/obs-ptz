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
#include "protocol-helpers.hpp"
#include "ptz-device.hpp"

#define VISCA_RESPONSE_ADDRESS 0x30
#define VISCA_RESPONSE_ACK 0x40
#define VISCA_RESPONSE_COMPLETED 0x50
#define VISCA_RESPONSE_ERROR 0x60
#define VISCA_PACKET_SENDER(pkt) ((unsigned)((pkt)[0] & 0x70) >> 4)

extern const PTZCmd VISCA_ENUMERATE;

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

protected:
	unsigned int timeout_retry = 0;
	unsigned int address = 1;
	bool protocol_trace = false;
	QMap<QByteArray, QByteArray> replyLast;
	QMap<QByteArray, int> replyCount;
	QList<PTZCmd> pending_cmds;
	std::optional<PTZCmd> active_cmd[8];
	QTimer timeout_timer;
	QTimer update_timer;

	QString visca_interface;
	ViscaTransport *transport = nullptr;
	void setInterface(const QString &interface);

	unsigned int visca_pan_speed_max = 0x18;
	unsigned int visca_tilt_speed_max = 0x14;
	unsigned int visca_zoom_speed_max = 7;
	unsigned int visca_focus_speed_max = 7;

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
	void reset();

protected slots:
	void receive(const QByteArray &msg);
	void get(calldata_t *cd) const override;
	void set(calldata_t *cd) override;

public:
	PTZVisca(OBSData config);
	QString description() override;
	obs_properties_t *get_obs_properties() override;

	void getDefaults(OBSData config) const override;
	void update(OBSData config) override;
	void save(OBSData config) const override;

	void cmd_get_camera_info();

	void do_update() override;
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
