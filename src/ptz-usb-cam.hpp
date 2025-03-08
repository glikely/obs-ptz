/* Pan Tilt Zoom VISCA over UVC USB
	*
	* Copyright 2025 Fabio Ferrari <fabioferrari@gmail.com>
	*
	* SPDX-License-Identifier: GPLv2
	*/
#pragma once

#include <QObject>
#include <QTcpSocket>
#include "ptz-device.hpp"


enum PTZAxis {
    PTZ_PAN = 0, PTZ_TILT = 1, PTZ_ZOOM = 2, PTZ_FOCUS = 3, PTZ_FOCUS_AUTO = 4
};

class PTZControl {
protected:
    std::string device_path;
    std::array<long, 4> min, max;
public:
    virtual ~PTZControl() {}
    virtual bool internal_pan(long value) = 0;
    bool pan(double value) {
        value = std::clamp(value, -1.0, 1.0);
        long mapped_value = static_cast<long>(value * max[PTZ_PAN]);
        mapped_value = std::clamp(mapped_value, min[PTZ_PAN], max[PTZ_PAN]);
        return internal_pan(mapped_value);
    }
    virtual bool internal_tilt(long value) = 0;
    bool tilt(double value) {
        value = std::clamp(value, -1.0, 1.0);
        long mapped_value = static_cast<long>(value * max[PTZ_TILT]);
        mapped_value = std::clamp(mapped_value, min[PTZ_TILT], max[PTZ_TILT]);
        return internal_tilt(mapped_value);
    }
    virtual bool internal_zoom(long value) = 0;
    bool zoom(double value) {
        value = std::clamp(value, 0.0, 1.0);
        long mapped_value = static_cast<long>(value * max[PTZ_ZOOM]);
        mapped_value = std::clamp(mapped_value, min[PTZ_ZOOM], max[PTZ_ZOOM]);
        return internal_zoom(mapped_value);
    }
    virtual bool internal_focus(long value) = 0;
    bool focus(double value) {
        value = std::clamp(value, 0.0, 1.0);
        long mapped_value = static_cast<long>(value * max[PTZ_FOCUS]);
        mapped_value = std::clamp(mapped_value, min[PTZ_FOCUS], max[PTZ_FOCUS]);
        return internal_focus(mapped_value);
    }
    virtual bool setAutoFocus(bool enabled) = 0;
    std::string getDevicePath() { return device_path; }
    virtual bool isValid() const = 0;
};

struct PtzUsbCamPos {
        double pan = 0;
        double tilt = 0;
        double zoom = 0;
        bool focusAuto = true;
        double focus = 0;
        // bool whitebalAuto = true;
        //double temperature = 0;
    };

class PTZUSBCam : public PTZDevice {
	Q_OBJECT

private:
    QString m_PTZAddress{""};
    PtzUsbCamPos now_pos;
    QMap<int, PtzUsbCamPos> presets;
    double tick_elapsed = 0.0f;
    PTZControl *ptz_control_ = nullptr;
protected:
    static void ptz_tick_callback(void *param, float seconds);
    void ptz_tick(float seconds);

public:
	PTZUSBCam(OBSData config);
	~PTZUSBCam();
	void save(obs_data_t* settings) const;
	virtual QString description();

	void set_config(OBSData ptz_data);
	OBSData get_config();
	obs_properties_t *get_obs_properties();

	void initialize_and_check_ptz_control();

	void do_update();
	void pantilt_rel(double pan, double tilt);
	void pantilt_abs(double pan, double tilt);
	void pantilt_home();
	void zoom_abs(double pos);
	void focus_abs(double pos);
	void set_autofocus(bool enabled);
	void memory_reset(int i);
	void memory_set(int i);
	void memory_recall(int i);
};
