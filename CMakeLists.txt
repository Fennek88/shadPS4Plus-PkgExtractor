cmake_minimum_required(VERSION 3.16)
project(PkgExtractor)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(APPLE)
	# Use @rpath based install names and point them at the Frameworks
	# directory inside the .app bundle so macdeployqt can resolve and
	# relocate the Qt frameworks correctly.
	set(CMAKE_MACOSX_RPATH ON)
	set(CMAKE_INSTALL_RPATH "@executable_path/../Frameworks")
	set(CMAKE_BUILD_RPATH "@executable_path/../Frameworks")
endif()

find_package(ZLIB 1.3 REQUIRED)

include_directories(src)
include_directories(externals)

add_executable(
		pkg_extractor
		main.cpp
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

find_package(Qt6 6.0 QUIET COMPONENTS Widgets)
if(NOT Qt6_FOUND)
	message(WARNING
		"Qt 6 Widgets was not found. Install Qt 6 from https://www.qt.io/download-qt-installer "
		"and set CMAKE_PREFIX_PATH to your Qt installation if needed.")
	message(FATAL_ERROR "Qt 6 Widgets is required to build the PKG extractor GUI.")
endif()

set_target_properties(pkg_extractor PROPERTIES AUTOMOC ON AUTORCC ON)
if(APPLE)
	set_target_properties(pkg_extractor PROPERTIES MACOSX_BUNDLE ON)
endif()
target_compile_definitions(pkg_extractor PRIVATE PKG_EXTRACTOR_WITH_QT)
target_link_libraries(pkg_extractor PRIVATE Qt6::Widgets)

if(WIN32)
	target_compile_definitions(pkg_extractor PRIVATE
		NTDDI_VERSION=0x0A000006
		_WIN32_WINNT=0x0A00
		WINVER=0x0A00
	)
endif()