# Included into obs-studio's own project() by _setup_obs_studio (see buildspec_common.cmake).
# Its libobs-metal is written in Swift, but before 32.2 obs-studio didn't enable the
# language itself, which the Xcode generator of CMake 4 no longer does for it.
enable_language(Swift)
