#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APP_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
PM3_DIR="${PM3_DIR:-$APP_DIR/../proxmark3}"
VERSION="${VERSION:-1.4.1}"
ARCH="${ARCH:-arm64}"
PKG_NAME="czpm3"
STAGE_DIR="$APP_DIR/build/deb-stage"
DIST_DIR="$APP_DIR/dist"

if [ ! -x "$APP_DIR/build/czpm3-gui" ]; then
    echo "Error: $APP_DIR/build/czpm3-gui not found. Build GUI first." >&2
    exit 1
fi

if [ ! -x "$PM3_DIR/client/proxmark3" ]; then
    echo "Error: $PM3_DIR/client/proxmark3 not found. Build Proxmark3 client first." >&2
    exit 1
fi

rm -rf "$STAGE_DIR"
mkdir -p "$DIST_DIR"
mkdir -p \
    "$STAGE_DIR/DEBIAN" \
    "$STAGE_DIR/usr/bin" \
    "$STAGE_DIR/usr/share/proxmark3" \
    "$STAGE_DIR/usr/share/APPLaunch/bin" \
    "$STAGE_DIR/usr/share/APPLaunch/applications" \
    "$STAGE_DIR/usr/share/APPLaunch/share/images" \
    "$STAGE_DIR/usr/share/doc/$PKG_NAME"

# Proxmark3 client binary and shared assets
install -m 0755 "$PM3_DIR/client/proxmark3" "$STAGE_DIR/usr/bin/proxmark3"
strip --strip-unneeded "$STAGE_DIR/usr/bin/proxmark3" 2>/dev/null || true
for d in dictionaries resources lualibs luascripts pyscripts cmdscripts; do
    if [ -d "$PM3_DIR/client/$d" ]; then
        cp -a "$PM3_DIR/client/$d" "$STAGE_DIR/usr/share/proxmark3/"
    fi
done

# CardputerZero GUI binary, wrapper, desktop file, icon, docs
install -m 0755 "$APP_DIR/build/czpm3-gui" "$STAGE_DIR/usr/share/APPLaunch/bin/czpm3-gui"
strip --strip-unneeded "$STAGE_DIR/usr/share/APPLaunch/bin/czpm3-gui" 2>/dev/null || true
install -m 0755 "$APP_DIR/packaging/czpm3" "$STAGE_DIR/usr/share/APPLaunch/bin/czpm3"
install -m 0644 "$APP_DIR/packaging/czpm3.desktop" "$STAGE_DIR/usr/share/APPLaunch/applications/czpm3.desktop"
install -m 0644 "$APP_DIR/assets/czpm3.png" "$STAGE_DIR/usr/share/APPLaunch/share/images/czpm3.png"
install -m 0644 "$APP_DIR/README.md" "$STAGE_DIR/usr/share/doc/$PKG_NAME/README.md"
install -m 0644 "$APP_DIR/patches/pm3-ble-cpz.patch" "$STAGE_DIR/usr/share/doc/$PKG_NAME/pm3-ble-cpz.patch"

PM3_COMMIT="${PM3_COMMIT:-$(git -C "$PM3_DIR" rev-parse --short HEAD 2>/dev/null || echo da929f9)}"

cat > "$STAGE_DIR/usr/share/doc/$PKG_NAME/copyright" <<EOF
Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/
Upstream-Name: czpm3
Source: https://github.com/MrBoogus/czpm3
Comment: Upstream Proxmark3 client from https://github.com/RfidResearchGroup/proxmark3 (commit ${PM3_COMMIT}; tested commit da929f9).

Files: *
Copyright: 2026 MrBoogus <MrBoogus@users.noreply.github.com>
License: GPL-3.0

Files: usr/bin/proxmark3 usr/share/proxmark3/* usr/share/doc/czpm3/pm3-ble-cpz.patch
Copyright: Proxmark3 contributors (RfidResearchGroup/proxmark3, commit ${PM3_COMMIT})
License: GPL-3.0

License: GPL-3.0
 This program is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, version 3 of the License.
 .
 On Debian systems, the full text of the GNU General Public License
 version 3 can be found in /usr/share/common-licenses/GPL-3.
EOF
chmod 0644 "$STAGE_DIR/usr/share/doc/$PKG_NAME/copyright"

INSTALLED_SIZE=$(du -sk "$STAGE_DIR/usr" | awk '{print $1}')

cat > "$STAGE_DIR/DEBIAN/control" <<EOF
Package: $PKG_NAME
Version: $VERSION
Architecture: $ARCH
Maintainer: MrBoogus <MrBoogus@users.noreply.github.com>
Section: APPLaunch
Priority: optional
Replaces: iceman-pm3
Conflicts: iceman-pm3
Installed-Size: $INSTALLED_SIZE
Depends: libc6, libstdc++6, libgcc-s1, libfreetype6, libpng16-16t64 | libpng16-16, libreadline8t64 | libreadline8, libbz2-1.0, liblz4-1, libssl3t64 | libssl3, libjansson4, zlib1g, libtinfo6, libxxhash0, libbluetooth3, libcap2-bin
Recommends: bluez
Description: CZPM3 - Proxmark3 RFID/NFC toolkit with native 320x170 GUI
 Native arm64 build of the RfidResearchGroup/proxmark3 client paired with a
 custom 320x170 Linux framebuffer + keyboard GUI tailored for the M5Stack
 CardputerZero APPLaunch environment, supporting USB serial, Proxmark5 BLE,
 and RDV4 BlueShark Classic Bluetooth.
EOF

cat > "$STAGE_DIR/DEBIAN/postinst" <<'EOF'
#!/bin/sh
set -e
PATH="$PATH:/usr/sbin:/sbin"
if [ -x /usr/bin/proxmark3 ]; then
    if ! getent group bluetooth >/dev/null 2>&1; then
        if command -v groupadd >/dev/null 2>&1; then
            groupadd -r bluetooth || true
        elif command -v addgroup >/dev/null 2>&1; then
            addgroup --system bluetooth || true
        fi
    fi
    if getent group bluetooth >/dev/null 2>&1; then
        chgrp bluetooth /usr/bin/proxmark3
    else
        echo "Warning: group 'bluetooth' missing; /usr/bin/proxmark3 group unchanged" >&2
    fi
    chmod 0750 /usr/bin/proxmark3
    if command -v setcap >/dev/null 2>&1; then
        setcap 'cap_net_raw,cap_net_admin+ep' /usr/bin/proxmark3 || \
            echo "Warning: setcap failed on /usr/bin/proxmark3; non-root BLE HCI features may require root" >&2
    else
        echo "Warning: setcap not found; /usr/bin/proxmark3 does not have cap_net_raw,cap_net_admin+ep capabilities" >&2
    fi
fi
exit 0
EOF

chmod 0755 "$STAGE_DIR/DEBIAN"
chmod 0644 "$STAGE_DIR/DEBIAN/control"
chmod 0755 "$STAGE_DIR/DEBIAN/postinst"

OUT_DEB="$DIST_DIR/${PKG_NAME}_${VERSION}_${ARCH}.deb"
dpkg-deb -Zgzip --build --root-owner-group "$STAGE_DIR" "$OUT_DEB"
echo "Built package: $OUT_DEB"
