#!/usr/bin/env bash
# Builds every app listed in apps.txt:
#   dist/<App>.exe           portable, statically linked exe
#   dist/<App>-Setup.exe     NSIS installer (needs makensis), unique AppId per app
#   dist/AfterburnerSuite-<ver>.zip   everything above
# Debian/Ubuntu: sudo apt install g++-mingw-w64-x86-64 nsis python3 zip
#
#   ./build.sh            build all apps
#   ./build.sh GPUFanTracer   build only the named app(s)
set -euo pipefail
cd "$(dirname "$0")"
VERSION=2.1.0
VERC=${VERSION//./,},0
CXX=${CXX:-x86_64-w64-mingw32-g++}
WINDRES=${WINDRES:-x86_64-w64-mingw32-windres}
mkdir -p build dist

trim() { local s="$1"; s="${s#"${s%%[![:space:]]*}"}"; echo "${s%"${s##*[![:space:]]}"}"; }

built=()
while IFS='|' read -r key name aumid guid src flags accent libs; do
    key=$(trim "$key"); [[ -z "$key" || "$key" == \#* ]] && continue
    name=$(trim "$name"); aumid=$(trim "$aumid"); guid=$(trim "$guid")
    src=$(trim "$src"); flags=$(trim "$flags"); accent=$(trim "$accent"); libs=$(trim "$libs")
    if [[ $# -gt 0 && ! " $* " =~ " $key " ]]; then continue; fi

    # identity must match the source, otherwise two apps could share one
    if ! grep -q "L\"$aumid\"" "$src"; then
        echo "error: $src does not declare AppUserModelID $aumid" >&2; exit 1
    fi
    echo "==> $key  ($aumid)"
    python3 tools/gen_icon.py "build/$key.ico" "$accent"
    sed -e "s/@KEY@/$key/g" -e "s/@NAME@/$name/g" -e "s/@VER@/$VERSION/g" -e "s/@VERC@/$VERC/g" -e "s/@AUMID@/$aumid/g" \
        res/app.rc.in > "build/$key.rc"
    sed -e "s/@KEY@/$key/g" -e "s/@NAME@/$name/g" -e "s/@VER@/$VERSION/g" res/app.manifest.in > "build/$key.manifest"
    $WINDRES -I build "build/$key.rc" -O coff -o "build/$key.res"
    # shellcheck disable=SC2086
    $CXX -std=c++17 -O2 -s -municode -mwindows -static -static-libgcc -static-libstdc++ \
        -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
        -Isrc $flags "$src" "build/$key.res" \
        -lgdiplus -lcomdlg32 -lgdi32 -luser32 -lshell32 -lole32 -luuid -lpropsys $libs \
        -o "dist/$key.exe"
    if command -v makensis >/dev/null; then
        makensis -V2 -DAPPKEY="$key" -DAPPNAME="$name" -DAPPGUID="$guid" -DVERSION="$VERSION" \
            -DSRCDIR="$PWD/dist" -DICON="$PWD/build/$key.ico" -DOUTFILE="$PWD/dist/$key-Setup.exe" installer/app.nsi
    fi
    built+=("$key")
done < apps.txt

if [[ $# -eq 0 ]] && command -v zip >/dev/null; then
    rm -f dist/AfterburnerSuite-*.zip
    (cd dist && zip -q -9 "AfterburnerSuite-$VERSION.zip" ./*.exe)
fi
echo "Built: ${built[*]}"
ls -la dist
