/* Pan Tilt Zoom Controls - Qt list model for managing devices
 *
 * Copyright 2020-2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */
#pragma once

#include <QObject>
#include <QAbstractItemModel>
#include <QHash>
#include <QList>
#include "ptz.h"

/**
 * PTZListModel never holds a PTZDevice* (see AGENTS.md / the decoupling
 * design in ptz-device.cpp): every device it knows about is represented
 * purely by its device_id plus the proc_handler_t/signal_handler_t pair
 * handed over on the "ptz_device_create" signal, and a local cache of the
 * fields QAbstractItemModel::data() needs to stay synchronous. All control
 * goes out through proc_handler_call(); the cache is kept in sync purely by
 * signal_handler notifications (see PTZListModel()'s constructor/the *_cb
 * trampolines in ptz-list-model.cpp).
 */
class PTZListModel : public QAbstractItemModel {
	Q_OBJECT

public:
	struct PresetEntry {
		int id = 0;
		QString name;
		QString token;
	};

private:
	struct PTZDeviceEntry {
		uint32_t id = 0;
		proc_handler_t *ph = nullptr;
		signal_handler_t *sh = nullptr;
		QString name;
		QString description;
		bool connected = false;
		bool live = false;
		bool preview = false;
		bool locked = false;
		bool supportsSetHome = false;
		int maxPresets = 16;
		QList<PresetEntry> presets;
	};

	QList<PTZDeviceEntry> devices;
	QHash<uint32_t, int> rowByDeviceId;

	void rebuildRowIndex();
	PTZDeviceEntry *entryAt(int row);
	const PTZDeviceEntry *entryAt(int row) const;
	PTZDeviceEntry *entryAt(const QModelIndex &index);
	const PTZDeviceEntry *entryAt(const QModelIndex &index) const;
	PTZDeviceEntry *entryById(uint32_t device_id);
	const PTZDeviceEntry *entryById(uint32_t device_id) const;
	void refreshDeviceState(PTZDeviceEntry *entry);
	void refreshPresetList(PTZDeviceEntry *entry);

public:
	enum PTZListModelRole {
		DeviceIdRole = Qt::UserRole,
		DescriptionRole,
		IsLiveRole,
		IsPreviewRole,
		IsConnectedRole,
		IsLockedRole,
		SupportsSetHomeRole,
	};

	PTZListModel();
	~PTZListModel();
	/* Constructs/destroys the ptzDeviceList singleton. Called from
	 * ptz_load_devices()/ptz_unload_devices() so construction happens at
	 * obs_module_load() time, once the global PTZ signal_handler these
	 * devices connect to already exists -- see ptzDeviceList's extern
	 * declaration below. */
	static void create();
	static void destroy();
	QModelIndex index(int row, int column, const QModelIndex &parent = QModelIndex()) const override;
	QModelIndex parent(const QModelIndex &child) const override;
	int rowCount(const QModelIndex &parent = QModelIndex()) const override;
	int columnCount(const QModelIndex &) const override { return 1; };
	bool insertRows(int row, int count, const QModelIndex &parent = QModelIndex()) override;
	bool removeRows(int row, int count, const QModelIndex &parent = QModelIndex()) override;
	bool moveRows(const QModelIndex &srcParent, int srcRow, int count, const QModelIndex &destParent,
		      int destChild) override;
	QVariant data(const QModelIndex &index, int role) const override;
	bool setData(const QModelIndex &index, const QVariant &value, int role = Qt::EditRole) override;
	void do_reset();
	Qt::ItemFlags flags(const QModelIndex &index) const override;
	void onSceneChanged();

	/* Data Model */
	QModelIndex indexFromDeviceId(uint32_t device_id) const;
	QModelIndex indexFromName(const QString &name) const;
	bool callDevice(const QModelIndex &index, const char *method, calldata_t *cd = nullptr);
	bool callDevice(const char *method, calldata_t *cd = nullptr);
	void renameDevice(QString new_name, QString prev_name);
	void save(OBSDataArray configs) const;
	void save(const QModelIndex &index, OBSData settings) const;
	void update(const QModelIndex &index, OBSData settings);
	obs_properties_t *getProperties(const QModelIndex &index) const;

	/* Bracket a preset list mutation with the appropriate
	 * QAbstractItemModel begin/end calls. Called from the per-device
	 * signal_handler trampolines in ptz-list-model.cpp (see
	 * deviceCreated()) in response to PTZDevice's preset_insert/
	 * preset_remove/preset_move signals, not directly by PTZDevice. */
	void presetBeginInsert(uint32_t device_id, int row);
	void presetEndInsert(uint32_t device_id);
	void presetBeginRemove(uint32_t device_id, int row);
	void presetEndRemove(uint32_t device_id);
	bool presetBeginMove(uint32_t device_id, int srcRow, int destRow);
	void presetEndMove(uint32_t device_id);

	/* Called by the signal_handler trampolines in ptz-list-model.cpp;
	 * not Qt slots since nothing emits a Qt signal for any of this. */
	void deviceCreated(uint32_t device_id, proc_handler_t *ph, signal_handler_t *sh);
	void deviceDestroyed(uint32_t device_id);
	void deviceStatusChanged(uint32_t device_id);
	void presetsChanged(uint32_t device_id);

public slots:
	void preset_recall(uint32_t device_id, int preset_id);
	void preset_save(uint32_t device_id, int preset_id);
};

/* A pointer rather than a plain global: a plain global's constructor would
 * run at plugin-library-load time (static init), before OBS calls
 * obs_module_load() -> ptz_load_devices(), which is what creates the global
 * PTZ signal_handler this class's constructor needs to connect to.
 * PTZListModel::create() allocates it at the right time instead. It's valid
 * for the plugin's entire lifetime after that (obs_module_load() calls
 * create() before anything else that could touch it), so callers don't
 * null-check it. */
extern PTZListModel *ptzDeviceList;
