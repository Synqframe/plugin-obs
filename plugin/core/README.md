# Synqframe Core

Frontend-neutral Synqframe integration for libobs.

This library owns scene-channel encoder construction and contribution publishing. It links only against `OBS::libobs` and deliberately contains no Qt or OBS Studio frontend API dependencies.

Frontend adapters consume the public headers under `include/synqframe` and translate their native account, session, and UI state into `ChannelSceneConfig` and `ProgramConfig`.

It is currently built as part of an adapter with `add_subdirectory`; it is not itself an OBS module and does not install into the OBS plugins directory.
