#include <QListView>
#include <QDropEvent>
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

void CircularListView::enableRowDragging()
{
	setDragEnabled(true);
	setAcceptDrops(true);
	setDropIndicatorShown(true);
	setDragDropMode(QAbstractItemView::InternalMove);
	setDefaultDropAction(Qt::MoveAction);
}

/* Reorder with the model's moveRow() ourselves: QListView's own handling moves
 * the rows of the top level only, not those under rootIndex(), where the presets
 * of a camera are, and would let the base class remove the "moved" source rows */
void CircularListView::dropEvent(QDropEvent *event)
{
	if (event->source() != this || !dropCurrentRowAt(event->position().toPoint())) {
		event->ignore();
		return;
	}
	/* Accepting with the action ignored keeps the base class from removing the
	 * source rows */
	event->setDropAction(Qt::IgnoreAction);
	event->accept();
}

/* The row goes before the one at `pos` if that is in the first half of it, after
 * it if in the second, or last if there is no row there */
bool CircularListView::dropCurrentRowAt(const QPoint &pos)
{
	auto m = model();
	auto current = currentIndex();
	if (!m || !current.isValid())
		return false;
	auto target = indexAt(pos);
	int dest = m->rowCount(rootIndex());
	if (target.isValid()) {
		auto r = visualRect(target);
		bool after = viewMode() == IconMode ? pos.x() > r.center().x() : pos.y() > r.center().y();
		dest = target.row() + (after ? 1 : 0);
	}
	return m->moveRow(rootIndex(), current.row(), rootIndex(), dest);
}
