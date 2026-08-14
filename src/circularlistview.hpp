/* Circular list view widget
 *
 * Copyright 2023 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once
#include <QListView>

class CircularListView : public QListView {
	Q_OBJECT

public:
	CircularListView(QWidget *parent = nullptr) : QListView(parent) {};

	/* What dropping the current row at `pos` (in the viewport) does, apart from
	 * the drag: moves it there, with the model's moveRow(). False if it did not move */
	bool dropCurrentRowAt(const QPoint &pos);

	/* Rows can be dragged to reorder them, which a list of presets has */
	void enableRowDragging();

public slots:
	void cursorUp();
	void cursorDown();

protected:
	void dragEnterEvent(QDragEnterEvent *event) override;
	void dragMoveEvent(QDragMoveEvent *event) override;
	void dropEvent(QDropEvent *event) override;
	QModelIndex moveCursor(QAbstractItemView::CursorAction action, Qt::KeyboardModifiers modifiers) override;
};
