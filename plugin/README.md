# Synqframe for OBS

This directory is the complete product boundary for the OBS integration. It
must build against stock OBS without modifications to OBS Studio or libobs.

## Ownership

- OBS owns scenes, sources, profiles, encoders, the main canvas, and the normal
  streaming controls.
- Aitum Vertical (a separate plugin) owns the vertical canvas and its editor.
- The plugin owns the Synqframe Bundle filter (what a source sends), layout synchronization and
  contribution outputs. It has no dock of its own: sign-in, session selection, the
  composition choices and Go Live live in the web dock (`<serviceUrl>/dock`), and
  the plugin applies what the web asks for and reports its state.
- The platform owns accounts, streams, layouts, ingest placement, optional
  cloud composition, and downstream publication.

## Composition modes

- OBS Native publishes the normal horizontal program and native vertical canvas.
- Synqframe Local keeps composition in OBS, publishes one horizontal program by
  default, and sends analysis-grade isolated bundles (maximum 1280x720/30 fps,
  3 Mb/s video, and 96 kb/s AAC). Vertical is an explicit additional encode.
- Synqframe Remote sends composition-grade bundles at their configured targets;
  the platform worker owns program rendering and encoding.

## Integration invariants

1. Use `obs-frontend-api`, `libobs`, and Qt only. Do not include `OBSBasic` or
   locate private OBS widgets by object name.
2. A bundle is an explicit publishable A/V unit with its own resolution, frame
   rate, bitrate and background contract. Source UUIDs and device settings never
   leave the client.
3. Program layouts are server-side compositions of bundle IDs. Native OBS
   scenes are implementation details and are never uploaded as layouts.
4. The `synqframe_bundle_filter` video filter is the native local adapter. Put
   on any source with video (a scene included), it marks that source as a
   bundle and holds the bundle's id, resolution, frame rate, bitrate and
   transport matte; it passes the picture through untouched. The bundle sends
   the source's picture and its own audio (a scene's: everything in it), so a
   mic is tied to a camera with OBS's own controls. Removing or disabling the
   filter makes the source plain OBS again. The old `synqframe_bundle` source
   is gone: when a collection loads, any left in it (and their hidden bundle
   canvases) are removed, and the creator adds the filter to whatever should
   send the bundle.
5. Synqframe output must be an OBS output implementation so the native
   Start/Stop Streaming lifecycle remains authoritative.
6. The vertical canvas is Aitum Vertical's. The plugin finds it by name ("Aitum Vertical"), asks
   `aitum_vertical_get_scene` which scene it shows (`aitum-vertical.cpp`), encodes
   the canvas, and layout sync reads and edits that scene's items like the main
   one's. It never installs Aitum or uses Aitum's own outputs.
   Without it a local production has no vertical output, and OBS shows a notice
   when the web asks for one (`localVerticalProgram`).
7. Start Streaming only publishes feeds; it never makes anything public and
   never calls `/api/live`. Where the program goes is decided in
   `<serviceUrl>/dock`, hosted in an obs-browser panel dock (the vendored
   `obs-browser-panel.hpp` interface, version-gated at runtime) with its own
   cookie profile. Without obs-browser the dock offers to open the page in the
   user's browser. No token ever appears in a URL.
8. Stop Streaming sends `PUT /api/live {"live": false, "reason": "stopped"}` and
   graceful OBS exit sends reason `exit`, each before leaving the session, with
   a short timeout so shutdown is never held up by the network.
9. The web dock configures OBS over a per-client socket
   (`/api/obs/clients/{clientId}/ws`, `contracts/schemas/obs-client-config.schema.json`).
   The plugin applies the desired session and composition settings and reports
   its state, but never starts OBS outputs on a command: Start Streaming is the
   user's. It stops streaming only on `client.stop`, which the server sends when
   a stream put nothing on air for 5 minutes, exactly as Stop Streaming would. Settings that need an output restart are held until
   it stops, and a session switch never touches a running output. The channel is
   on by default; `client/configChannel=false` switches it off.
