#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build_dir="${repo_root}/linux/.build/amd64-release"
artifact_dir="${repo_root}/artifacts/linux"
package_version="${CLIPDECK_VERSION:-0.3.0~preview7}"

cmake -S "${repo_root}/linux" -B "${build_dir}" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/opt/clipdeck
cmake --build "${build_dir}" --parallel

stage_dir="$(mktemp -d /tmp/clipdeck-stage.XXXXXX)"
cleanup() {
  rm -rf "${stage_dir}"
}
trap cleanup EXIT

DESTDIR="${stage_dir}" cmake --build "${build_dir}" --target install

install -Dm755 "${repo_root}/linux/packaging/clipdeck-launcher" \
  "${stage_dir}/usr/bin/clipdeck"
install -Dm644 "${repo_root}/linux/packaging/clipdeck.desktop" \
  "${stage_dir}/usr/share/applications/clipdeck.desktop"
install -Dm644 "${repo_root}/mac/Assets.xcassets/AppIcon.appiconset/icon-256.png" \
  "${stage_dir}/usr/share/icons/hicolor/256x256/apps/clipdeck.png"
install -d "${stage_dir}/opt/clipdeck/lib" "${stage_dir}/opt/clipdeck/plugins"

qt_plugin_dir="$(qmake -qt=5 -query QT_INSTALL_PLUGINS)"
for plugin_group in platforms platforminputcontexts imageformats iconengines xcbglintegrations sqldrivers; do
  if [[ -d "${qt_plugin_dir}/${plugin_group}" ]]; then
    cp -a "${qt_plugin_dir}/${plugin_group}" "${stage_dir}/opt/clipdeck/plugins/"
  fi
done

mapfile -t qt_libraries < <(
  find \
    "${stage_dir}/opt/clipdeck/bin/clipdeck-linux" \
    "${stage_dir}/opt/clipdeck/plugins" \
    -type f -print0 \
    | xargs -0 file \
    | awk -F: '/ELF/ {print $1}' \
    | while IFS= read -r binary; do ldd "${binary}" 2>/dev/null || true; done \
    | awk '/=> \/.*libQt5[^ ]*\.so/ {print $3}' \
    | sort -u
)

for library in "${qt_libraries[@]}"; do
  cp -L "${library}" "${stage_dir}/opt/clipdeck/lib/$(basename "${library}")"
done

install -d "${stage_dir}/DEBIAN"
cat > "${stage_dir}/DEBIAN/control" <<EOF
Package: clipdeck
Version: ${package_version}
Section: utils
Priority: optional
Architecture: amd64
Maintainer: ClipDeck Contributors
Depends: ca-certificates, libc6 (>= 2.28), libdbus-1-3, libfontconfig1, libfreetype6, libgcc1, libgl1, libglib2.0-0, libharfbuzz0b, libice6, libjpeg62-turbo, libpng16-16, libsm6, libstdc++6, libx11-6, libx11-xcb1, libxdamage1, libxext6, libxfixes3, libxi6, libxrender1, libxtst6, libxcb1, libxcb-icccm4, libxcb-image0, libxcb-keysyms1, libxcb-randr0, libxcb-render-util0, libxcb-shape0, libxcb-shm0, libxcb-util0, libxcb-xfixes0, libxcb-xinerama0, libxcb-xkb1, libxkbcommon0, libxkbcommon-x11-0
Description: ClipDeck clipboard history for UOS
 A native clipboard history Deck for UOS with persistent text and image history,
 automatic categories, search, editing and X11 automatic paste through Ctrl+V.
EOF

mkdir -p "${artifact_dir}"
package_path="${artifact_dir}/clipdeck_${package_version}_amd64.deb"
dpkg-deb --root-owner-group --build "${stage_dir}" "${package_path}"

dpkg-deb --info "${package_path}"
file "${stage_dir}/opt/clipdeck/bin/clipdeck-linux"
sha256sum "${package_path}"
