#!/usr/bin/env bash
set -e

# Usage: ./build_app_bundle.sh <binary_path> <app_name> <output_dir> [disc_path] [boot_exe]
BINARY_PATH="${1:-build/MimiPS1}"
APP_NAME="${2:-MimiPS1}"
OUTPUT_DIR="${3:-build}"
DISC_PATH="${4:-}"
BOOT_EXE="${5:-}"

APP_BUNDLE="${OUTPUT_DIR}/${APP_NAME}.app"
MACOS_DIR="${APP_BUNDLE}/Contents/MacOS"
RESOURCES_DIR="${APP_BUNDLE}/Contents/Resources"

echo "==> Creating macOS App Bundle: ${APP_BUNDLE}"
rm -rf "${APP_BUNDLE}"
mkdir -p "${MACOS_DIR}" "${RESOURCES_DIR}"

# Copy native binary
cp "${BINARY_PATH}" "${MACOS_DIR}/${APP_NAME}"
chmod +x "${MACOS_DIR}/${APP_NAME}"

# Optionally embed disc image for self-contained games
if [ -n "${DISC_PATH}" ] && [ -f "${DISC_PATH}" ]; then
    echo "==> Embedding game disc image into App Resources: $(basename "${DISC_PATH}")"
    cp "${DISC_PATH}" "${RESOURCES_DIR}/"
fi

# Optionally embed initial boot executable into App Resources
if [ -n "${BOOT_EXE}" ] && [ -f "${BOOT_EXE}" ]; then
    echo "==> Embedding boot executable into App Resources: boot.exe"
    cp "${BOOT_EXE}" "${RESOURCES_DIR}/boot.exe"
fi

# Generate Info.plist
cat << EOF > "${APP_BUNDLE}/Contents/Info.plist"
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleDevelopmentRegion</key>
    <string>en</string>
    <key>CFBundleExecutable</key>
    <string>${APP_NAME}</string>
    <key>CFBundleIdentifier</key>
    <string>com.mimips1.${APP_NAME}</string>
    <key>CFBundleInfoDictionaryVersion</key>
    <string>6.0</string>
    <key>CFBundleName</key>
    <string>${APP_NAME}</string>
    <key>CFBundlePackageType</key>
    <string>APPL</string>
    <key>CFBundleShortVersionString</key>
    <string>1.0.0</string>
    <key>CFBundleVersion</key>
    <string>1</string>
    <key>LSMinimumSystemVersion</key>
    <string>11.0</string>
    <key>NSHighResolutionCapable</key>
    <true/>
    <key>NSSupportsAutomaticGraphicsSwitching</key>
    <true/>
</dict>
</plist>
EOF

echo "APPL????" > "${APP_BUNDLE}/Contents/PkgInfo"

echo "==> Successfully created ${APP_BUNDLE}"
