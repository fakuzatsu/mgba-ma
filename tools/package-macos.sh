#!/usr/bin/env bash
set -euo pipefail

BUILD_DIR=${1:?usage: package-macos.sh BUILD_DIR OUTPUT_DIR ASSET_NAME}
OUTPUT_DIR=${2:?usage: package-macos.sh BUILD_DIR OUTPUT_DIR ASSET_NAME}
ASSET_NAME=${3:?usage: package-macos.sh BUILD_DIR OUTPUT_DIR ASSET_NAME}
APP="$BUILD_DIR/qt/mGBA.app"

if [[ ! -d $APP ]]; then
	echo "Application bundle not found: $APP" >&2
	exit 1
fi

macdeployqt=${MACDEPLOYQT:-}
if [[ -z $macdeployqt ]]; then
	for candidate in macdeployqt macdeployqt-qt5; do
		if command -v "$candidate" >/dev/null 2>&1; then
			macdeployqt=$(command -v "$candidate")
			break
		fi
	done
fi
if [[ -z $macdeployqt ]]; then
	echo "Please install macdeployqt or set MACDEPLOYQT" >&2
	exit 1
fi

package_root=$(mktemp -d "${TMPDIR:-/tmp}/mgba-package.XXXXXX")
cleanup() {
	if [[ -n ${package_root:-} && -d $package_root ]]; then
		rm -rf "$package_root"
	fi
}
trap cleanup EXIT

# The mGBA install rules first fix up non-Qt dependencies in the build-tree
# bundle, then copy that completed bundle and the application data here.
cmake --install "$BUILD_DIR" --prefix "$package_root" --component mgba
cmake --install "$BUILD_DIR" --prefix "$package_root" --component mgba-qt

packaged_app="$package_root/mGBA.app"
"$macdeployqt" "$packaged_app" -always-overwrite

if [[ ! -f $packaged_app/Contents/PlugIns/platforms/libqcocoa.dylib ]]; then
	echo "The packaged app is missing the Qt Cocoa platform plugin" >&2
	exit 1
fi

# This is an ad-hoc signature. It restores bundle integrity after dependency
# rewriting but does not identify the publisher or suppress Gatekeeper.
codesign --force --deep --sign - "$packaged_app"
codesign --verify --deep --strict --verbose=2 "$packaged_app"

ln -s /Applications "$package_root/Applications"
mkdir -p "$OUTPUT_DIR"
hdiutil create \
	-volname "$ASSET_NAME" \
	-srcfolder "$package_root" \
	-ov \
	-format UDZO \
	"$OUTPUT_DIR/$ASSET_NAME.dmg"
