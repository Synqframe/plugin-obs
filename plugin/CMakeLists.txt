find_package(Qt6 REQUIRED COMPONENTS Core Network Widgets)
find_package(CURL REQUIRED)

configure_file(version.h.in "${CMAKE_CURRENT_BINARY_DIR}/version.h")

add_subdirectory(core)

add_library(synqframe MODULE)
add_library(OBS::synqframe ALIAS synqframe)

target_sources(
  synqframe
  PRIVATE
    module.cpp
    client-config.cpp
    client-config.hpp
    global-audio.cpp
    global-audio.hpp
    go-live-dock.cpp
    go-live-dock.hpp
    obs-connect.hpp
    obs-browser-panel.hpp
    contribution-output.cpp
    contribution-output.hpp
    golive.cpp
    golive.hpp
    multitrack-publish.cpp
    multitrack-publish.hpp
    time-client.cpp
    time-client.hpp
    client-settings.cpp
    client-settings.hpp
    bundle-filter.cpp
    plugin-signals.cpp
    page-sources.cpp
    slot-groups.cpp
    bundle-filter.hpp
    aitum-vertical.cpp
    aitum-vertical.hpp
    layout-stacking.hpp
    # Twitch's BPM SEI for the program ladder, injected into each ladder track's keyframes.
    bpm-output.cpp
    bpm-output.hpp
    scene-sync.hpp
)

target_include_directories(synqframe PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}" "${CMAKE_SOURCE_DIR}")
target_compile_features(synqframe PRIVATE cxx_std_20)
target_compile_definitions(synqframe PRIVATE _CRT_SECURE_NO_WARNINGS)
# HTTPS on libcurl, as OBS does its own (network-request.hpp), and the layout and config sockets.
target_link_libraries(synqframe PRIVATE OBS::obs-frontend-api OBS::libobs synqframe-core synqframe-protocol-net Qt::Core Qt::Network
                                        Qt::Widgets CURL::libcurl)
if(WIN32)
  # The GoLive request's GPU list (multitrack-publish.cpp), as OBS gathers it; DPAPI for the saved
  # tokens (client-settings.cpp).
  target_link_libraries(synqframe PRIVATE dxgi crypt32 ws2_32)
endif()

set_target_properties(synqframe PROPERTIES PREFIX "" OUTPUT_NAME "synqframe" AUTOMOC ON AUTORCC ON)

# The plugin's calls to the service (network-request.hpp) against a fake server; Qt only, no OBS.
add_executable(network-request-test tests/network-request-test.cpp)
target_include_directories(network-request-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_SOURCE_DIR}")
# Qt Network only for the fake service the test runs; the plugin's calls are libcurl.
target_link_libraries(network-request-test PRIVATE Qt::Core Qt::Network synqframe-protocol-net)
target_compile_features(network-request-test PRIVATE cxx_std_20)

# The GoLive request and answer handling (golive.hpp); Qt only, no OBS.
# Scene integrity's plan for one canvas (scene-sync.hpp); no OBS, no Qt.
add_executable(scene-sync-test tests/scene-sync-test.cpp)
target_include_directories(scene-sync-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(scene-sync-test PRIVATE cxx_std_20)

# The layout's stacking rule (layout-stacking.hpp); no OBS, no Qt.
add_executable(layout-stacking-test tests/layout-stacking-test.cpp)
target_include_directories(layout-stacking-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(layout-stacking-test PRIVATE cxx_std_20)

add_executable(golive-test tests/golive-test.cpp golive.cpp)
target_include_directories(golive-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(golive-test PRIVATE Qt::Core)
target_compile_features(golive-test PRIVATE cxx_std_20)

# Keep deployment into a portable OBS tree reproducible and limited to this
# plugin. Install with:
#   cmake --install <build-dir> --config RelWithDebInfo \
#     --component synqframe --prefix <obs-portable-dir>
install(
  TARGETS synqframe
  RUNTIME DESTINATION "obs-plugins/64bit"
  LIBRARY DESTINATION "obs-plugins/64bit"
  COMPONENT synqframe
)
install(
  DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/data/"
  DESTINATION "data/obs-plugins/synqframe"
  COMPONENT synqframe
)
