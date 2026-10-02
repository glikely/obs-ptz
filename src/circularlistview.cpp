/* Circular list view widget
 *
 * Copyright 2023 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2+
 */

#include <QListView>
#include "circularlistview.hpp"

void CircularListView::cursorUp()
{
	/* In a grid, up and down would move by rows of cells: step through
	 * the presets one at a time instead */
	auto next = moveCursor(viewMode() == IconMode ? MovePrevious : MoveUp, Qt::NoModifier);
	if (next.isValid())
		setCurrentIndex(next);
}

void CircularListView::cursorDown()
{
	auto next = moveCursor(viewMode() == IconMode ? MoveNext : MoveDown, Qt::NoModifier);
	if (next.isValid())
		setCurrentIndex(next);
}

QModelIndex CircularListView::moveCursor(QAbstractItemView::CursorAction action, Qt::KeyboardModifiers modifiers)
{
	auto m = model();
	auto index = currentIndex();
	if (m && m->rowCount() > 0 && index.isValid()) {
		auto last = m->rowCount() - 1;
		/* Only the list steps by up and down; a grid's cells wrap
		 * on their own, so there they move by the cell */
		bool back = action == MovePrevious || (viewMode() == ListMode && action == MoveUp);
		bool forward = action == MoveNext || (viewMode() == ListMode && action == MoveDown);
		if (index.row() <= 0 && back)
			return m->index(last, index.column(), index.parent());
		if (index.row() >= last && forward)
			return m->index(0, index.column(), index.parent());
	}
	return QListView::moveCursor(action, modifiers);
}
