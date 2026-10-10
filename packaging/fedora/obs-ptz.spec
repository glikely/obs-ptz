# Upstream version as tagged (with a leading v). A pre-release is packaged as
# Version: <next>~<pre>, which sorts before the final release. When v1.0.0 is
# tagged: set upstream_version and Version to 1.0.0 and fix the License below.
%global upstream_version 0.20.0-pre2

Name:           obs-ptz
Version:        0.20.0~pre2
Release:        %autorelease
Summary:        PTZ camera control plugin for OBS Studio

# The v0.20.0-pre2 sources are GPL-2.0-only (their "GPLv2" SPDX tags mean the
# LICENSE text). Upstream relicensed to GPL-2.0-or-later in 8fe88490, after that
# tag, so use GPL-2.0-or-later for any release that includes it.
# Bundled helper code, built into the plugin:
#   shared/qjoysticks (QJoysticks)           MIT
#   shared/qjoysticks/SDL/Database.txt       Zlib (SDL_GameControllerDB)
#   shared/properties-view, shared/qt/*      GPL-2.0-or-later (from obs-studio)
# shared/catch2 and shared/qtserialport are not built on Linux and not shipped.
License:        GPL-2.0-only AND GPL-2.0-or-later AND MIT AND Zlib
URL:            https://github.com/glikely/obs-ptz
Source0:        %{url}/archive/refs/tags/v%{upstream_version}/%{name}-%{upstream_version}.tar.gz

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

# The project defaults an empty build type to RelWithDebInfo, but sdl2-compat's
# imported SDL2::SDL2 only has a "noconfig" location, so ask for None (the
# Fedora convention: all flags come from the rpm macros)
%build
%cmake \
    -DCMAKE_BUILD_TYPE=None \
    -DENABLE_FRONTEND_API=ON \
    -DENABLE_QT=ON \
    -DENABLE_SERIALPORT=ON \
    -DENABLE_JOYSTICK=ON \
    -DENABLE_USB_CAM=ON \
    -DENABLE_ONVIF=ON
%cmake_build

%install
%cmake_install

%files
%license LICENSE
%doc README.md AUTHORS
%{_libdir}/obs-plugins/obs-ptz.so
%{_datadir}/obs/obs-plugins/obs-ptz/

%changelog
%autochangelog
