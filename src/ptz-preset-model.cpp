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
		while (m_presets.contains(curr_id))
			curr_id++;
		if (curr_id >= m_maxPresets)
			return false;
		ids.append(curr_id);
	}

	beginInsertRows(parent, row, count);
	for (auto id : ids) {
		QVariantMap map;
		map["id"] = (uint)id;
		m_presets[id] = map;
		m_displayOrder.insert(row++, id);
	}
	endInsertRows();
	return true;
}

bool PTZPresetListModel::removeRows(int row, int count, const QModelIndex &parent)
{
	if (row < 0 || row >= rowCount())
		return false;
	beginRemoveRows(parent, row, count);
	QList<size_t> ids = m_displayOrder.mid(row, count);
	;
	for (auto id : ids) {
		m_displayOrder.removeAt(row);
		m_presets.remove(id);
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
	m_displayOrder.move(srcRow, destChild);
	endMoveRows();
	return true;
}
int PTZPresetListModel::rowCount(const QModelIndex &parent) const
{
	Q_UNUSED(parent);
	return (int)m_displayOrder.size();
}

int PTZPresetListModel::getPresetId(const QModelIndex &index) const
{
	if (!index.isValid())
		return -1;

	if (index.row() >= m_displayOrder.size()) {
		blog(LOG_ERROR, "ERROR: Preset Row %i is not valid", index.row());
		return -1;
	}
	return (int)m_displayOrder[index.row()];
}

QVariant PTZPresetListModel::data(const QModelIndex &index, int role) const
{
	auto id = getPresetId(index);
	if (id < 0)
		return QVariant();
	auto preset = m_presets[id];
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

void PTZPresetListModel::sanitize(size_t id)
{
	if (!m_presets.contains(id))
		return;
	if (!m_displayOrder.contains(id))
		m_displayOrder.append(id);
	QVariantMap &preset = m_presets[id];
	QString name = preset["name"].toString();
	if (name == "" || name == QString(obs_module_text("PTZ.PresetNum")).arg(id))
		preset.remove("name");
}

bool PTZPresetListModel::setData(const QModelIndex &index, const QVariant &value, int role)
{
	auto id = getPresetId(index);
	if (id < 0)
		return false;

	if (role == Qt::EditRole) {
		QVariantMap &preset = m_presets[id];
		preset["name"] = value.toString();
		sanitize(id);
		emit dataChanged(index, index);
		return true;
	}
	return false;
}

/* Insert a new preset and return the ID */
int PTZPresetListModel::newPreset()
{
	size_t id = 0;
	while (m_presets.contains(id))
		id++;
	if (id >= m_maxPresets)
		return -1;

	QVariantMap map;
	map["id"] = (uint)id;
	beginInsertRows(QModelIndex(), rowCount(), 1);
	m_presets[id] = map;
	m_displayOrder.append(id);
	endInsertRows();
	return (int)id;
}

QVariant PTZPresetListModel::presetProperty(size_t id, QString key)
{
	/* Safe to derefernce unconditionally here. Both levels will return
	 * default empty values without segfaulting */
	return m_presets[id][key];
}

bool PTZPresetListModel::updatePreset(size_t id, const QVariantMap &map)
{
	if (!m_presets.contains(id))
		return false;
	m_presets[id].insert(map);
	return true;
}

int PTZPresetListModel::find(QString key, QVariant value)
{
	/* Search for the matching key/value pair in all presets.
	 * This is an O(N) operation */
	auto end = m_presets.cend();
	for (auto i = m_presets.cbegin(); i != end; i++) {
		if (i.value()[key] == value)
			return (int)i.key();
	}
	return -1;
}

void PTZPresetListModel::loadPresets(OBSDataArray preset_array)
{
	if (!preset_array)
		return;
	beginResetModel();
	m_presets.clear();
	m_displayOrder.clear();
	for (size_t i = 0; i < obs_data_array_count(preset_array); i++) {
		OBSDataAutoRelease item = obs_data_array_item(preset_array, i);
		auto id = obs_data_get_int(item, "id");
		if (m_displayOrder.contains(id))
			continue;
		QVariantMap preset = OBSDataToVariantMap(item.Get());
		m_presets[id] = preset;
		sanitize(id);
	}
	endResetModel();
}

OBSDataArray PTZPresetListModel::savePresets() const
{
	OBSDataArrayAutoRelease preset_array = obs_data_array_create();
	for (auto id : m_displayOrder) {
		OBSDataAutoRelease data = variantMapToOBSData(m_presets[id]);
		obs_data_set_int(data, "id", id);
		obs_data_array_push_back(preset_array, data);
	}
	return preset_array.Get();
}
