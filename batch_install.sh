#!/bin/bash
# Usage: batch_install.sh <directory-with-pkgs>
# Exit codes: 0 all installed, 1 at least one failure, 2 usage error.

scriptDir="$(cd "$(dirname "$0")" && pwd)"
dir="$1"

if [ -z "$dir" ] || [ ! -d "$dir" ]; then
	echo "Usage: $0 <directory-with-pkgs>" >&2
	exit 2
fi

shopt -s nullglob
files=("$dir"/*.pkg)
if [ "${#files[@]}" -eq 0 ]; then
	echo "No .pkg files found in $dir" >&2
	exit 1
fi

failed=0
for f in "${files[@]}"; do
	"$scriptDir/install_pkg.sh" "$f" --batch || { echo "Failed: $f" >&2; failed=$((failed + 1)); }
done

echo "Batch install done: $((${#files[@]} - failed)) succeeded, $failed failed"

if [ -t 0 ] && [ -t 1 ]; then
	echo "Press [enter] to close"
	read -r
fi
[ "$failed" -eq 0 ]
