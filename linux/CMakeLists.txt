cmake_minimum_required(VERSION 3.13)

project(ClipDeckLinux VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_INSTALL_RPATH "$ORIGIN/../lib")

find_package(Qt5 5.11 REQUIRED COMPONENTS Core Gui Widgets Network Sql Qml X11Extras WebEngine WebEngineWidgets)
find_package(X11 REQUIRED)
find_package(Threads REQUIRED)
find_library(XTST_LIBRARY Xtst)
find_library(XCB_LIBRARY xcb)

if(NOT XTST_LIBRARY OR NOT XCB_LIBRARY)
  message(FATAL_ERROR "libXtst and libxcb are required")
endif()

add_executable(clipdeck-linux
  src/main.cpp
  src/history_repository.cpp
  src/history_repository.h
  src/history/clipboard_pipeline.cpp
  src/history/clipboard_pipeline.h
  src/history/clipboard_snapshot.h
  src/history/hybrid_logical_clock.cpp
  src/history/hybrid_logical_clock.h
  src/sync/cpace.cpp
  src/sync/cpace.h
  src/sync/discovery.cpp
  src/sync/discovery.h
  src/sync/key_store.cpp
  src/sync/key_store.h
  src/sync/network_interfaces.cpp
  src/sync/network_interfaces.h
  src/sync/noise.cpp
  src/sync/noise.h
  src/sync/secure_channel.cpp
  src/sync/secure_channel.h
  src/sync/sync_protocol.cpp
  src/sync/sync_protocol.h
  src/sync/sync_service.cpp
  src/sync/sync_service.h
  src/ui/sync_panel.cpp
  src/ui/sync_panel.h
  src/history/file_reference.cpp
  src/history/file_reference.h
  src/history/capture_payload.cpp
  src/history/capture_payload.h
  src/history/file_paste_controller.cpp
  src/history/file_paste_controller.h
  src/history/file_reference_tests.cpp
  src/history/file_reference_tests.h
  src/history/clipboard_pipeline_tests.cpp
  src/history/clipboard_pipeline_tests.h
  src/platform/application_source.cpp
  src/platform/application_source.h
  src/platform/x11_clipboard_reader.cpp
  src/platform/x11_clipboard_reader.h
  src/platform/x11_global_shortcut.cpp
  src/platform/x11_global_shortcut.h
  src/platform/linux_application_icon.cpp
  src/platform/linux_application_icon.h
  src/ui/display_scale.cpp
  src/ui/display_scale.h
  src/ui/eliding_label.cpp
  src/ui/eliding_label.h
  src/ui/deck_symbols.cpp
  src/ui/deck_symbols.h
  src/ui/file_card_view.cpp
  src/ui/file_card_view.h
  src/ui/linux_icons.cpp
  src/ui/linux_icons.h
  src/ui/script_editor_widget.cpp
  src/ui/script_editor_widget.h
)

# LAN sync: libsodium >= 1.0.18 (ristretto255), linked statically. The Docker
# image builds it into /opt/libsodium (linux/docker/Dockerfile).
set(SODIUM_ROOT "/opt/libsodium" CACHE PATH "libsodium install prefix")
find_path(SODIUM_INCLUDE_DIR sodium.h HINTS "${SODIUM_ROOT}/include")
find_library(SODIUM_LIBRARY NAMES libsodium.a sodium HINTS "${SODIUM_ROOT}/lib")
if(NOT SODIUM_INCLUDE_DIR OR NOT SODIUM_LIBRARY)
  message(FATAL_ERROR "libsodium >= 1.0.18 is required (set SODIUM_ROOT)")
endif()

target_include_directories(clipdeck-linux PRIVATE ${X11_INCLUDE_DIR} ${SODIUM_INCLUDE_DIR} src)
target_compile_definitions(clipdeck-linux PRIVATE SODIUM_STATIC=1)
target_compile_definitions(clipdeck-linux PRIVATE QT_NO_KEYWORDS)
target_link_libraries(clipdeck-linux PRIVATE
  Qt5::Core
  Qt5::Gui
  Qt5::Widgets
  Qt5::Network
  Qt5::Sql
  Qt5::Qml
  Qt5::X11Extras
  Qt5::WebEngine
  Qt5::WebEngineWidgets
  ${X11_LIBRARIES}
  ${XTST_LIBRARY}
  ${XCB_LIBRARY}
  Threads::Threads
  ${SODIUM_LIBRARY}
)

install(TARGETS clipdeck-linux
  RUNTIME DESTINATION bin
)

install(FILES ../shared/assets/clipdeck.svg
  DESTINATION share/tray
)

install(DIRECTORY ../script-editor/dist/
  DESTINATION share/script-editor
)
