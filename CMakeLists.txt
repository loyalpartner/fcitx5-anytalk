cmake_minimum_required(VERSION 3.10)
project(fcitx5-anytalk)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Fcitx5Core REQUIRED)
find_package(Fcitx5Utils REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(JSONC REQUIRED json-c)

add_library(anytalk MODULE
  src/addon.cpp
  src/ipc_client.cpp
)

target_include_directories(anytalk PRIVATE
  ${Fcitx5Core_INCLUDE_DIRS}
  ${Fcitx5Utils_INCLUDE_DIRS}
  ${JSONC_INCLUDE_DIRS}
)

target_link_libraries(anytalk
  Fcitx5::Core
  Fcitx5::Utils
  ${JSONC_LIBRARIES}
)

set_target_properties(anytalk PROPERTIES
  PREFIX ""
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib/fcitx5"
)

install(TARGETS anytalk DESTINATION lib/fcitx5)
install(FILES data/anytalk.conf DESTINATION share/fcitx5/inputmethod)
install(FILES data/anytalk-addon.conf DESTINATION share/fcitx5/addon RENAME anytalk.conf)
file(GLOB ICONS "data/anytalk*.png")
install(FILES ${ICONS} DESTINATION share/fcitx5/icons)
