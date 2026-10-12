cmake_minimum_required(VERSION 3.21)
project(moza_fs_bridge VERSION 0.1.3 LANGUAGES CXX)

# Which pre-release of that version this is: 1 builds 0.1.3-pre1, published as tag v0.1.3-pre1.
# For the finished version leave the number out: set(BRIDGE_PRERELEASE ). scripts/package-release.sh reads this line.
set(BRIDGE_PRERELEASE 3)
if(BRIDGE_PRERELEASE)
    set(BRIDGE_VERSION_TEXT "${PROJECT_VERSION}-pre${BRIDGE_PRERELEASE}")
else()
    set(BRIDGE_VERSION_TEXT "${PROJECT_VERSION}")
endif()

# The SDK's typed device headers (moza/device_as.hpp) use concepts.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(MOZA_SDK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../sdk/x64-release" CACHE PATH
    "MOZA SDK variant directory (the one containing include/, lib/ and bin/)")

# The real SDK is a Windows DLL that needs the MOZA device service and a connected base. The fake
# implements the same C API in-process so the bridge logic builds and tests anywhere.
if(WIN32)
    set(_fake_sdk_default OFF)
else()
    set(_fake_sdk_default ON)
endif()
option(BRIDGE_FAKE_SDK "Link the in-process fake MOZA SDK instead of MOZA_SDK.dll" ${_fake_sdk_default})

add_library(bridge_core STATIC src/ffb_session.cpp src/force_model.cpp src/link.cpp src/mod_sync.cpp src/telemetry.cpp src/update_check.cpp
            src/yoke_lights.cpp)
target_include_directories(bridge_core PUBLIC src)
# The settings registry, live status and the local web page that edits them. No SDK involved:
# a thread for the HTTP server and the system's sockets are all they need.
target_sources(bridge_core PRIVATE src/settings.cpp src/status_board.cpp src/web_ui.cpp src/web_ui_page.cpp)
find_package(Threads REQUIRED)
target_link_libraries(bridge_core PUBLIC Threads::Threads)
if(WIN32)
    target_link_libraries(bridge_core PUBLIC ws2_32 shell32 ole32)
endif()
if(MSVC)
    target_compile_options(bridge_core PUBLIC /W4 /utf-8)
else()
    target_compile_options(bridge_core PUBLIC -Wall -Wextra)
    # device_base.hpp has a requires-clause only MSVC accepts unparenthesized. Compile against a
    # patched copy placed ahead of the SDK on the include path; the vendored header stays untouched.
    set(_sdk_compat_dir "${CMAKE_CURRENT_BINARY_DIR}/sdk_compat")
    file(READ "${MOZA_SDK_ROOT}/include/moza/private/device_base.hpp" _device_base)
    string(REPLACE "requires std::is_integral_v<T> && !std::is_same_v<T, bool>"
                   "requires std::is_integral_v<T> && (!std::is_same_v<T, bool>)" _device_base "${_device_base}")
    file(WRITE "${_sdk_compat_dir}/moza/private/device_base.hpp" "${_device_base}")
    target_include_directories(bridge_core BEFORE PUBLIC "${_sdk_compat_dir}")
endif()

if(BRIDGE_FAKE_SDK)
    add_library(moza_sdk_fake STATIC tests/fake_moza_sdk.cpp)
    target_include_directories(moza_sdk_fake PUBLIC "${MOZA_SDK_ROOT}/include" tests)
    # moza.h declares the API __declspec(dllimport) unless this is defined; the fake is linked statically.
    target_compile_definitions(moza_sdk_fake PUBLIC MOZA_SDK_EXPORT_DLL)
    if(NOT MSVC)
        target_compile_options(moza_sdk_fake PUBLIC -fdeclspec -Wno-ignored-attributes -Wno-unknown-attributes)
    endif()
    target_link_libraries(bridge_core PUBLIC moza_sdk_fake)
else()
    find_package(MOZA_SDK CONFIG REQUIRED PATHS "${MOZA_SDK_ROOT}/lib/cmake/MOZA_SDK" NO_DEFAULT_PATH)
    # Only the release variant is vendored; its C API is safe to call from any build configuration.
    set_target_properties(MOZA_SDK::MOZA_SDK PROPERTIES
        MAP_IMPORTED_CONFIG_DEBUG Release
        MAP_IMPORTED_CONFIG_RELWITHDEBINFO Release
        MAP_IMPORTED_CONFIG_MINSIZEREL Release)
    target_link_libraries(bridge_core PUBLIC MOZA_SDK::MOZA_SDK)
endif()

add_executable(moza_fs_bridge src/main.cpp)
target_compile_definitions(moza_fs_bridge PRIVATE BRIDGE_VERSION="${BRIDGE_VERSION_TEXT}")
target_link_libraries(moza_fs_bridge PRIVATE bridge_core)
if(MINGW)
    # Fold the GCC runtime into the exe so MOZA_SDK.dll is the only file that has to ship with it.
    # -s: the static GCC runtime otherwise brings 15 MB of its own debug symbols along.
    target_link_options(moza_fs_bridge PRIVATE -static $<$<CONFIG:Release>:-s>)
endif()

# The everyday program on Windows: the same code built without a console, living behind an icon by
# the clock. moza_fs_bridge stays a console program for the test options.
if(WIN32)
    enable_language(RC)
    target_sources(moza_fs_bridge PRIVATE src/updater_win.cpp windows/app.rc)
    target_link_libraries(moza_fs_bridge PRIVATE winhttp bcrypt shell32)
    add_executable(moza_fs_link WIN32 src/main.cpp src/settings_window_win.cpp src/tray_win.cpp src/updater_win.cpp windows/app.rc)
    target_compile_definitions(moza_fs_link PRIVATE BRIDGE_TRAY BRIDGE_VERSION="${BRIDGE_VERSION_TEXT}")
    target_link_libraries(moza_fs_link PRIVATE bridge_core shell32 winhttp bcrypt comctl32 comdlg32 gdi32)
    if(MINGW)
        target_link_options(moza_fs_link PRIVATE -static $<$<CONFIG:Release>:-s>)
    elseif(MSVC)
        # WIN32 makes MSVC look for WinMain; this program has an ordinary main.
        target_link_options(moza_fs_link PRIVATE /ENTRY:mainCRTStartup)
    endif()
endif()

if(NOT BRIDGE_FAKE_SDK)
    add_custom_command(TARGET moza_fs_bridge POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:MOZA_SDK::MOZA_SDK> $<TARGET_FILE_DIR:moza_fs_bridge>
        COMMENT "Copying MOZA_SDK.dll next to moza_fs_bridge")
endif()

if(BRIDGE_FAKE_SDK)
    enable_testing()
    add_executable(test_ffb_session tests/test_ffb_session.cpp)
    target_link_libraries(test_ffb_session PRIVATE bridge_core)
    add_test(NAME ffb_session COMMAND test_ffb_session)
    # Telemetry parsing and the force model need no SDK; one reader test rewrites the file from a second thread.
    find_package(Threads REQUIRED)
    add_executable(test_force_model tests/test_force_model.cpp)
    target_link_libraries(test_force_model PRIVATE bridge_core Threads::Threads)
    add_test(NAME force_model COMMAND test_force_model)
    add_test(NAME bridge_hold COMMAND moza_fs_bridge --hold --seconds 1.5 --refresh-ms 100)
    # Link mode with no game: no telemetry file, so it idles; the settings page is served on a test port.
    add_test(NAME bridge_link COMMAND moza_fs_bridge --seconds 1.5 --ui-port 18778
             --telemetry "${CMAKE_CURRENT_BINARY_DIR}/no-telemetry.txt" --settings "${CMAKE_CURRENT_BINARY_DIR}/link-test.ini")
    add_test(NAME bridge_list COMMAND moza_fs_bridge --list)
    # A loop that freezes, as inside an SDK call that never returns: the guard ends the process with its own exit code.
    add_test(NAME bridge_stall COMMAND sh -c "\"$<TARGET_FILE:moza_fs_bridge>\" --no-ui --seconds 60 --test-stall-after 0.5 --telemetry \"${CMAKE_CURRENT_BINARY_DIR}/no-telemetry.txt\" --settings \"${CMAKE_CURRENT_BINARY_DIR}/stall-test.ini\"; test $? -eq 71")
    set_tests_properties(bridge_stall PROPERTIES TIMEOUT 30)
    add_test(NAME bridge_sweep COMMAND moza_fs_bridge --sweep pitch --sweep-period 1 --seconds 1.2)
    add_test(NAME bridge_force COMMAND moza_fs_bridge --force pitch --force-period 1 --seconds 1.2)
    # The fake's axis never moves, so this exercises the stall path: each end is given up on in turn.
    add_test(NAME bridge_stroke COMMAND moza_fs_bridge --stroke pitch --stroke-cycles 1)
    # Settings, status board and the web UI's HTTP server, over a real loopback socket.
    add_executable(test_web_ui tests/test_web_ui.cpp)
    target_link_libraries(test_web_ui PRIVATE bridge_core)
    add_test(NAME web_ui COMMAND test_web_ui)
    # Reading GitHub's release document, version comparison, and keeping the game's mod current.
    add_executable(test_update tests/test_update.cpp)
    target_link_libraries(test_update PRIVATE bridge_core)
    add_test(NAME update COMMAND test_update)
    # The guard that notices a loop stuck inside the SDK.
    add_executable(test_stall_guard tests/test_stall_guard.cpp)
    target_link_libraries(test_stall_guard PRIVATE bridge_core)
    add_test(NAME stall_guard COMMAND test_stall_guard)
    # Not a test: serves the tuning page with simulated values, for looking at it in a browser.
    add_executable(web_ui_demo tests/web_ui_demo.cpp)
    target_link_libraries(web_ui_demo PRIVATE bridge_core)
endif()
