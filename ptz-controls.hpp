#pragma once

#include <QTimer>
#include <obs.hpp>
#include <QDockWidget>

#include "../UI/qt-wrappers.hpp"
#include <../UI/obs-frontend-api/obs-frontend-api.h>

#include "ui_ptz-controls.h"

class PTZControls : public QDockWidget {
	Q_OBJECT

private:
	static void OBSSignal(void *data, const char *signal,
			      calldata_t *calldata);
	static void OBSFrontendEvent(enum obs_frontend_event event, void *ptr);
	void ControlContextMenu();

	std::unique_ptr<Ui::PTZControls> ui;

/*
private slots:
	void SignalMediaSource();
*/

public:
	PTZControls(QWidget *parent = nullptr);
	~PTZControls();
};
