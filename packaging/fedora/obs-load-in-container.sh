#!/bin/bash
# Runs inside the container started by obs-load-test.sh: install obs-studio and
# the built RPM, then run OBS on a virtual display with a window manager and a
# VNC server. The host script drives and inspects it with `docker exec`.
exec > /tmp/setup.log 2>&1
set -x

dnf -y install --setopt=install_weak_deps=False obs-studio \
    $(ls /rpms/obs-studio-plugin-ptz-[0-9]*.rpm | grep -v -e src.rpm -e debug) \
    xorg-x11-server-Xvfb openbox x11vnc xdotool ImageMagick mesa-dri-drivers \
    mesa-libGL dbus-x11 dejavu-sans-fonts glibc-langpack-en procps-ng \
    || { echo SETUP-FAILED > /tmp/state; exit 1; }
rpm -q obs-studio obs-studio-plugin-ptz

useradd -m obs
# Skip OBS's first-run auto-configuration wizard
install -d -o obs -g obs /home/obs/.config/obs-studio
printf '[General]\nFirstRun=true\n' > /home/obs/.config/obs-studio/user.ini
chown -R obs:obs /home/obs/.config

Xvfb :99 -screen 0 1600x900x24 -ac &
sleep 2
runuser -u obs -- env DISPLAY=:99 HOME=/home/obs openbox &
# No password: the host script only publishes this port on localhost
x11vnc -display :99 -forever -shared -nopw -rfbport 5900 -bg -o /tmp/x11vnc.log
runuser -u obs -- env DISPLAY=:99 HOME=/home/obs LIBGL_ALWAYS_SOFTWARE=1 \
    QT_QPA_PLATFORM=xcb dbus-run-session -- obs --disable-shutdown-check --verbose \
    > /tmp/obs.out 2>&1 &
echo READY > /tmp/state
wait
