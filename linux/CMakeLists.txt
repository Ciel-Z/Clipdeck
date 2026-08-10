cmake_minimum_required(VERSION 3.13)

project(ClipDeckLinux VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_INSTALL_RPATH "$ORIGIN/../lib")

find_package(Qt5 5.11 REQUIRED COMPONENTS Core Gui Widgets Sql X11Extras)
find_package(X11 REQUIRED)
find_library(XTST_LIBRARY Xtst)

if(NOT XTST_LIBRARY)
  message(FATAL_ERROR "libXtst was not found")
endif()

add_executable(clipdeck-linux
  src/main.cpp
  src/history_repository.cpp
  src/history_repository.h
)

target_include_directories(clipdeck-linux PRIVATE ${X11_INCLUDE_DIR})
target_compile_definitions(clipdeck-linux PRIVATE QT_NO_KEYWORDS)
target_link_libraries(clipdeck-linux PRIVATE
  Qt5::Core
  Qt5::Gui
  Qt5::Widgets
  Qt5::Sql
  Qt5::X11Extras
  ${X11_LIBRARIES}
  ${XTST_LIBRARY}
)

install(TARGETS clipdeck-linux
  RUNTIME DESTINATION bin
)

install(FILES ../mac/Assets.xcassets/AppIcon.appiconset/icon-256.png
  DESTINATION share/icons/hicolor/256x256/apps
  RENAME clipdeck.png
)
