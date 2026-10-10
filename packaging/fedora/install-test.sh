#!/bin/bash
# Install the RPMs from ./rpms-out into a clean Fedora container with
# obs-studio and check the plugin lands where OBS looks for it.
#   packaging/fedora/install-test.sh [fedora-version]   (default: rawhide)
set -euo pipefail
cd "$(dirname "$0")"
ver=${1:-rawhide}
base=${BASE_IMAGE:-registry.fedoraproject.org/fedora}
cat > rpms-out/install-check.sh <<'INNER'
set -euxo pipefail
dnf -y install --setopt=install_weak_deps=False /out/obs-ptz-[0-9]*.aarch64.rpm /out/obs-ptz-[0-9]*.x86_64.rpm 2>/dev/null \
    || dnf -y install --setopt=install_weak_deps=False $(ls /out/obs-ptz-[0-9]*.rpm | grep -v src.rpm)
rpm -q obs-studio obs-ptz
# Where does this OBS look for plugins? Compare to the owning rpm's files.
plugin=$(rpm -ql obs-ptz | grep '/obs-plugins/obs-ptz\.so$')
data=$(rpm -ql obs-ptz | grep -m1 '/obs/obs-plugins/obs-ptz$')
echo "plugin: $plugin"; echo "data:   $data"
# obs-studio's own plugins/data, for comparison (may live in obs-studio-libs)
rpm -ql $(rpm -qa 'obs-studio*') | grep -E '/obs-plugins/[a-z-]+\.so$|/share/obs/obs-plugins/[a-z-]+$' | head -6 || true
rpm -ql $(rpm -qa 'obs-studio*') | grep -E '/(lib|lib64)/obs-plugins/' | head -1 | grep -q "$(dirname "$plugin")"
ldd -r "$plugin" | grep -E 'not found|undefined' && exit 1
test -f "$data/locale/en-US.ini"
echo "install check OK"
INNER
docker run --rm -v "$PWD/rpms-out":/out "$base:$ver" bash /out/install-check.sh
rm -f rpms-out/install-check.sh
