1. pkg_extractor.exe
	
	Double click "Start Pkg Extractor.bat" "pkg-extractor-windows\pkg_extractor.exe" to open the GUI.
	Don't extract it out of the folder.
	Can be also called in command line.
	The first argument is the path to the pkg file to extract.
	The second argument is where to extract it.
	If no second argument is provided, it will be extracted next to the pkg file.

	Optional flags: --no-pause (never wait for Enter), --print-type (prints game, update or dlc),
	--check-type (legacy: exit code 101 game, 102 update, 103 dlc).
	Exit codes: 0 success, 1 failure, 2 invalid usage. Errors go to stderr.
	
	Alternatively, you can drag and drop the pkg file onto the exe and it will extract it at the same location.


2. install_pkg.bat

	Needs to be edited before use.
	Set the values for gamesDir and addonsDir to the ones used in ShadPS4 (you can check in settings > paths),
	or set the GAMES_DIR and ADDONS_DIR environment variables instead of editing the script.
	
	Can then be called in command line.
	The first argument is the path to the pkg file to extract.
	
	Alternatively, you can drag and drop the pkg file onto the bat and it will extract it where it needs to be.

	Pass --batch as second argument to never wait for Enter. Waiting only happens on an interactive terminal.
	Returns 0 on success, 1 on failure, 2 on configuration/usage errors.


3. batch_install.bat

	The first argument is a directory; every .pkg file in it is installed with install_pkg.bat.
	Returns 1 if no package was found or any installation failed.
