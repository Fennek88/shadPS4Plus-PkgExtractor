#!/bin/bash
# Usage: install_pkg.sh <file.pkg> [--batch]
# GAMES_DIR / ADDONS_DIR environment variables override the settings below.
# Exit codes: 0 success, 1 failure, 2 usage/configuration error.

gamesDir="${GAMES_DIR:-PATH_TO_GAMES_DIR}"
addonsDir="${ADDONS_DIR:-PATH_TO_ADDONS_DIR}"

scriptDir="$(cd "$(dirname "$0")" && pwd)"
pkg="$1"
batch=0
[ "$2" == "--batch" ] && batch=1

finish() {
	if [ "$batch" -eq 0 ] && [ -t 0 ] && [ -t 1 ]; then
		echo "Press [enter] to close"
		read -r
	fi
	exit "$1"
}

if [ -z "$pkg" ]; then
	echo "Usage: $0 <file.pkg> [--batch]" >&2
	finish 2
fi

if [ "$gamesDir" == "PATH_TO_GAMES_DIR" ]; then
	echo "You need to update gamesDir with the games path used by ShadPs4 (or set GAMES_DIR)" >&2
	finish 2
fi

if [ "$addonsDir" == "PATH_TO_ADDONS_DIR" ]; then
	echo "You need to update addonsDir with the addons path used by ShadPs4 (or set ADDONS_DIR)" >&2
	finish 2
fi

type="$("$scriptDir/pkg_extractor" "$pkg" --print-type --no-pause)" || finish 1

case "$type" in
	game)
		echo "The file is a base game, installing to $gamesDir"
		target="$gamesDir" ;;
	update)
		echo "The file is a game update, installing to $gamesDir"
		target="$gamesDir" ;;
	dlc)
		echo "The file is a dlc, installing to $addonsDir"
		target="$addonsDir" ;;
	*)
		echo "Unexpected package type: $type" >&2
		finish 1 ;;
esac

"$scriptDir/pkg_extractor" "$pkg" "$target" --no-pause || finish 1
finish 0
