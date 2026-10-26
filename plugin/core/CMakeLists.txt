# WebRTC's VAD as plain C (vendor/libfvad/README.md): the speech detector's voice decisions.
set(FVAD_DIR "${PROJECT_SOURCE_DIR}/vendor/libfvad")
add_library(fvad STATIC
  ${FVAD_DIR}/src/fvad.c
  ${FVAD_DIR}/src/signal_processing/division_operations.c
  ${FVAD_DIR}/src/signal_processing/energy.c
  ${FVAD_DIR}/src/signal_processing/get_scaling_square.c
  ${FVAD_DIR}/src/signal_processing/resample_48khz.c
  ${FVAD_DIR}/src/signal_processing/resample_by_2_internal.c
  ${FVAD_DIR}/src/signal_processing/resample_fractional.c
  ${FVAD_DIR}/src/signal_processing/spl_inl.c
  ${FVAD_DIR}/src/vad/vad_core.c
  ${FVAD_DIR}/src/vad/vad_filterbank.c
  ${FVAD_DIR}/src/vad/vad_gmm.c
  ${FVAD_DIR}/src/vad/vad_sp.c
)
target_include_directories(fvad PUBLIC "${FVAD_DIR}/include")
set_target_properties(fvad PROPERTIES POSITION_INDEPENDENT_CODE ON LINKER_LANGUAGE C)
# Upstream's int-to-int16 narrowing (spl_inl.h) and an unused local; vendored unchanged.
if(MSVC)
  target_compile_options(fvad PRIVATE /wd4244 /wd4189)
endif()

add_library(synqframe-core STATIC
  src/capture-time-stamper.cpp
  src/program-publisher.cpp
  src/service.cpp
  src/speech-detector.cpp
)

# The capture-time SEI and packed alpha (synqframe-protocol), shared with the compositor worker.
target_link_libraries(synqframe-core PUBLIC OBS::libobs synqframe-protocol PRIVATE fvad)
target_include_directories(synqframe-core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(synqframe-core PUBLIC cxx_std_20)
set_target_properties(synqframe-core PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Speech debounce timing and fvad on synthetic audio; no libobs.
add_executable(speech-detector-test tests/speech-detector-test.cpp src/speech-detector.cpp)
target_include_directories(speech-detector-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(speech-detector-test PRIVATE cxx_std_20)
target_link_libraries(speech-detector-test PRIVATE fvad)
