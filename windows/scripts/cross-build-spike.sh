#!/usr/bin/env bash
# Runs inside clipdeck-windows-builder: builds windows/spike, assembles a
# self-contained folder (windeployqt cannot run on Linux), zips it and runs a
# headless Wine smoke test.
set -euo pipefail

repo_root="${PWD}"
source_dir="windows/spike"
build_dir="windows/.build/spike"
stage_dir="${build_dir}/ClipDeckSpike"
output_dir="artifacts/windows"
qt_win="${QT_WINDOWS_PATH:?}"
mingw_sysroot="/usr/x86_64-w64-mingw32"
gcc_runtime="$(dirname "$(x86_64-w64-mingw32-g++-posix -print-libgcc-file-name)")"

cmake -S "${source_dir}" -B "${build_dir}" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="${PWD}/windows/cmake/mingw-w64-x86_64.cmake" \
  -DCMAKE_PREFIX_PATH="${qt_win}" \
  -DQT_HOST_PATH="${QT_HOST_PATH:?}"
cmake --build "${build_dir}"

rm -rf "${stage_dir}"
mkdir -p "${stage_dir}/plugins" "${stage_dir}/qml"
cp "${build_dir}/clipdeck-spike.exe" "${stage_dir}/"
cp -r "${qt_win}/plugins/platforms" "${stage_dir}/plugins/"
for module in QtQuick QtQml; do
  cp -r "${qt_win}/qml/${module}" "${stage_dir}/qml/"
done
# Controls, dialogs and other heavy QML modules are not used by the spike.
rm -rf "${stage_dir}/qml/QtQuick/"{Controls,Dialogs,NativeStyle,Scene2D,Scene3D,VirtualKeyboard,Pdf,LocalStorage,Particles,Timeline,Templates,tooling}
printf '[Paths]\nPrefix=.\nPlugins=plugins\nQmlImports=qml\n' > "${stage_dir}/qt.conf"

# Copy every DLL the executable and the copied plugins import, transitively.
declare -A seen=()
resolve() {
  local binary="$1" dll candidate
  while read -r dll; do
    local key="${dll,,}"
    [[ -n "${seen[$key]:-}" ]] && continue
    seen[$key]=1
    for candidate in "${qt_win}/bin/${dll}" "${gcc_runtime}/${dll}" "${mingw_sysroot}/lib/${dll}" "${mingw_sysroot}/bin/${dll}"; do
      if [[ -f "${candidate}" ]]; then
        cp -n "${candidate}" "${stage_dir}/"
        resolve "${candidate}"
        break
      fi
    done
  done < <(x86_64-w64-mingw32-objdump -p "${binary}" | sed -n 's/^\s*DLL Name: //p')
}
resolve "${stage_dir}/clipdeck-spike.exe"
while IFS= read -r -d '' plugin; do
  resolve "${plugin}"
done < <(find "${stage_dir}/plugins" "${stage_dir}/qml" -name '*.dll' -print0)

cp windows/spike/TESTING.md "${stage_dir}/"
mkdir -p "${output_dir}"
rm -f "${output_dir}/ClipDeckSpike.zip"
(cd "${build_dir}" && zip -qr "${repo_root}/${output_dir}/ClipDeckSpike.zip" ClipDeckSpike)
echo "Packaged ${output_dir}/ClipDeckSpike.zip ($(du -sh "${stage_dir}" | cut -f1) unpacked)"

if [[ "${CLIPDECK_SKIP_WINE:-0}" != "1" ]]; then
  export WINEPREFIX="/tmp/wineprefix" WINEDLLOVERRIDES="mscoree,mshtml=" HOME="/tmp"
  wineboot --init >/dev/null 2>&1 || true
  for rhi in opengl software; do
    echo "--- Wine smoke test (rhi=${rhi}) ---"
    log="/tmp/wine-${rhi}.log"
    status=0
    xvfb-run -a -s "-screen 0 1920x1080x24" \
      wine "${stage_dir}/clipdeck-spike.exe" --self-test "--rhi=${rhi}" >"${log}" 2>&1 || status=$?
    grep -E '\[(env|qml|qt\.(warning|critical|fatal)|selftest|benchmark|effect|focus)\]' "${log}" || tail -20 "${log}"
    echo "exit=${status}"
  done
fi
