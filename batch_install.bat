@echo off
rem Usage: batch_install.bat directory-with-pkgs
rem Exit codes: 0 all installed, 1 at least one failure, 2 usage error.
setlocal EnableDelayedExpansion

set "dir=%~1"
if "%dir%"=="" (
	echo Usage: batch_install.bat directory-with-pkgs 1>&2
	exit /b 2
)
if not exist "%dir%\" (
	echo Directory not found: %dir% 1>&2
	exit /b 2
)

set /a total=0
set /a failed=0
for %%i in ("%dir%\*.pkg") do (
	set /a total+=1
	call "%~dp0install_pkg.bat" "%%~i" --batch
	if errorlevel 1 (
		echo Failed: %%~i 1>&2
		set /a failed+=1
	)
)

if !total!==0 (
	echo No .pkg files found in %dir% 1>&2
	set /a failed=1
)

echo Batch install done: !failed! failed

if not defined CI pause
if !failed! GTR 0 exit /b 1
exit /b 0
