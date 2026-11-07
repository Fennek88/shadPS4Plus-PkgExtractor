@echo off
rem Usage: install_pkg.bat file.pkg [--batch]
rem GAMES_DIR / ADDONS_DIR environment variables override the settings below.
rem Exit codes: 0 success, 1 failure, 2 usage/configuration error.
setlocal EnableDelayedExpansion

set "pkg=%~1"
set "par=%~2"
set "gamesDir=%GAMES_DIR%"
set "addonsDir=%ADDONS_DIR%"
if not defined gamesDir set "gamesDir=PATH_TO_GAMES_DIR"
if not defined addonsDir set "addonsDir=PATH_TO_ADDONS_DIR"
set "extractor=%~dp0pkg_extractor_cli.exe"
set "rc=0"

if "%pkg%"=="" (
	echo Usage: install_pkg.bat file.pkg [--batch] 1>&2
	set "rc=2"
	goto :done
)

if "%gamesDir%"=="PATH_TO_GAMES_DIR" (
	echo You need to update gamesDir with the games path used by ShadPs4 ^(or set GAMES_DIR^) 1>&2
	set "rc=2"
	goto :done
)

if "%addonsDir%"=="PATH_TO_ADDONS_DIR" (
	echo You need to update addonsDir with the addons path used by ShadPs4 ^(or set ADDONS_DIR^) 1>&2
	set "rc=2"
	goto :done
)

set "pkgType="
for /f "usebackq delims=" %%t in (`""%extractor%" "%pkg%" --print-type --no-pause""`) do set "pkgType=%%t"
if errorlevel 1 (
	set "rc=1"
	goto :done
)

if "!pkgType!"=="game" (
	echo The file is a base game, installing to !gamesDir!
	set "target=!gamesDir!"
) else if "!pkgType!"=="update" (
	echo The file is a game update, installing to !gamesDir!
	set "target=!gamesDir!"
) else if "!pkgType!"=="dlc" (
	echo The file is a dlc, installing to !addonsDir!
	set "target=!addonsDir!"
) else (
	echo Unexpected package type: !pkgType! 1>&2
	set "rc=1"
	goto :done
)

"%extractor%" "%pkg%" "!target!" --no-pause
if errorlevel 1 set "rc=1"

:done
if not "%par%"=="--batch" if not defined CI pause
exit /b %rc%
