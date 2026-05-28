/* Pan Tilt Zoom device list model
 *
 * Copyright 2020 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */
#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QStringList>
#include <obs.hpp>

class PTZDevice;

class PTZListModel : public QAbstractListModel {
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
int rowCount(const QModelIndex &parent = QModelIndex()) const override;
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
QModelIndex indexFromDeviceId(uint32_t device_id);
QModelIndex indexFromName(const QString &name);
void renameDevice(QString new_name, QString prev_name);
obs_data_array_t *getConfigs();
void removeDevice(const QModelIndex &index);
void add(PTZDevice *ptz);
void remove(PTZDevice *ptz);
void delete_all();

public slots:
void preset_recall(uint32_t device_id, int preset_id);
void preset_save(uint32_t device_id, int preset_id);
void move_continuous(uint32_t device_id, uint32_t flags, double pan, double tilt, double zoom, double focus);
};

extern PTZListModel ptzDeviceList;
