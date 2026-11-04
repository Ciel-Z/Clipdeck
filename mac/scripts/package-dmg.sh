#!/bin/zsh

# Builds a universal (Apple silicon + Intel) Release ClipDeck.app and wraps it
# in a drag-to-Applications DMG: dist/ClipDeck-<MARKETING_VERSION>-macos.dmg.

set -euo pipefail

project_root="${0:A:h:h}"
derived_data="$project_root/.build/XcodeDerivedData-Release"
dist="$project_root/dist"

xcodebuild \
  -project "$project_root/ClipDeck.xcodeproj" \
  -scheme ClipDeck \
  -configuration Release \
  -derivedDataPath "$derived_data" \
  ARCHS="arm64 x86_64" \
  ONLY_ACTIVE_ARCH=NO \
  CODE_SIGN_IDENTITY=- \
  build

app="$derived_data/Build/Products/Release/ClipDeck.app"
version=$(/usr/libexec/PlistBuddy -c "Print :CFBundleShortVersionString" "$app/Contents/Info.plist")
dmg="$dist/ClipDeck-$version-macos.dmg"

staging=$(mktemp -d)
trap 'rm -rf "$staging"' EXIT
ditto "$app" "$staging/ClipDeck.app"
ln -s /Applications "$staging/Applications"

mkdir -p "$dist"
rm -f "$dmg"
hdiutil create -volname "ClipDeck $version" -srcfolder "$staging" -fs HFS+ -format UDZO -ov "$dmg"
echo "$dmg"
