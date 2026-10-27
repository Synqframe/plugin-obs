# libfvad (vendored)

WebRTC's voice activity detector extracted as plain C, from
https://github.com/dpirch/libfvad at commit `532ab666c20d3cfda38bca63abbb0f152706c369`
(2024-02-19; libfvad 1.0.1, imported from WebRTC `ebd9abc1a23db8fe9acfd163155e0f9bc544b10e`).
Only `include/` and `src/` (unchanged), `LICENSE` (BSD-3-Clause), `PATENTS` and `AUTHORS` are
kept; the build is the `fvad` target in `plugin/core/CMakeLists.txt`. The plugin's
speech detector (`synqframe/speech-detector.hpp`) uses it to decide voice per 10 ms frame.
