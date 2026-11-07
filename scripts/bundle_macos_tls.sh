#!/bin/bash
set -euo pipefail

if [[ $# != 2 ]]; then
    echo "Usage: $0 app-bundle qt-prefix" >&2
    exit 2
fi

app="$1"
qt="$2"
plugin_dir="$("$qt/bin/qtpaths" --plugin-dir)"
source="$plugin_dir/tls/libqsecuretransportbackend.dylib"
target="$app/Contents/PlugIns/tls/libqsecuretransportbackend.dylib"
if [[ ! -f "$source" || ! -d "$app/Contents/Frameworks/QtNetwork.framework" ]]; then
    echo "Qt Secure Transport plugin or bundled Qt Network framework is missing." >&2
    exit 1
fi
mkdir -p "$(dirname "$target")"
cp "$source" "$target"
chmod u+w "$target"
for framework in QtCore QtNetwork; do
    dependency="$(otool -L "$target" | awk -v name="$framework.framework/" \
        'index($1, name) { print $1; exit }')"
    if [[ -z "$dependency" ]]; then
        echo "Missing $framework dependency in the Qt TLS plugin." >&2
        exit 1
    fi
    install_name_tool -change "$dependency" \
        "@executable_path/../Frameworks/$framework.framework/Versions/A/$framework" "$target"
done
codesign --force --sign - "$target"
codesign --force --sign - "$app"
codesign --verify --deep --strict "$app"
