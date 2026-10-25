# The Homebrew App Store, an Xbox 360 title on xsdk (https://github.com/xenosworks/xsdk), and store_check, its checks
# on the console. Build xsdk first (make in its checkout), check out imgui-xenon, curl-xenon and mbedtls-xenon beside
# this repository or name them below, then:
#   cmake -S . -B build -GNinja -DCMAKE_TOOLCHAIN_FILE=<xsdk>/cmake/xsdk.cmake -DCMAKE_BUILD_TYPE=Release
#   cmake --build build
cmake_minimum_required(VERSION 3.25)
project(homebrew_app_store C CXX ASM)

set(IMGUI_XENON_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../imgui-xenon" CACHE PATH "A checkout of imgui-xenon")
set(CURL_XENON_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../curl-xenon" CACHE PATH "A checkout of curl-xenon")
set(MBEDTLS_XENON_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../mbedtls-xenon" CACHE PATH "A checkout of mbedtls-xenon")
# The store takes only catalogues this key signed: xenosworks's, whose private half is kept off line. The checks build
# with tests/make_site.py's test key in its place.
set(STORE_PUBLIC_KEY "${CMAKE_CURRENT_SOURCE_DIR}/keys/catalogue.pub.pem"
	CACHE FILEPATH "The catalogue's public key, from xsdk's tools/store_catalogue.py keygen")
set(STORE_CATALOGUE_URL "https://xenosworks.github.io/homebrew-app-catalogue/catalogue.json"
	CACHE STRING "Where the store fetches the catalogue until a catalogue it kept names another address")
if(NOT EXISTS "${STORE_PUBLIC_KEY}")
	message(FATAL_ERROR "Set STORE_PUBLIC_KEY to the catalogue's public key (.pub.pem)")
endif()
message(STATUS "The store takes catalogues signed by ${STORE_PUBLIC_KEY}")
file(READ "${STORE_PUBLIC_KEY}" STORE_PUBLIC_KEY_PEM)
string(REPLACE "\r" "" STORE_PUBLIC_KEY_PEM "${STORE_PUBLIC_KEY_PEM}")
string(REPLACE "\n" "\\n" STORE_PUBLIC_KEY_PEM "${STORE_PUBLIC_KEY_PEM}")
configure_file(src/key.h.in key.h @ONLY)

# Mbed TLS and curl, as curl-xenon's tests/xsdk/CMakeLists.txt adds them: curl takes mbedtls-xenon's own targets.
set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
add_subdirectory("${MBEDTLS_XENON_DIR}" mbedtls)
set(MBEDTLS_INCLUDE_DIR "${MBEDTLS_XENON_DIR}/include" CACHE PATH "")
set(MBEDTLS_LIBRARY mbedtls CACHE STRING "")
set(MBEDX509_LIBRARY mbedx509 CACHE STRING "")
set(MBEDCRYPTO_LIBRARY mbedcrypto CACHE STRING "")
set(CURL_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
add_subdirectory("${CURL_XENON_DIR}" curl)

# What the store and store_check share: the catalogue, screenshots, fetching and installing, with cJSON 1.7.19,
# stb_image 2.30 and miniz 3.1.2 as upstream ships them, stb_image for PNG and JPEG alone.
# tests/breaks.py points STORE_CORE_DIR at a copy with one thing broken on purpose.
set(STORE_CORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src" CACHE PATH "Where the shared C files come from")
add_library(store_core STATIC "${STORE_CORE_DIR}/catalogue.c" "${STORE_CORE_DIR}/net.c" "${STORE_CORE_DIR}/shots.c"
	"${STORE_CORE_DIR}/install.c" "${STORE_CORE_DIR}/launch.c" third_party/cjson/cJSON.c third_party/stb/stb_image.c
	third_party/miniz/miniz.c)
target_include_directories(store_core PUBLIC src third_party/cjson third_party/stb third_party/miniz
	"${CMAKE_CURRENT_BINARY_DIR}")
target_compile_definitions(store_core PUBLIC STBI_ONLY_PNG STBI_ONLY_JPEG STBI_NO_STDIO STBI_NO_LINEAR
	STBI_MAX_DIMENSIONS=4096)
# miniz 3.1.2 for packages: reading zips from memory alone, on a big-endian CPU, its byte order set rather than guessed.
target_compile_definitions(store_core PUBLIC MINIZ_NO_STDIO MINIZ_NO_TIME MINIZ_NO_ARCHIVE_WRITING_APIS
	MINIZ_NO_ZLIB_COMPATIBLE_NAMES MINIZ_LITTLE_ENDIAN=0 MINIZ_USE_UNALIGNED_LOADS_AND_STORES=0
	MINIZ_HAS_64BIT_REGISTERS=0)
target_link_libraries(store_core PUBLIC CURL::libcurl mbedtls)
set(warnings -Wall -Wextra -Werror)
set_source_files_properties("${STORE_CORE_DIR}/catalogue.c" "${STORE_CORE_DIR}/net.c" "${STORE_CORE_DIR}/shots.c"
	"${STORE_CORE_DIR}/install.c" "${STORE_CORE_DIR}/launch.c" src/main.c src/worker.c src/ui.cpp tests/store_check.c
	PROPERTIES COMPILE_OPTIONS "${warnings}")

# Dear ImGui, from imgui-xenon (its docs/README-xsdk.md).
add_library(imgui STATIC "${IMGUI_XENON_DIR}/imgui.cpp" "${IMGUI_XENON_DIR}/imgui_draw.cpp"
	"${IMGUI_XENON_DIR}/imgui_tables.cpp" "${IMGUI_XENON_DIR}/imgui_widgets.cpp"
	"${IMGUI_XENON_DIR}/backends/imgui_impl_xsdk.cpp")
target_include_directories(imgui PUBLIC "${IMGUI_XENON_DIR}" "${IMGUI_XENON_DIR}/backends")
# ImGui's default shell function calls fork() and execvp(), which the console hasn't got.
target_compile_definitions(imgui PUBLIC IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS)

add_executable(homebrew_app_store src/main.c src/worker.c src/ui.cpp src/fonts.S)
target_link_libraries(homebrew_app_store PRIVATE store_core imgui)
target_compile_features(homebrew_app_store PRIVATE cxx_std_23)
# The assembler looks for .incbin's files beside fonts.S, and fonts.S is built again when a font changes.
set(fonts Archivo-Regular.ttf Archivo-Medium.ttf Archivo-Bold.ttf)
list(TRANSFORM fonts PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/src/fonts/")
set_source_files_properties(src/fonts.S PROPERTIES COMPILE_OPTIONS "-Wa,-I${CMAKE_CURRENT_SOURCE_DIR}/src"
	OBJECT_DEPENDS "${fonts}")
xsdk_add_xex(homebrew_app_store TITLE_ID 0x58534153 NAME "Homebrew App Store" VERSION 0.1.0.0)
# The same, newer, for tests/make_site.py's selfstore/, which the store updates itself to.
add_executable(homebrew_app_store_v2 src/main.c src/worker.c src/ui.cpp src/fonts.S)
target_link_libraries(homebrew_app_store_v2 PRIVATE store_core imgui)
target_compile_features(homebrew_app_store_v2 PRIVATE cxx_std_23)
xsdk_add_xex(homebrew_app_store_v2 TITLE_ID 0x58534153 NAME "Homebrew App Store" VERSION 0.2.0.0)

add_executable(store_check tests/store_check.c)
target_link_libraries(store_check PRIVATE store_core)
xsdk_add_xex(store_check TITLE_ID 0x58534143 NAME "store_check" VERSION 0.0.0.1)
# The same, newer, which tests/make_site.py puts in the catalogue for store_check to update itself to.
add_executable(store_check_v2 tests/store_check.c)
target_link_libraries(store_check_v2 PRIVATE store_core)
xsdk_add_xex(store_check_v2 TITLE_ID 0x58534143 NAME "store_check" VERSION 0.0.0.2)

# Beside the titles: Mozilla's roots, as mbedtls-xenon's checks have them, and Mbed TLS's test CA, which
# xsdk's tools/net_http.py's certificate for localhost is under.
configure_file("${MBEDTLS_XENON_DIR}/programs/xsdk/cacert.pem" cacert.pem COPYONLY)
configure_file("${MBEDTLS_XENON_DIR}/framework/data_files/test-ca2.crt" test-ca2.crt COPYONLY)
