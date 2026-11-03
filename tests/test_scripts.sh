#!/bin/bash
# Usage: test_scripts.sh <pkg_extractor binary> <repo dir> [small.pkg]
# Tests CLI exit codes and helper script behaviour. The pkg is optional.
bin="$1"; repo="$2"; pkg="$3"
fail=0
check() { if [ "$2" != "$3" ]; then echo "FAIL: $1 (expected $2, got $3)"; fail=1; fi; }

tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
echo "not a pkg" > "$tmp/bad.pkg"

"$bin" --help >/dev/null; check "help" 0 $?
"$bin" --bogus >/dev/null 2>&1; check "unknown option" 2 $?
"$bin" "$tmp/bad.pkg" "$tmp/out" --no-pause >/dev/null 2>&1 </dev/null; check "invalid pkg" 1 $?
"$bin" "$tmp/bad.pkg" --print-type >/dev/null 2>&1; check "print-type invalid" 1 $?

mkdir "$tmp/s" "$tmp/g" "$tmp/a" "$tmp/empty" "$tmp/pk"
cp "$repo/install_pkg.sh" "$repo/batch_install.sh" "$tmp/s/"
ln -s "$bin" "$tmp/s/pkg_extractor"

"$tmp/s/install_pkg.sh" </dev/null >/dev/null 2>&1; check "script no args" 2 $?
"$tmp/s/install_pkg.sh" "$tmp/bad.pkg" </dev/null >/dev/null 2>&1; check "script unconfigured" 2 $?
GAMES_DIR="$tmp/g" ADDONS_DIR="$tmp/a" "$tmp/s/install_pkg.sh" "$tmp/bad.pkg" </dev/null >/dev/null 2>&1; check "script bad pkg" 1 $?
"$tmp/s/batch_install.sh" "$tmp/missing" </dev/null >/dev/null 2>&1; check "batch missing dir" 2 $?
GAMES_DIR="$tmp/g" ADDONS_DIR="$tmp/a" "$tmp/s/batch_install.sh" "$tmp/empty" </dev/null >/dev/null 2>&1; check "batch empty" 1 $?
cp "$tmp/bad.pkg" "$tmp/pk/"
GAMES_DIR="$tmp/g" ADDONS_DIR="$tmp/a" "$tmp/s/batch_install.sh" "$tmp/pk" </dev/null >/dev/null 2>&1; check "batch failure" 1 $?

if [ -n "$pkg" ] && [ -f "$pkg" ]; then
	rm "$tmp/pk/bad.pkg"; cp "$pkg" "$tmp/pk/"
	type="$("$bin" "$pkg" --print-type)"; check "print-type exit" 0 $?
	[ -n "$type" ] || { echo "FAIL: empty type"; fail=1; }
	GAMES_DIR="$tmp/g" ADDONS_DIR="$tmp/a" "$tmp/s/batch_install.sh" "$tmp/pk" </dev/null >/dev/null 2>&1; check "batch install" 0 $?
	[ -n "$(find "$tmp/g" "$tmp/a" -type f | head -1)" ] || { echo "FAIL: no files installed"; fail=1; }
fi

[ "$fail" -eq 0 ] && echo "script tests passed"
exit $fail
