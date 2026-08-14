/* Own-main() Catch2 entry point (catch2_amalgamated is built with
 * CATCH_AMALGAMATED_CUSTOM_MAIN, see shared/catch2/CMakeLists.txt). The worker
 * tests need Qt's event loop, for the signals that come back from its thread.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <catch_amalgamated.hpp>

#include <QCoreApplication>

int main(int argc, char *argv[])
{
	QCoreApplication app(argc, argv);
	return Catch::Session().run(argc, argv);
}
