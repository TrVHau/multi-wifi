#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
BUILD_DIR="${1:-${ROOT_DIR}/build}"
APP_DIR="${2:-${BUILD_DIR}/AppDir}"

echo ":: Building portable GUI AppDir..."
rm -rf "${APP_DIR}"
mkdir -p "${APP_DIR}/usr/bin"
mkdir -p "${APP_DIR}/usr/share/applications"
mkdir -p "${APP_DIR}/usr/share/icons/hicolor/scalable/apps"

# Copy unprivileged GUI binary
cp "${BUILD_DIR}/multiwifi-gui" "${APP_DIR}/usr/bin/"
chmod 0755 "${APP_DIR}/usr/bin/multiwifi-gui"

# Copy desktop and app icon
cp "${ROOT_DIR}/data/io.github.multiwifi.MultiWiFi.desktop" "${APP_DIR}/"
cp "${ROOT_DIR}/data/io.github.multiwifi.MultiWiFi.desktop" "${APP_DIR}/usr/share/applications/"

# Copy AppRun
cp "${SCRIPT_DIR}/AppRun" "${APP_DIR}/"
chmod 0755 "${APP_DIR}/AppRun"

echo ":: GUI AppDir staged successfully at ${APP_DIR}."
echo ":: AppImage contains only unprivileged GUI components; no system daemon or privileged route tools included."
