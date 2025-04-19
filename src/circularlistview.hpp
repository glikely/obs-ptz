#pragma once
#include <QListView>

class CircularListView : public QListView {
	Q_OBJECT

public:
	CircularListView(QWidget *parent = nullptr) : QListView(parent){};

public slots:
	void cursorUp();
	void cursorDown();

protected:
	QModelIndex moveCursor(QAbstractItemView::CursorAction action,
			       Qt::KeyboardModifiers modifiers);
};
