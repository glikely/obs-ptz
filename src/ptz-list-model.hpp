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

class PTZDevice;

class PTZListModel : public QAbstractItemModel {
	Q_OBJECT

private:
	QList<PTZDevice *> devices;
	QHash<uint32_t, PTZDevice *> devicesById;

public:
	enum PTZListModelRole {
		DeviceIdRole = Qt::UserRole,
		DescriptionRole,
		IsLiveRole,
		IsConnectedRole,
		IsLockedRole,
		SupportsSetHomeRole,
	};

	PTZListModel();
	~PTZListModel();
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
	void name_changed(PTZDevice *ptz);
	Qt::ItemFlags flags(const QModelIndex &index) const override;
	void onSceneChanged();

	/* Data Model */
	PTZDevice *make_device(OBSData config);
	PTZDevice *getDevice(const QModelIndex &index) const;
	PTZDevice *getDevice(uint32_t device_id) const;
	PTZDevice *getDeviceByName(const QString &name) const;
	QStringList getDeviceNames() const;
	bool callDevice(const QModelIndex &index, const char *method, calldata_t *cd = nullptr);
	bool callDevice(const char *method, calldata_t *cd = nullptr);
	QModelIndex indexFromDeviceId(uint32_t device_id);
	QModelIndex indexFromName(const QString &name);
	void renameDevice(QString new_name, QString prev_name);
	void save(OBSDataArray configs) const;
	void save(const QModelIndex &index, OBSData settings) const;
	void update(const QModelIndex &index, OBSData settings);
	obs_properties_t *getProperties(const QModelIndex &index) const;
	void removeDevice(const QModelIndex &index);
	void add(PTZDevice *ptz);
	void remove(PTZDevice *ptz);
	void delete_all();

public slots:
	void preset_recall(uint32_t device_id, int preset_id);
	void preset_save(uint32_t device_id, int preset_id);
	void deviceSettingsChanged(PTZDevice *ptz, OBSData changed);
};

extern PTZListModel ptzDeviceList;
