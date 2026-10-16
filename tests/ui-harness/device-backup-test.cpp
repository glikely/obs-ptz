/* PTZ UI test harness: the device backup store
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "ui-test-harness.hpp"

#include <obs-module.h>

#include "ptz-backup-store.hpp"

namespace {

/* Has the store read the backup file again, after a test wrote it: it reads it
 * when it first needs it, and keeps what it has until it is stopped */
void runReloadDeviceBackupsTest(const QMap<QString, QString> &)
{
	ptz_backup_store_stop();
	ptz_backup_store_start();
}

} // namespace

/* reload_device_backups: read the backup file again
 */
void registerDeviceBackupTest(PTZUITestHarness *harness)
{
	harness->registerTest(QStringLiteral("reload_device_backups"), &runReloadDeviceBackupsTest);
}
