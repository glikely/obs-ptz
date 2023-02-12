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
	auto index = currentIndex();
	auto bottom = model()->rowCount() - 1;
	if (index.isValid() && (index.row() <= 0) && (action == MoveUp))
		return model()->index(bottom, index.column(), index.parent());
	if (index.isValid() && (index.row() >= bottom) && (action == MoveDown))
		return model()->index(0, index.column(), index.parent());
	return QListView::moveCursor(action, modifiers);
}
