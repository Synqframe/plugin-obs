cmake_minimum_required(VERSION 3.28...3.30)
project(synqframe-obs-plugin VERSION 0.2.0 LANGUAGES C CXX)

if(DEFINED OBS_SOURCE_DIR)
  file(TO_CMAKE_PATH "${OBS_SOURCE_DIR}" OBS_SOURCE_CMAKE_DIR)
  list(APPEND CMAKE_MODULE_PATH "${OBS_SOURCE_CMAKE_DIR}/cmake/finders")
endif()

# The Synqframe plugin for OBS Studio (plugin/). It runs inside OBS, needs Qt for its docks and
# obs-frontend-api to reach the running session, and is built and tested on Windows today (its
# GoLive request reads the GPUs through DXGI, and saved tokens go through DPAPI).
find_package(libobs CONFIG REQUIRED)
find_package(obs-frontend-api CONFIG REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Core Network Widgets)

# The wire formats and clients the plugin shares with the Synqframe service (MIT; its README).
add_subdirectory(vendor/synqframe-protocol)

add_subdirectory(plugin)
