# shadPS4Plus PKG Extractor

A desktop and command-line tool for extracting PlayStation 4 PKG files for use with PS Emulators or Recomp Tools. The project is a rework of [AzaharPlus/shadPS4Plus](https://github.com/AzaharPlus/shadPS4Plus), with a Qt interface and multilingual support.

![shadPS4Plus PKG Extractor window](images/screenshots/screenshot-linux-01.png)

## Features

- Extract base-game, game-update, and DLC PKG files.
- Use the Qt desktop interface or run the extractor from a terminal.
- In the GUI, choose a PKG-file and output directory and start extraction.
- Automatically classify package content and create the corresponding output layout:
  - Base game: `<output>/<title-id>/`
  - Update: `<output>/<title-id>-patch/`
  - DLC: `<output>/<title-id>/<content-id-part>/`

## Usage

### Desktop interface

Run `pkg_extractor` (Linux), `pkg_extractor.exe` (Windows), or the `pkg_extractor.app` bundle (macOS) without arguments. Select a PKG file and an output directory, then click **Extract**. The output directory is the root under which the package-specific layout is created.

### Command line

Pass the PKG path followed by an optional output directory:

```sh
./pkg_extractor /path/to/game.pkg /path/to/output
```

On Windows:

```powershell
.\pkg_extractor.exe C:\path\to\game.pkg C:\path\to\output
```

If the output directory is omitted, files are extracted beneath the current working directory. The extractor creates the package-specific subdirectory described above.

### Optional ShadPS4 install scripts

`install_pkg.sh` and `install_pkg.bat` classify a package and route base games and updates to the configured games directory and DLC to the configured add-ons directory. Before using either script, edit its `gamesDir` and `addonsDir` values to match the paths configured in ShadPS4. The `batch_install.sh` and `batch_install.bat` helpers apply the install script to PKG files in a directory.

The scripts are automation-safe: instead of editing them you can set the `GAMES_DIR` and `ADDONS_DIR` environment variables, they only wait for Enter on an interactive terminal (never with `--batch`), and they return a non-zero exit code on failure (batch scripts if any package failed).

CLI reference: `pkg_extractor <pkg> [output-dir] [--no-pause]` extracts a package; `pkg_extractor <pkg> --print-type` prints `game`, `update` or `dlc`; `--check-type` is the legacy variant that exits with 101/102/103. Exit codes: 0 success, 1 failure, 2 invalid usage. Errors go to stderr; progress goes to stdout (one line per 10% when not a terminal).

## Build

### Requirements

- CMake 3.20 or newer (the first release that supports `CMAKE_CXX_STANDARD 23`)
- A C++23 compiler
- GNU Make (used to build the bundled Crypto++)
- Qt 6 Widgets
- zlib 1.3 or newer

Install the Qt and zlib development packages for your platform. For example, on Ubuntu:

```sh
sudo apt install build-essential cmake qt6-base-dev zlib1g-dev
```

Configure and build from the repository root:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

The executable is produced in the selected build directory. If CMake cannot locate Qt or zlib, set `CMAKE_PREFIX_PATH` to the relevant installation prefix. The project also has GitHub Actions builds for Linux, macOS, and Windows.

## Adding a translation

Translations are embedded from [`src/common/translations/translations.json`](src/common/translations/translations.json). Add a language entry using a locale code as its key, provide a display name in `_languageName`, and translate the UI strings using the English (`en`) entry as the key list. Missing or empty translations fall back to English, then to the source text. Rebuild the application to include the changes.

## License

This project is licensed under [GPL-2.0-or-later](LICENSE), consistent with the shadPS4 code it incorporates. The bundled Crypto++ dependency has its own license; see [`externals/cryptopp/License.txt`](externals/cryptopp/License.txt).
