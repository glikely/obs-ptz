/* Pan Tilt Zoom Controls - Qt list model for managing presets
 *
 * Copyright 2020-2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */

#include <obs.hpp>
#include "ptz-device.hpp"
#include "ptz-list-model.hpp"
#include "ptz.h"
#include "protocol-helpers.hpp"

#if defined(ENABLE_SERIALPORT)
#include "ptz-visca-uart.hpp"
#include "ptz-pelco.hpp"
#endif

bool PTZPresetListModel::insertRows(int row, int count, const QModelIndex &parent)
{
	if (row < 0 || row > rowCount())
		return false;
	size_t curr_id = 0;
	QList<size_t> ids;
	for (int i = 0; i < count; i++) {
		while (ptz->m_presets.contains(curr_id))
			curr_id++;
		if (curr_id >= ptz->m_maxPresets)
			return false;
		ids.append(curr_id);
	}

	beginInsertRows(parent, row, count);
	for (auto id : ids) {
		QVariantMap map;
		map["id"] = (uint)id;
		ptz->m_presets[id] = map;
		ptz->m_presetsDisplayOrder.insert(row++, id);
	}
	endInsertRows();
	return true;
}

bool PTZPresetListModel::removeRows(int row, int count, const QModelIndex &parent)
{
	if (row < 0 || row >= rowCount())
		return false;
	beginRemoveRows(parent, row, count);
	QList<size_t> ids = ptz->m_presetsDisplayOrder.mid(row, count);
	;
	for (auto id : ids) {
		ptz->m_presetsDisplayOrder.removeAt(row);
		ptz->m_presets.remove(id);
	}
	endRemoveRows();
	return true;
}

bool PTZPresetListModel::moveRows(const QModelIndex &srcParent, int srcRow, int count, const QModelIndex &destParent,
				  int destChild)
{
	if (srcRow < 0 || srcRow >= rowCount() || destChild < 0 || destChild > rowCount() || count != 1)
		return false;

	if (!beginMoveRows(srcParent, srcRow, srcRow + count - 1, destParent, destChild))
		return false;
	if (srcRow < destChild)
		destChild--;
	ptz->m_presetsDisplayOrder.move(srcRow, destChild);
	endMoveRows();
	return true;
}
int PTZPresetListModel::rowCount(const QModelIndex &) const
{
	return ptz->presetCount();
}

int PTZPresetListModel::getPresetId(const QModelIndex &index) const
{
	return checkIndex(index) ? ptz->presetAtDisplayRow(index.row()) : -1;
}

QVariant PTZPresetListModel::data(const QModelIndex &index, int role) const
{
	auto id = getPresetId(index);
	if (id < 0)
		return QVariant();
	auto preset = ptz->m_presets[id];
	if (role == Qt::DisplayRole) {
		QString name = preset["name"].toString();
		return (name != "") ? name : QString(obs_module_text("PTZ.PresetNum")).arg(id);
	}
	if (role == Qt::ToolTipRole) {
		auto token = preset["token"].toString();
		if (token != "")
			return QString(obs_module_text("PTZ.Preset.Tooltip")).arg("'" + token + "'");
		return QString(obs_module_text("PTZ.Preset.Tooltip")).arg(id);
	}
	if (role == Qt::EditRole)
		return preset["name"].toString();
	if (role == Qt::UserRole)
		return id;
	if (role == Qt::SizeHintRole)
		return QSize(0, 20);

	return QVariant();
}

Qt::ItemFlags PTZPresetListModel::flags(const QModelIndex &index) const
{
	auto f = QAbstractListModel::flags(index);
	if (index.column() == 0)
		f |= Qt::ItemIsEditable;
	return f;
}

bool PTZPresetListModel::setData(const QModelIndex &index, const QVariant &value, int role)
{
	auto id = getPresetId(index);
	if (id < 0)
		return false;

	if (role == Qt::EditRole) {
		ptz->setPresetName(id, value.toString());
		emit dataChanged(index, index);
		return true;
	}
	return false;
}
