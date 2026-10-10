Name:           obs-ptz
Version:        0.19.0
Release:        %autorelease
Summary:        PTZ camera control plugin for OBS Studio

# Plugin sources are GPL-2.0-only. Bundled helper code, built into the plugin:
#   shared/qjoysticks (QJoysticks)           MIT
#   shared/qjoysticks/SDL/Database.txt       Zlib (SDL_GameControllerDB)
#   shared/properties-view, shared/qt/*      GPL-2.0-or-later (from obs-studio)
# shared/catch2 and shared/qtserialport are not built on Linux and not shipped.
License:        GPL-2.0-only AND GPL-2.0-or-later AND MIT AND Zlib
URL:            https://github.com/glikely/obs-ptz
Source0:        %{url}/archive/refs/tags/v%{version}/%{name}-%{version}.tar.gz

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

# Copied from obs-studio and QJoysticks and built into the plugin
Provides:       bundled(qjoysticks)
Provides:       bundled(obs-studio-qt-widgets)

%description
Pan, tilt and zoom control for PTZ cameras from within OBS Studio. Supports
VISCA over IP (UDP and TCP) and serial, Pelco, ONVIF, Sony discovery and USB
(UVC) cameras. Includes a controls dock, camera presets, a PTZ source and
filter, and optional joystick and game controller control.

%prep
%autosetup -p1

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
