cmake_minimum_required(VERSION 3.21)

project(ClipDeckWindows VERSION 0.9.5 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets Network Sql Qml Quick Test)
qt_standard_project_setup(REQUIRES 6.5)

# The data layer is shared with the Linux client; it is compiled from there so
# both platforms keep a single implementation of the schema and classifier.
set(LINUX_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../linux/src")

add_library(clipdeck-core STATIC
  ${LINUX_SRC}/history_repository.cpp
  ${LINUX_SRC}/history/file_reference.cpp
  ${LINUX_SRC}/history/capture_payload.cpp
  ${LINUX_SRC}/history/file_paste_controller.cpp
  ${LINUX_SRC}/history/clipboard_pipeline.cpp
  ${LINUX_SRC}/history/hybrid_logical_clock.cpp
  ${LINUX_SRC}/sync/noise.cpp
  ${LINUX_SRC}/sync/cpace.cpp
  ${LINUX_SRC}/sync/sync_protocol.cpp
  ${LINUX_SRC}/sync/key_store.cpp
  ${LINUX_SRC}/sync/network_interfaces.cpp
  ${LINUX_SRC}/sync/secure_channel.cpp
  ${LINUX_SRC}/sync/discovery.cpp
  ${LINUX_SRC}/sync/sync_service.cpp
  ${LINUX_SRC}/ui/display_scale.cpp
)
target_include_directories(clipdeck-core PUBLIC ${LINUX_SRC})
target_compile_definitions(clipdeck-core PUBLIC QT_NO_KEYWORDS UNICODE _UNICODE NOMINMAX CLIPDECK_NO_X11)
target_link_libraries(clipdeck-core PUBLIC Qt6::Core Qt6::Gui Qt6::Sql Qt6::Qml Qt6::Network crypt32)

# libsodium (>= 1.0.18 for ristretto255), linked statically: LAN sync crypto
# (docs/lan-sync-design.md §9). Point SODIUM_ROOT at the official MinGW build,
# e.g. libsodium-1.0.20-mingw/libsodium-win64.
set(SODIUM_ROOT "" CACHE PATH "libsodium install prefix (include/ and lib/libsodium.a)")
find_path(SODIUM_INCLUDE_DIR sodium.h HINTS "${SODIUM_ROOT}/include" REQUIRED)
find_library(SODIUM_LIBRARY NAMES libsodium.a sodium HINTS "${SODIUM_ROOT}/lib" REQUIRED)
target_include_directories(clipdeck-core PUBLIC ${SODIUM_INCLUDE_DIR})
target_compile_definitions(clipdeck-core PUBLIC SODIUM_STATIC=1)
target_link_libraries(clipdeck-core PUBLIC ${SODIUM_LIBRARY})

# Win32 platform layer: clipboard monitor/reader/writer, source application,
# global hot key.
add_library(clipdeck-platform STATIC
  src/platform/win_clipboard_format.cpp
  src/platform/win_clipboard_reader.cpp
  src/platform/win_clipboard_writer.cpp
  src/platform/win_clipboard_monitor.cpp
  src/platform/win_source_application.cpp
  src/platform/win_capture_service.cpp
  src/platform/win_global_hotkey.cpp
  src/platform/win_process_util.cpp
  src/platform/win_paste_service.cpp
  src/platform/win_tray.cpp
  src/platform/win_window_effects.cpp
  src/platform/win_launch_at_login.cpp
)
target_include_directories(clipdeck-platform PUBLIC src/platform)
target_compile_definitions(clipdeck-platform PUBLIC _WIN32_WINNT=0x0A00 WINVER=0x0A00 WIN32_LEAN_AND_MEAN)
target_link_libraries(clipdeck-platform PUBLIC clipdeck-core Qt6::Widgets user32 shell32 version ole32 gdi32 dwmapi advapi32)

# Deck state machine: port of mac InMemoryHistoryStore.
add_library(clipdeck-store STATIC
  src/store/history_store.cpp
)
target_include_directories(clipdeck-store PUBLIC src/store)
target_link_libraries(clipdeck-store PUBLIC clipdeck-core)

# The ClipDeck application: Deck window (QML), tray, hot key, capture, paste.
add_library(clipdeck-app STATIC
  src/app/deck_presentation.cpp
  src/app/deck_backdrop.cpp
  src/app/code_highlight.cpp
  src/app/deck_image_provider.cpp
  src/app/deck_item_model.cpp
  src/app/deck_controller.cpp
  src/app/app_preferences.cpp
  src/app/javascript_formatter.cpp
  src/app/settings_controller.cpp
  src/app/link_metadata_fetcher.cpp
)
target_include_directories(clipdeck-app PUBLIC src/app)
target_link_libraries(clipdeck-app PUBLIC clipdeck-store clipdeck-platform Qt6::Quick Qt6::Network)
# highlight.js is shared with the mac client (CodeHighlightService).
set(MAC_RESOURCES "${CMAKE_CURRENT_SOURCE_DIR}/../mac/Sources/ClipDeck/Resources")
qt_add_resources(clipdeck-app "highlight_js"
  PREFIX "/highlight"
  BASE "${MAC_RESOURCES}"
  FILES "${MAC_RESOURCES}/highlight.min.js"
)

qt_add_executable(clipdeck WIN32 src/app/main.cpp resources/clipdeck.rc)
qt_add_qml_module(clipdeck
  URI ClipDeck
  VERSION 1.0
  QML_FILES
    qml/Main.qml
    qml/Card.qml
    qml/DeckIcon.qml
    qml/LinkIcon.qml
    qml/SourceBadge.qml
    qml/PopoverWindow.qml
    qml/ContextMenuWindow.qml
    qml/RotatedImage.qml
    qml/Checkerboard.qml
    qml/FileRows.qml
    qml/PillButton.qml
    qml/IconButton.qml
    qml/SettingsWindow.qml
    qml/SettingsPage.qml
    qml/SettingsSection.qml
    qml/SettingsRow.qml
    qml/ToggleSwitch.qml
    qml/DropDown.qml
    qml/RetentionSlider.qml
    qml/ShortcutRecorder.qml
    qml/ConfirmDialog.qml
    qml/TextButton.qml
    qml/CategorySettings.qml
    qml/DropShadow.qml
    qml/ScriptEditor.qml
    qml/ExpandedScriptEditor.qml
    qml/OverlayScrollBar.qml
    qml/ColorPicker.qml
    qml/SegmentedControl.qml
    qml/SyncPanel.qml
    qml/SyncLoader.qml
    qml/CardImage.qml
)
# Windows icons stand on a transparent background (the mac AppIcon is a tile).
set(APP_ICONS "${CMAKE_CURRENT_SOURCE_DIR}/../shared/assets/windows")
qt_add_resources(clipdeck "app_icons"
  PREFIX "/icons"
  BASE "${APP_ICONS}"
  FILES "${APP_ICONS}/icon-16.png" "${APP_ICONS}/icon-20.png" "${APP_ICONS}/icon-24.png"
        "${APP_ICONS}/icon-32.png" "${APP_ICONS}/icon-40.png" "${APP_ICONS}/icon-48.png"
        "${APP_ICONS}/icon-64.png" "${APP_ICONS}/icon-256.png"
)
target_link_libraries(clipdeck PRIVATE clipdeck-app)

add_executable(core-tests
  tests/core_tests.cpp
  ${LINUX_SRC}/history/file_reference_tests.cpp
  # file_reference_tests also exercises the Widgets file card shared with Linux.
  ${LINUX_SRC}/ui/file_card_view.cpp
  ${LINUX_SRC}/ui/eliding_label.cpp
  ${LINUX_SRC}/ui/deck_symbols.cpp
)
target_link_libraries(core-tests PRIVATE clipdeck-core Qt6::Widgets)

add_executable(store-tests tests/store_tests.cpp)
target_link_libraries(store-tests PRIVATE clipdeck-store)

add_executable(deck-tests tests/deck_tests.cpp)
target_link_libraries(deck-tests PRIVATE clipdeck-app)

add_executable(sync-tests tests/sync_tests.cpp)
target_link_libraries(sync-tests PRIVATE clipdeck-core)

# Golden values for the macOS implementation (mac/Tests/.../SyncGoldenTests.swift).
add_executable(sync-golden tests/sync_golden.cpp)
target_link_libraries(sync-golden PRIVATE clipdeck-core)

add_executable(platform-tests tests/platform_tests.cpp)
target_link_libraries(platform-tests PRIVATE clipdeck-platform Qt6::Widgets)
