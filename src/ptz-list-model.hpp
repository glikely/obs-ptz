/* Pan Tilt Zoom Controls - Qt list model for managing devices
 *
 * Copyright 2020-2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once

#include <obs.hpp>
#include <QObject>
#include <QAbstractItemModel>
#include <QHash>
#include <QList>
#include <QMimeData>
#include <optional>
#include "ptz.h"

/**
 * PTZListModel never holds a PTZDevice* (see AGENTS.md / the decoupling
 * design in ptz-device.cpp): every device it knows about is represented
 * purely by its source, a filter or not, a weak reference to it found through
 * OBS's "source_create" and "source_filter_add" signals, and a local cache of the
 * fields QAbstractItemModel::data() needs to stay synchronous. All control
 * goes out through proc_handler_call(); the cache is kept in sync purely by
 * signal_handler notifications (see PTZListModel()'s constructor/the *_cb
 * trampolines in ptz-list-model.cpp).
 */
class PTZListModel : public QAbstractItemModel {
	Q_OBJECT

public:
	struct PresetEntry {
		/* The device's own id for it, see docs/ptz-device-api.md */
		QString id;
		/* The store it is in: "camera" or "local" */
		QString store;
		QString name;
		QString thumbnail; /* path of the image file, or empty */
	};

private:
	struct PTZDeviceEntry {
		/* Private to the model, to say whose preset a row is in an index's
		 * internalId(): 0 is not one */
		uint32_t serial = 0;
		/* The UUID of the device's source, which is how a device is
		 * known outside the model */
		QString uuid;
		/* This device's source, a filter or not. Its proc_handler and
		 * signal_handler are the device's, so they are asked for when needed,
		 * from a strong reference, never kept: they go when the source does */
		OBSWeakSource weakSource;
		QString name;
		bool connected = false;
		/* Whether the device's source is in the program scene, and in the
		 * preview scene (Studio Mode only). Worked out here from the
		 * source, not told by the device */
		bool live = false;
		bool preview = false;
		/* Only when the camera says it is, not when it doesn't say */
		bool poweredOff = false;
		/* Movement from the UI is refused. A property of the UI, not the
		 * device: it follows live at each scene change, and in between a
		 * user can lock or unlock it */
		bool locked = false;
		/* The names in its state's "features", or nothing if it has none:
		 * a device from before there were, which can do anything */
		std::optional<QStringList> features;
		QList<PresetEntry> presets;
	};

	QList<PTZDeviceEntry> devices;
	QHash<uint32_t, int> rowBySerial;
	QHash<QString, int> rowByUuid;
	uint32_t nextSerial = 1;

	/* The cameras' order, as the uuids of their devices: what the user left them in, kept for
	 * the scene collection that has them. A camera is put where it has in this when it
	 * turns up, and one it does not have goes last. */
	QStringList savedOrder;
	/* Where a camera that turns up goes, by savedOrder */
	int rowForNewDevice(const QString &uuid) const;
	bool moveDevices(int srcRow, int count, int destChild);

	void rebuildRowIndex();
	PTZDeviceEntry *entryAt(int row);
	const PTZDeviceEntry *entryAt(int row) const;
	PTZDeviceEntry *entryAt(const QModelIndex &index);
	const PTZDeviceEntry *entryAt(const QModelIndex &index) const;
	PTZDeviceEntry *entryBySerial(uint32_t serial);
	const PTZDeviceEntry *entryBySerial(uint32_t serial) const;
	PTZDeviceEntry *entryByUuid(const QString &uuid);
	const PTZDeviceEntry *entryByUuid(const QString &uuid) const;
	void refreshDeviceState(PTZDeviceEntry *entry);
	OBSSource parentSourceOf(const PTZDeviceEntry &entry) const;
	/* Works out live and preview from the program and preview scenes.
	 * Returns whether anything changed. */
	bool refreshSceneState(PTZDeviceEntry *entry);
	void refreshPresetList(PTZDeviceEntry *entry);
	/* What the device says its presets are, which does not change the cache */
	QList<PresetEntry> fetchPresets(const PTZDeviceEntry &entry) const;
	bool callEntry(const PTZDeviceEntry &entry, const char *method, calldata_t *cd) const;

public:
	enum PTZListModelRole {
		DeviceUuidRole = Qt::UserRole, /* QString: the UUID of the device's source */
		IsLiveRole,
		IsPreviewRole,
		IsConnectedRole,
		IsPoweredOffRole,
		IsLockedRole,
		ThumbnailRole,   /* QPixmap of a preset row, null if it has none */
		FeaturesRole,    /* QStringList, or invalid if the device doesn't say */
		PresetStoreRole, /* QString of a preset row: "camera" or "local" */
	};

	PTZListModel();
	~PTZListModel();
	/* Constructs/destroys the ptzDeviceList singleton */
	static void create();
	static void destroy();
	QModelIndex index(int row, int column, const QModelIndex &parent = QModelIndex()) const override;
	QModelIndex parent(const QModelIndex &child) const override;
	int rowCount(const QModelIndex &parent = QModelIndex()) const override;
	int columnCount(const QModelIndex &) const override { return 1; };
	bool removeRows(int row, int count, const QModelIndex &parent = QModelIndex()) override;
	bool moveRows(const QModelIndex &srcParent, int srcRow, int count, const QModelIndex &destParent,
		      int destChild) override;
	QVariant data(const QModelIndex &index, int role) const override;
	bool setData(const QModelIndex &index, const QVariant &value, int role = Qt::EditRole) override;
	void do_reset();
	Qt::ItemFlags flags(const QModelIndex &index) const override;
	QStringList mimeTypes() const override;
	QMimeData *mimeData(const QModelIndexList &indexes) const override;
	Qt::DropActions supportedDropActions() const override;
	/* The program or preview scene, or Studio Mode, changed: every device
	 * is in the program scene, the preview scene, both or neither anew, and
	 * locked if it is live, whatever a user had locked or unlocked */
	void onSceneChanged();

	/* The uuids of the devices in the order of the camera list, which is the order to save for
	 * the scene collection. Devices turn up in the order OBS loads them, and a move by the
	 * user is what makes the order a user's. */
	QStringList deviceOrder() const;
	/* Puts the cameras in this order, and has those that are not there yet go where it says when
	 * they are: a scene collection that is loaded has its own. Cameras it does not name go last,
	 * in the order they were in. */
	void setDeviceOrder(const QStringList &uuids);

	/* Starts making a preset in `store` ("camera" or "local") of the device with
	 * this uuid from where the camera is now, last in the list. Returns the request,
	 * or "" if the device refuses at once: it has no such store, or no free slot.
	 * presetCreateDone() says how the request went, with the id of the preset made,
	 * once the device knows, which is after this has returned. */
	QString addPreset(const QString &uuid, const QString &store, const QString &name = QString());

	/* The stores the device can make a preset in, "camera" and "local" */
	QStringList presetStores(const QModelIndex &device) const;

	/* Whether the device can do `feature` (see PTZDevice::featureNames()) */
	static bool hasFeature(const QModelIndex &index, const char *feature);

	/* Data Model */
	QModelIndex indexFromUuid(const QString &uuid) const;
	QModelIndex indexFromName(const QString &name) const;
	QModelIndex indexFromSource(obs_source_t *source) const;
	/* The source a device is on, whose video is what the camera shows, as
	 * the device says (see PTZDevice::parentSource()). Null while it has
	 * none. */
	OBSSource parentSource(const QModelIndex &index) const;
	bool callDevice(const QModelIndex &index, const char *method, calldata_t *cd = nullptr);
	void save(const QModelIndex &index, OBSData settings) const;
	void update(const QModelIndex &index, OBSData settings);
	obs_properties_t *getProperties(const QModelIndex &index) const;
	void saveState(const QModelIndex &index, OBSData state) const;
	void setState(const QModelIndex &index, OBSData state);
	void removeDevice(const QModelIndex &index);

	/* React to a change PTZDevice reports to its presets *after* it
	 * already happened, by working out what changed in the model's own
	 * (still-stale) cache and wrapping its refresh in the appropriate
	 * QAbstractItemModel begin/end calls -- called from the per-device
	 * signal_handler trampoline in ptz-list-model.cpp (see deviceCreated()) in
	 * response to any of PTZDevice's ptz_preset_* signals. All the begin/end
	 * bracketing lives here: PTZDevice just states that something changed. */
	void presetsSync(const QString &uuid);

	/* Called by the signal_handler trampolines in ptz-list-model.cpp;
	 * not Qt slots, they bring the cache up to date and then, for the
	 * two below, tell listeners with the signals of the same name. */
	void deviceCreated(OBSWeakSource weakSource);
	void deviceDestroyed(const QString &uuid);
	void deviceStateChanged(const QString &uuid, OBSData changed);
	void deviceSettingsChanged(const QString &uuid);

signals:
	/* For whoever shows a device's settings or state as more than a row
	 * (the settings dialog): dataChanged() says a row needs redrawing, and
	 * can't tell which half of the device changed, or how. Emitted after the
	 * cache above is current. Connect these directly: OBSData isn't a
	 * registered metatype, so they can't be queued. */
	/* The device's settings changed, from anywhere */
	void deviceSettingsUpdated(const QString &uuid);
	/* The device's state changed; `changed` holds the values it reported
	 * as new, as of when it reported them */
	void deviceStateUpdated(const QString &uuid, OBSData changed);
	/* A request to make a preset, from addPreset(), has been answered: `id` is the id of the
	 * preset that was made, or "" if the device made none */
	void presetCreateDone(const QString &uuid, const QString &request, const QString &id);

public slots:
	void preset_recall(const QString &uuid, const QString &preset_id);
	void preset_save(const QString &uuid, const QString &preset_id);
};

extern PTZListModel *ptzDeviceList;
