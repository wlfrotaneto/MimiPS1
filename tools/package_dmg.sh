#!/usr/bin/env bash
set -e

# Usage: ./package_dmg.sh <app_path> <dmg_name> <output_dir>
APP_PATH="${1:-build/MimiPS1.app}"
DMG_NAME="${2:-MimiPS1-macOS-AppleSilicon}"
OUTPUT_DIR="${3:-dist}"

if [ ! -d "${APP_PATH}" ]; then
    echo "[Error] App bundle not found at: ${APP_PATH}"
    exit 1
fi

mkdir -p "${OUTPUT_DIR}"
DMG_OUTPUT="${OUTPUT_DIR}/${DMG_NAME}.dmg"
STAGING_DIR=$(mktemp -d /tmp/mimips1_dmg.XXXXXX)

echo "==> Staging DMG contents in ${STAGING_DIR}"
cp -R "${APP_PATH}" "${STAGING_DIR}/"

# Create /Applications symlink for drag-and-drop install
ln -s /Applications "${STAGING_DIR}/Applications"

echo "==> Building compressed DMG disk image using hdiutil..."
rm -f "${DMG_OUTPUT}"

hdiutil create -volname "${DMG_NAME}" \
               -srcfolder "${STAGING_DIR}" \
               -ov -format UDZO \
               "${DMG_OUTPUT}"

# Clean staging directory
rm -rf "${STAGING_DIR}"

echo "=========================================================="
echo " [SUCCESS] Apple Silicon DMG created successfully!"
echo " Disk Image: ${DMG_OUTPUT}"
echo " Ready to install or distribute for macOS Apple Silicon!"
echo "=========================================================="
