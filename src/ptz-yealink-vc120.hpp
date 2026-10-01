/* Yealink VC120 Driver for OBS-PTZ
 *
 * Copyright 2026
 */

#pragma once

#include <QObject>
#include <QTimer>
#include "ptz-device.hpp"

class PTZYealinkVC120 : public PTZDevice
{
    Q_OBJECT

public:
    explicit PTZYealinkVC120(OBSData config);

    QString description() override;

    // PTZDevice
    void do_update() override;
    void set(calldata_t *cd) override;
    void getDefaults(OBSData config) const override;
    void update(OBSData config) override;
    void save(OBSData config) const override;

    obs_properties_t *get_obs_properties() override;

protected:
    void pantilt_home() override;
    void memory_set(int preset) override;
    void memory_recall(int preset) override;
    void memory_reset(int preset) override;

private:
    QString host;
    int port = 0;

    QString username;
    QString password;

    bool loggedIn = false;

    QTimer commandTimer;

    void login();
    void standbyToggle();
    void sendKey(int key);
    void sendPreset(int preset);
    void recallPreset(int preset);

};