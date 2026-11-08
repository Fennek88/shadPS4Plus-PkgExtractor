#!/bin/bash
set -euo pipefail

if [[ $# != 2 && $# != 3 ]]; then
    echo "Usage: $0 app-bundle qt-prefix [https-url]" >&2
    exit 2
fi

app="$(cd "$1" && pwd -P)"
qt="$2"
source_dir="$(cd "$(dirname "$0")" && pwd -P)"
work="$(mktemp -d "${TMPDIR:-/tmp}/pkg-tls-test-XXXXXXXX")"
cleanup() {
    rm -f "$work/probe.o" "$work/Contents/MacOS/probe" "$work/Contents/Frameworks"
    rmdir "$work/Contents/MacOS" "$work/Contents" "$work"
}
trap cleanup EXIT
mkdir -p "$work/Contents/MacOS"
ln -s "$app/Contents/Frameworks" "$work/Contents/Frameworks"
clang++ -std=c++23 -isystem "$qt/include/QtCore" -isystem "$qt/include/QtNetwork" \
    -iframework "$qt/lib" -idirafter "$qt/include" -c "$source_dir/mac_tls_probe.cpp" -o "$work/probe.o"
probe="$work/Contents/MacOS/probe"
clang++ "$work/probe.o" -F"$work/Contents/Frameworks" -framework QtCore -framework QtNetwork \
    -Wl,-rpath,@executable_path/../Frameworks -o "$probe"
for framework in QtCore QtNetwork; do
    dependency="$(otool -L "$probe" | awk -v name="$framework.framework/" \
        'index($1, name) { print $1; exit }')"
    if [[ -z "$dependency" ]]; then
        echo "Missing $framework dependency in the TLS probe." >&2
        exit 1
    fi
    install_name_tool -change "$dependency" \
        "@executable_path/../Frameworks/$framework.framework/Versions/A/$framework" "$probe"
done
codesign --force --sign - "$probe"
"$probe" "$app/Contents/PlugIns" "${@:3}"
