#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
[ "$(uname -s)" = Darwin ] || { echo 'Build the Mac app on macOS.' >&2; exit 1; }
[ "$(uname -m)" = arm64 ] || { echo 'This release targets Apple Silicon.' >&2; exit 1; }
version=$(cat packaging/VERSION)
case "$version" in *[!0-9.]*|'') echo 'Invalid packaging version' >&2; exit 1;; esac
sh ./shared_ui/deploy.sh build
swift build --package-path mac_sender -c release --arch arm64
binary_dir=$(swift build --package-path mac_sender -c release --arch arm64 --show-bin-path)
staging=$(mktemp -d "${TMPDIR:-/tmp}/thunderlink-package.XXXXXX")
trap 'rm -rf "$staging"' EXIT HUP INT TERM
app="$staging/ThunderLink.app"
mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources" dist
cp "$binary_dir/ThunderLinkSender" "$app/Contents/MacOS/ThunderLink"
cp "$binary_dir/ThunderLinkPowerHelper" "$app/Contents/MacOS/ThunderLinkPowerHelper"
cp packaging/ThunderLink.icns "$app/Contents/Resources/ThunderLink.icns"
cp packaging/licenses/THIRD-PARTY-NOTICES.txt "$app/Contents/Resources/"
cp shared_ui/licenses/Inter-OFL.txt "$app/Contents/Resources/"
cp shared_ui/licenses/Lit-BSD-3-Clause.txt "$app/Contents/Resources/"
cp shared_ui/dist/manifest.json "$app/Contents/Resources/ui-manifest.json"
cp packaging/VERSION "$app/Contents/Resources/VERSION"
ui_bundle=$(find "$binary_dir" -maxdepth 1 -type d -name '*ThunderLinkUIResources*.bundle' -print -quit)
[ -n "$ui_bundle" ] || { echo 'Shared UI SwiftPM resource bundle missing. Rebuild shared_ui first.' >&2; exit 1; }
cp -R "$ui_bundle" "$app/Contents/Resources/"
sed "s/@VERSION@/$version/g" packaging/macos/Info.plist.in > "$app/Contents/Info.plist"
plutil -lint "$app/Contents/Info.plist"
# macOS 13 provides Swift and all linked Apple frameworks. Reject accidental
# third-party/build-machine dependencies rather than shipping a broken bundle.
for executable in ThunderLink ThunderLinkPowerHelper; do
    otool -L "$app/Contents/MacOS/$executable" | tail -n +2 | while IFS= read -r dependency; do
        case "$dependency" in *'/System/Library/'*|*'/usr/lib/'*) ;; *) echo "Unbundled dependency: $dependency" >&2; exit 1;; esac
    done
done
codesign --force --sign - "$app/Contents/MacOS/ThunderLinkPowerHelper"
codesign --force --sign - "$app"
codesign --verify --strict --verbose=2 "$app"
archive="dist/ThunderLink-$version-macos-arm64.zip"
ditto -c -k --sequesterRsrc --keepParent "$app" "$archive"
# Keep an immediately launchable copy; ditto updates only generated output.
ditto "$app" dist/ThunderLink.app
shasum -a 256 "$archive" > "$archive.sha256"
echo "Created $archive and dist/ThunderLink.app"
