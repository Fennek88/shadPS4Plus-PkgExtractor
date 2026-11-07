cmake_minimum_required(VERSION 3.20)
project(PkgExtractor VERSION 0.9.2)

if(WIN32)
	enable_language(RC)
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(PKG_EXTRACTOR_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer (GCC/Clang)" OFF)

function(pkg_extractor_configure_target target)
	if(MSVC)
		target_compile_options(${target} PRIVATE /W4)
	else()
		target_compile_options(${target} PRIVATE -Wall -Wextra)
	endif()
	if(WIN32)
		target_compile_definitions(${target} PRIVATE
			NTDDI_VERSION=0x0A000006
			_WIN32_WINNT=0x0A00
			WINVER=0x0A00
		)
	endif()
	if(PKG_EXTRACTOR_SANITIZE AND NOT MSVC)
		target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
		target_link_options(${target} PRIVATE -fsanitize=address,undefined)
	endif()
endfunction()

if(APPLE)
	# Use @rpath based install names and point them at the Frameworks
	# directory inside the .app bundle so macdeployqt can resolve and
	# relocate the Qt frameworks correctly.
	set(CMAKE_MACOSX_RPATH ON)
	set(CMAKE_INSTALL_RPATH "@executable_path/../Frameworks")
	set(CMAKE_BUILD_RPATH "@executable_path/../Frameworks")
endif()

find_package(ZLIB 1.3 REQUIRED)
set(PKG_EXTRACTOR_USE_LIBARCHIVE OFF)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR WIN32 OR APPLE)
	set(PKG_EXTRACTOR_USE_LIBARCHIVE ON)
	find_package(LibArchive REQUIRED)
endif()

include_directories(src)
include_directories(externals)

add_executable(
		pkg_extractor
		main.cpp
		src/app/package_extraction.cpp
		src/app/extractor_window.cpp
		src/app/release_info.cpp
		src/app/linux_update.cpp
		src/app/update_settings.cpp
		src/app/extraction_thread.h
		src/common/translations/translations.cpp
		src/common/translations/translations.qrc
		src/core/loader.cpp
		src/core/file_format/pkg.cpp
		src/core/file_format/psf.cpp
		src/core/file_format/pkg_type.cpp
		src/core/crypto/crypto.cpp
		src/common/io_file.cpp
		src/common/assert.cpp
		src/common/error.cpp
		src/common/path_util.cpp
		src/common/string_util.cpp
)

if(WIN32)
	configure_file("${CMAKE_CURRENT_SOURCE_DIR}/icon.rc.in" "${CMAKE_CURRENT_BINARY_DIR}/icon.rc" @ONLY)
	target_sources(pkg_extractor PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/icon.rc" src/app/windows_update.cpp)
endif()

if(APPLE)
	target_sources(pkg_extractor PRIVATE src/app/mac_update.cpp src/app/mac_update_zip.cpp)
	set(APP_ICONSET_DIR "${CMAKE_CURRENT_BINARY_DIR}/pkg_extractor.iconset")
	set(APP_ICON "${CMAKE_CURRENT_BINARY_DIR}/pkg_extractor.icns")
	add_custom_command(
		OUTPUT "${APP_ICON}"
		COMMAND "${CMAKE_COMMAND}" -E make_directory "${APP_ICONSET_DIR}"
		COMMAND sips -z 16 16 "${CMAKE_CURRENT_SOURCE_DIR}/icon.png" --out "${APP_ICONSET_DIR}/icon_16x16.png"
		COMMAND sips -z 32 32 "${CMAKE_CURRENT_SOURCE_DIR}/icon.png" --out "${APP_ICONSET_DIR}/icon_16x16@2x.png"
		COMMAND sips -z 32 32 "${CMAKE_CURRENT_SOURCE_DIR}/icon.png" --out "${APP_ICONSET_DIR}/icon_32x32.png"
		COMMAND sips -z 64 64 "${CMAKE_CURRENT_SOURCE_DIR}/icon.png" --out "${APP_ICONSET_DIR}/icon_32x32@2x.png"
		COMMAND sips -z 128 128 "${CMAKE_CURRENT_SOURCE_DIR}/icon.png" --out "${APP_ICONSET_DIR}/icon_128x128.png"
		COMMAND sips -z 256 256 "${CMAKE_CURRENT_SOURCE_DIR}/icon.png" --out "${APP_ICONSET_DIR}/icon_128x128@2x.png"
		COMMAND sips -z 256 256 "${CMAKE_CURRENT_SOURCE_DIR}/icon.png" --out "${APP_ICONSET_DIR}/icon_256x256.png"
		COMMAND sips -z 512 512 "${CMAKE_CURRENT_SOURCE_DIR}/icon.png" --out "${APP_ICONSET_DIR}/icon_256x256@2x.png"
		COMMAND sips -z 512 512 "${CMAKE_CURRENT_SOURCE_DIR}/icon.png" --out "${APP_ICONSET_DIR}/icon_512x512.png"
		COMMAND iconutil -c icns -o "${APP_ICON}" "${APP_ICONSET_DIR}"
		DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/icon.png"
		VERBATIM
	)
	set_source_files_properties("${APP_ICON}" PROPERTIES
		GENERATED TRUE
		MACOSX_PACKAGE_LOCATION Resources
	)
	target_sources(pkg_extractor PRIVATE "${APP_ICON}")
	set_target_properties(pkg_extractor PROPERTIES MACOSX_BUNDLE_ICON_FILE pkg_extractor.icns)
endif()

find_program(CRYPTO_MAKE_EXECUTABLE NAMES mingw32-make gmake make)
if(NOT CRYPTO_MAKE_EXECUTABLE)
	message(FATAL_ERROR "GNU Make is required to build the bundled Crypto++ dependency.")
endif()

set(CRYPTOPP_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/externals/cryptopp")
set(CRYPTOPP_LIBRARY "${CMAKE_CURRENT_BINARY_DIR}/cryptopp/libcryptopp.a")
add_custom_command(
	OUTPUT "${CRYPTOPP_LIBRARY}"
	COMMAND "${CRYPTO_MAKE_EXECUTABLE}" -j2 -C "${CRYPTOPP_SOURCE_DIR}" libcryptopp.a
	COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/cryptopp"
	COMMAND "${CMAKE_COMMAND}" -E copy_if_different
		"${CRYPTOPP_SOURCE_DIR}/libcryptopp.a" "${CRYPTOPP_LIBRARY}"
	DEPENDS "${CRYPTOPP_SOURCE_DIR}/GNUmakefile"
	VERBATIM
)
add_custom_target(cryptopp_build DEPENDS "${CRYPTOPP_LIBRARY}")
add_library(libcryptopp STATIC IMPORTED GLOBAL)
set_property(TARGET libcryptopp PROPERTY IMPORTED_LOCATION "${CRYPTOPP_LIBRARY}")
add_dependencies(pkg_extractor cryptopp_build)

target_link_libraries(pkg_extractor PRIVATE libcryptopp)
target_link_libraries(pkg_extractor PRIVATE ZLIB::ZLIB)

find_package(Qt6 6.0 QUIET COMPONENTS Widgets Network)
if(NOT Qt6_FOUND)
	message(WARNING
		"Qt 6 Widgets and Network were not found. Install Qt 6 from https://www.qt.io/download-qt-installer "
		"and set CMAKE_PREFIX_PATH to your Qt installation if needed.")
	message(FATAL_ERROR "Qt 6 Widgets and Network are required to build the PKG extractor GUI.")
endif()

set_target_properties(pkg_extractor PROPERTIES AUTOMOC ON AUTORCC ON)
if(APPLE)
	set_target_properties(pkg_extractor PROPERTIES MACOSX_BUNDLE ON)
endif()
target_compile_definitions(pkg_extractor PRIVATE PKG_EXTRACTOR_WITH_QT PKG_EXTRACTOR_VERSION="${PROJECT_VERSION}")
target_link_libraries(pkg_extractor PRIVATE Qt6::Widgets Qt6::Network)
if(PKG_EXTRACTOR_USE_LIBARCHIVE AND NOT APPLE)
	target_sources(pkg_extractor PRIVATE src/app/windows_update_archive.cpp)
endif()
if(PKG_EXTRACTOR_USE_LIBARCHIVE)
	target_link_libraries(pkg_extractor PRIVATE LibArchive::LibArchive)
endif()
pkg_extractor_configure_target(pkg_extractor)

if(WIN32)
	# Windows chooses console behavior from the executable's subsystem, so keep
	# a console build for scripts and use the GUI subsystem for the main app.
	get_target_property(PKG_EXTRACTOR_SOURCES pkg_extractor SOURCES)
	add_executable(pkg_extractor_cli ${PKG_EXTRACTOR_SOURCES})
	add_dependencies(pkg_extractor_cli cryptopp_build)
	set_target_properties(pkg_extractor PROPERTIES WIN32_EXECUTABLE ON)
	set_target_properties(pkg_extractor_cli PROPERTIES AUTOMOC ON AUTORCC ON)
	target_compile_definitions(pkg_extractor_cli PRIVATE PKG_EXTRACTOR_WITH_QT PKG_EXTRACTOR_VERSION="${PROJECT_VERSION}")
	target_link_libraries(pkg_extractor_cli PRIVATE libcryptopp ZLIB::ZLIB Qt6::Widgets Qt6::Network LibArchive::LibArchive)
	if(TARGET Qt6::EntryPoint)
		target_link_libraries(pkg_extractor PRIVATE Qt6::EntryPoint)
	endif()
	pkg_extractor_configure_target(pkg_extractor_cli)
endif()

option(PKG_EXTRACTOR_BUILD_TESTS "Build the automated tests" ON)
if(PKG_EXTRACTOR_BUILD_TESTS)
	enable_testing()
	add_executable(
		pkg_extractor_tests
		tests/test_main.cpp
		src/app/package_extraction.cpp
		src/app/release_info.cpp
		src/app/linux_update.cpp
		src/app/update_settings.cpp
		src/core/loader.cpp
		src/core/file_format/pkg.cpp
		src/core/file_format/psf.cpp
		src/core/file_format/pkg_type.cpp
		src/core/crypto/crypto.cpp
		src/common/io_file.cpp
		src/common/assert.cpp
		src/common/error.cpp
		src/common/path_util.cpp
		src/common/string_util.cpp
	)
	add_dependencies(pkg_extractor_tests cryptopp_build)
	target_link_libraries(pkg_extractor_tests PRIVATE libcryptopp ZLIB::ZLIB Qt6::Widgets)
	if(PKG_EXTRACTOR_USE_LIBARCHIVE AND NOT APPLE)
		target_sources(pkg_extractor_tests PRIVATE src/app/windows_update_archive.cpp)
	endif()
	if(PKG_EXTRACTOR_USE_LIBARCHIVE)
		target_link_libraries(pkg_extractor_tests PRIVATE LibArchive::LibArchive)
	endif()
	if(APPLE)
		target_sources(pkg_extractor_tests PRIVATE src/app/mac_update.cpp src/app/mac_update_zip.cpp)
	endif()
	set_target_properties(pkg_extractor_tests PROPERTIES AUTOMOC ON)
	pkg_extractor_configure_target(pkg_extractor_tests)
	add_test(NAME pkg_extractor_tests COMMAND pkg_extractor_tests)
	if(WIN32)
		add_test(NAME pkg_extractor_cli_help COMMAND pkg_extractor_cli --help)
	endif()
	if(UNIX)
		add_test(NAME pkg_extractor_scripts
			COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_scripts.sh $<TARGET_FILE:pkg_extractor> ${CMAKE_CURRENT_SOURCE_DIR} "$ENV{PKG_TEST_PKG}")
	endif()
endif()
