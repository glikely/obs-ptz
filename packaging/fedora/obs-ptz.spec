# Upstream version as tagged (with a leading v). A pre-release is packaged as
# Version: <next>~<pre>, which sorts before the final release. When v1.0.0 is
# tagged: set upstream_version and Version to 1.0.0.
%global upstream_version 0.20.0-pre7

Name:           obs-ptz
Version:        0.20.0~pre7
Release:        %autorelease
Summary:        PTZ camera control plugin for OBS Studio

# Plugin sources are GPL-2.0-or-later (relicensed from GPL-2.0-only upstream in
# 8fe88490, with the invalid GPLv2 SPDX ids and the old FSF address fixed).
# Bundled helper code, built into the plugin:
#   shared/qjoysticks (QJoysticks)           MIT
#   shared/qjoysticks/SDL/Database.txt       Zlib (SDL_GameControllerDB)
#   shared/properties-view, shared/qt/*      GPL-2.0-or-later (from obs-studio)
# Not shipped: shared/catch2 (BSL-1.0) is linked only into the %%check test
# executables, built in a separate tree, and shared/qtserialport (Qt's,
# multi-licensed) is not built on Linux.
License:        GPL-2.0-or-later AND MIT AND Zlib
URL:            https://github.com/glikely/obs-ptz
Source0:        %{url}/archive/refs/tags/v%{upstream_version}/%{name}-%{upstream_version}.tar.gz

# Same as obs-studio, which this plugin loads into
ExcludeArch:    %{ix86}

BuildRequires:  cmake >= 3.28
BuildRequires:  gcc
BuildRequires:  gcc-c++
BuildRequires:  make
BuildRequires:  obs-studio-devel
BuildRequires:  cmake(Qt6Core)
BuildRequires:  cmake(Qt6Gui)
BuildRequires:  cmake(Qt6Network)
BuildRequires:  cmake(Qt6SerialPort)
BuildRequires:  cmake(Qt6Svg)
BuildRequires:  cmake(Qt6Widgets)
BuildRequires:  cmake(Qt6Xml)
# OBSQTDisplay needs Qt6::GuiPrivate
BuildRequires:  qt6-qtbase-private-devel
# Joystick support (QJoysticks uses SDL2; sdl2-compat satisfies this on F43+)
BuildRequires:  cmake(SDL2)

# The plugin loads into the obs binary and uses its Qt/libobs ABI
Requires:       obs-studio%{?_isa}

# Code copied into the tree and built into the plugin:
#   shared/qjoysticks    QJoysticks, which has no releases; imported from
#                        commit ccc0f53 on 2025-04-24 (snapshot version)
#   shared/properties-view, shared/qt/*
#                        OBSPropertiesView and Qt widgets from obs-studio 32.2.1
#   shared/qjoysticks/SDL/Database.txt
#                        SDL_GameControllerDB data file (no upstream release)
# SDL2 itself is linked dynamically and needs no entry.
Provides:       bundled(qjoysticks) = 0^20250424gitccc0f53
Provides:       bundled(obs-studio) = 32.2.1
Provides:       bundled(SDL_GameControllerDB)

%description
Pan, tilt and zoom control for PTZ cameras from within OBS Studio. Supports
VISCA over IP (UDP and TCP) and serial, Pelco, ONVIF, Sony discovery and USB
(UVC) cameras. Includes a controls dock, camera presets, a PTZ source and
filter, and optional joystick and game controller control.

%prep
%autosetup -p1 -n %{name}-%{upstream_version}

%global ptz_cmake_options -DENABLE_FRONTEND_API=ON -DENABLE_QT=ON -DENABLE_SERIALPORT=ON -DENABLE_JOYSTICK=ON -DENABLE_USB_CAM=ON -DENABLE_ONVIF=ON

%build
%cmake %{ptz_cmake_options}
%cmake_build

%install
%cmake_install

%check
# The unit test suites that need no OBS instance, camera or serial port, and
# the check of the shipped VISCA command sets. They are built in a tree of
# their own, so no test option, source or Catch2 (the copy vendored in
# shared/catch2, BSL-1.0) reaches the plugin that is shipped.
%global __cmake_builddir %{_vpath_builddir}-tests
%cmake %{ptz_cmake_options} -DENABLE_SOFT_PTZ_TESTS=ON -DENABLE_USB_TESTS=ON -DENABLE_VISCA_PROFILE_CHECK=ON
%cmake_build --target soft-ptz-tests --target usb-backend-tests --target check-visca-profiles
%{__cmake_builddir}/tests/soft-ptz/soft-ptz-tests
%{__cmake_builddir}/tests/usb-backend/usb-backend-tests

%files
%license LICENSE
%doc README.md AUTHORS
%{_libdir}/obs-plugins/obs-ptz.so
%{_datadir}/obs/obs-plugins/obs-ptz/

%changelog
%autochangelog
