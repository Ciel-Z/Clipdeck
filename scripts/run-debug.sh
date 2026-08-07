#!/bin/zsh

set -euo pipefail

project_root="${0:A:h:h}"
derived_data="$project_root/.build/XcodeDerivedData"

xcodebuild \
  -project "$project_root/ClipDeck.xcodeproj" \
  -scheme ClipDeck \
  -configuration Debug \
  -derivedDataPath "$derived_data" \
  CODE_SIGN_IDENTITY=- \
  build

open "$derived_data/Build/Products/Debug/ClipDeck.app"
