cmake_minimum_required(VERSION 3.21)

project(ClipDeckSpike VERSION 0.0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Qml Quick Widgets)
qt_standard_project_setup(REQUIRES 6.5)

qt_add_executable(clipdeck-spike WIN32
  src/main.cpp
  src/global_hotkey.cpp
  src/global_hotkey.h
  src/paste_flow.cpp
  src/paste_flow.h
  src/spike_controller.cpp
  src/spike_controller.h
  src/spike_images.cpp
  src/spike_images.h
  src/spike_log.cpp
  src/spike_log.h
  src/win32_util.cpp
  src/win32_util.h
  src/window_effects.cpp
  src/window_effects.h
)

qt_add_qml_module(clipdeck-spike
  URI ClipDeckSpike
  VERSION 1.0
  QML_FILES
    qml/Main.qml
    qml/Card.qml
    qml/DeckIcon.qml
    qml/LinkIcon.qml
    qml/SourceBadge.qml
)

qt_add_resources(clipdeck-spike "tray_icon"
  PREFIX "/"
  BASE "${CMAKE_CURRENT_SOURCE_DIR}/../../shared/assets/Assets.xcassets/AppIcon.appiconset"
  FILES "${CMAKE_CURRENT_SOURCE_DIR}/../../shared/assets/Assets.xcassets/AppIcon.appiconset/icon-32.png"
)

target_compile_definitions(clipdeck-spike PRIVATE
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
  _WIN32_WINNT=0x0A00 WINVER=0x0A00
)

target_link_libraries(clipdeck-spike PRIVATE
  Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::Widgets
  dwmapi user32 advapi32
)
