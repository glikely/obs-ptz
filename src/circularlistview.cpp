#include <QListView>
#include "circularlistview.hpp"

void CircularListView::cursorUp()
{
	auto next = moveCursor(MoveUp, Qt::NoModifier);
	if (next.isValid())
		setCurrentIndex(next);
}

void CircularListView::cursorDown()
{
	auto next = moveCursor(MoveDown, Qt::NoModifier);
	if (next.isValid())
		setCurrentIndex(next);
}

QModelIndex CircularListView::moveCursor(QAbstractItemView::CursorAction action,
					 Qt::KeyboardModifiers modifiers)
{
	auto m = model();
	auto index = currentIndex();
	if (m && m->rowCount() > 0 && index.isValid()) {
		auto last = m->rowCount() - 1;
		if ((index.row() <= 0) && (action == MoveUp))
			return m->index(last, index.column(), index.parent());
		if ((index.row() >= last) && (action == MoveDown))
			return m->index(0, index.column(), index.parent());
	}
	return QListView::moveCursor(action, modifiers);
}
