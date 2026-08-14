/* uart-wrapper.cpp's addOBSProperties() calls obs_module_text() (never
 * exercised by this test suite, but still referenced at link time). That
 * symbol is not exported by libobs itself - it only exists because
 * OBS_MODULE_USE_DEFAULT_LOCALE() is expanded once, in the real plugin's
 * src/ptz.c, alongside OBS_DECLARE_MODULE(). Pulling in ptz.c itself would
 * drag in ptz_load_devices()/ptz_load_controls()/etc., which this test
 * binary has no use for - this gives it its own minimal, self-contained
 * module identity instead.
 *
 * obs_module_set_locale() is never called (no real module load happens),
 * so obs_module_text() falls back to returning its input string unchanged
 * (text_lookup_getstr(nullptr, ...) returns false without touching *out -
 * libobs/util/text-lookup.c) - fine, since this suite never calls
 * addOBSProperties().
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <obs-module.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-ptz-uart-hil-tests", "en-US")
