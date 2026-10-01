/* VISCA commands and inquiries
 *
 * Copyright 2020 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */
#pragma once

#include "protocol-helpers.hpp"

/* The ones the driver sends by name */
extern const PTZCmd VISCA_CAM_Power;
extern const PTZCmd VISCA_CAM_Zoom_drive;
extern const PTZCmd VISCA_CAM_Zoom_Direct;
extern const PTZCmd VISCA_CAM_Focus_drive;
extern const PTZCmd VISCA_CAM_Focus_Auto;
extern const PTZCmd VISCA_CAM_Focus_Manual;
extern const PTZCmd VISCA_CAM_Focus_OneTouch;
extern const PTZCmd VISCA_CAM_ZoomFocus_Direct;
extern const PTZCmd VISCA_CAM_AFMode_ActiveIntervalTime;
extern const PTZCmd VISCA_CAM_WB_OnePushTrigger;
extern const PTZCmd VISCA_CAM_Memory_Reset;
extern const PTZCmd VISCA_CAM_Memory_Set;
extern const PTZCmd VISCA_CAM_Memory_Recall;
extern const PTZCmd VISCA_CAM_Tally_On;
extern const PTZCmd VISCA_CAM_Tally_Off;
extern const PTZCmd VISCA_CAM_TallyGreen_On;
extern const PTZCmd VISCA_CAM_TallyGreen_Off;
extern const PTZCmd VISCA_SYSMenu_Off;
extern const PTZCmd VISCA_PanTilt_drive;
extern const PTZCmd VISCA_PanTilt_drive_abs;
extern const PTZCmd VISCA_PanTilt_drive_rel;
extern const PTZCmd VISCA_PanTilt_Home;

extern const QList<QPair<const char *, PTZCmd>> visca_state_commands;
const char *visca_tally_key(const QByteArray &cmd);
