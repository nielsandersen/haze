cmake_minimum_required(VERSION 3.21)
project(haze LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Gui Network Qml Quick QuickControls2 Test)
# The haze itself is a layer-shell surface above the windows, one per screen.
find_package(LayerShellQt REQUIRED)

# Everything but main.cpp, shared by the app and the tests, so each source
# compiles once and a new file in src/ needs no line here.
file(GLOB haze_sources CONFIGURE_DEPENDS src/*.cpp src/*.h)
list(REMOVE_ITEM haze_sources ${CMAKE_SOURCE_DIR}/src/main.cpp)
add_library(haze_core STATIC ${haze_sources})
target_include_directories(haze_core PUBLIC src)
target_link_libraries(haze_core PUBLIC
  Qt6::Core Qt6::Gui Qt6::Network Qt6::Qml Qt6::Quick Qt6::QuickControls2 LayerShellQt::Interface)
target_compile_options(haze_core PUBLIC -Wall -Wextra)

add_executable(haze src/main.cpp src/resources.qrc)
target_link_libraries(haze PRIVATE haze_core)

# The tests load the real Main.qml and Overlay.qml, so they carry the resources too.
enable_testing()
add_executable(haze_tests tests/haze_tests.cpp src/resources.qrc)
target_link_libraries(haze_tests PRIVATE haze_core Qt6::Test)
add_test(NAME haze_tests COMMAND haze_tests)
set_tests_properties(haze_tests PROPERTIES ENVIRONMENT QT_QPA_PLATFORM=offscreen)

# `cmake --install build --prefix ~/.local` puts the app in the launcher
# before it is packaged; a package installs the same files under /usr.
include(GNUInstallDirs)
install(TARGETS haze RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES share/haze.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES share/haze.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/haze)
