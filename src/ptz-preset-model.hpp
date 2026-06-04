/* Pan Tilt Zoom Controls - Qt list model for managing presets
 *
 * Copyright 2020-2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */
#pragma once

#include <QObject>
#include <QSize>
#include <QAbstractListModel>

class PTZDevice;

class PTZPresetListModel : public QAbstractListModel {
	Q_OBJECT

public:
	PTZDevice *ptz;
	/* QAbstractListModel overrides */
	bool insertRows(int row, int count, const QModelIndex &parent = QModelIndex()) override;
	bool removeRows(int row, int count, const QModelIndex &parent = QModelIndex()) override;
	bool moveRows(const QModelIndex &srcParent, int srcRow, int count, const QModelIndex &destParent,
		      int destChild) override;
	int rowCount(const QModelIndex &parent = QModelIndex()) const override;
	QVariant data(const QModelIndex &index, int role) const override;
	bool setData(const QModelIndex &index, const QVariant &value, int role = Qt::EditRole) override;
	Qt::ItemFlags flags(const QModelIndex &index) const override;

	/* PTZ Preset API */
	int getPresetId(const QModelIndex &index) const;
	int newPreset();
};
