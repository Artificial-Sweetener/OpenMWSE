Diagnostic bridge
=================

OpenMWSE includes a diagnostic bridge that runs the built ``openmw`` executable through its normal bootstrap, save loading,
simulation, rendering, and resource paths. It captures reproducible evidence for graphics, gameplay, and performance problems
without writing to the source game profile. The bridge is the authoritative full-scene diagnostic path; focused unit and shader
tests remain the authority for their smaller ownership boundaries.

Execution modes
---------------

``openmw-diagnosticctl`` uses the same bounded command protocol in both modes:

* An enabled interactive session accepts local inspection, graphics capture, gameplay and performance traces, capsule capture,
  and shader reload commands. Interactive sessions reject deterministic stepping, world-state mutation, and remote shutdown.
* ``run`` launches ``openmw`` as a windowless child with a production offscreen OpenGL context, copied configuration, temporary
  user data, device-free input state, disabled sound, and a fixed simulation step. ``replay`` adds the validated save and world
  state from a capsule. The controller rejects a visible or foreground child window, waits for readiness and capture barriers,
  records evidence, shuts down and reaps the child, proves temporary-workspace cleanup, and verifies that the source profile did
  not change.
  The offscreen frame boundary explicitly submits queued OpenGL commands without waiting for GPU completion.
* ``run --headed`` and ``replay --headed`` use the same isolated child, copied state, device-free input, evidence providers, and
  cleanup contract in a visible production graphics window. This mode supports representative driver presentation behavior and
  visible route-parity review without automating or modifying the normal game profile.

Interactive OpenMWSE builds start the protected current-user endpoint by default. Set ``OPENMW_DIAGNOSTICS=0`` or pass
``--diagnostic-session off`` to disable the bridge for a launch. ``OPENMW_DIAGNOSTICS=1`` and ``--diagnostic-session default``
enable the endpoint for builds configured with ``OPENMW_ENABLE_DIAGNOSTICS_BY_DEFAULT=OFF``. An explicit protected endpoint
with ``--diagnostic-session`` and an artifact directory with ``--diagnostic-artifacts`` support coordinated development
sessions. ``OPENMW_DIAGNOSTIC_ARTIFACTS`` selects the default artifact directory for launcher-started interactive sessions when
``--diagnostic-artifacts`` is absent. Disabled sessions perform no diagnostic transport or statistics collection.

Capture and replay
------------------

Run a windowless production capture from an existing save::

  openmw-diagnosticctl run --openmw <build>\openmw.exe --config <profile>\openmw.cfg \
      --save <profile>\userdata\saves\<character>\<save>.omwsave --artifacts <capture-directory> \
      --frames 120 --performance-samples 64

Replay that capture after rebuilding::

  openmw-diagnosticctl replay --openmw <build>\openmw.exe --config <profile>\openmw.cfg \
      --capsule <capture-directory>\capture.omwdiag --artifacts <replay-directory> \
      --frames 120 --performance-samples 64

Attach an authored route to an existing save capsule without launching the engine::

  openmw-diagnosticctl add-route --capsule <capture-directory>\capture.omwdiag \
      --route <route-directory>\route.yaml --output <artifact-directory>\motion.omwdiag

The command validates the version-1 route and writes a new capsule. The source remains unchanged; an existing output or a
source capsule already containing a route is rejected. Route YAML contains ``route-version: 1`` and a ``frames`` sequence.
Each frame specifies ``dt`` in seconds within (0, 0.2], ``cell`` as a serialized cell identity, ``position`` in world units,
``rotation`` and ``camera`` as three-component radians, and ``first-person`` as a boolean. An exterior cell identity is
``Esm3ExteriorCell:2:8``; interior identities are content record IDs. Routes contain at most 10,000 frames.
Replay the resulting capsule with the command above to execute the route in an isolated headless child.

Record a representative gameplay route from an interactive session by pressing Shift+F11 after loading the desired save. Play
the route once, then press Shift+F11 again. OpenMWSE writes a timestamped ``gameplay-route-*.omwdiag`` capsule beneath the
diagnostic artifact directory. The capsule contains the initial loadable world snapshot and one bounded sequence of rendered
player transforms, cells, camera angles, view modes, and frame durations. Recording stops automatically at 10,000 frames.

Replay the recorded route in a visible isolated child after rebuilding::

  openmw-diagnosticctl replay --openmw <build>\openmw.exe --config <profile>\openmw.cfg \
      --capsule <artifact-directory>\gameplay-route-<timestamp>.omwdiag \
      --artifacts <replay-directory> --headed

Route replay applies the recorded player and camera state authoritatively at each world update; it does not synthesize keyboard,
mouse, physics, or gameplay decisions. This preserves the recorded viewport, cell traversal, streaming pressure, and rendering
workload while making repeated candidate comparisons independent of physical input timing. The controller verifies that each
scheduled route state was consumed before accepting a segment. Rendering frames without advancing the route is a replay failure.
Performance collection spans every renderer frame used by the requested route states, including loading-screen draws.
The ``route-frame-map`` associates each segment-local ``route-ordinal`` with its ``first-render-frame`` (world update) and
``scene-render-frame`` (completed scene traversal). Use this map when pairing route states; a raw renderer-frame ordinal is
not a route ordinal. Loading frames remain in the complete trace with the viewer's ``Loading`` statistic. Collections that
outgrow retained statistics fail explicitly; use fewer ``--performance-samples`` per segment for those routes.
Route image sequences begin at the scheduled route boundary. Older draw-thread frames already queued when the request arrives
are excluded, so they cannot consume image slots intended for the route's final states.
The controller measures every route
frame in bounded segments, writes ``performance-segment-*.txt``, captures full graphics evidence at each segment boundary, and
then closes and cleans the isolated child. Use the same capsule, build configuration, driver state, and replay command for every
baseline and candidate repetition.

Add ``--capture-route-frames`` when diagnosing temporal rendering defects. The controller stores the exact final composite for
every executed route frame together with frame-matched shadow-map depth and projection metadata in bounded, versioned
``route-frames-*.omwdiag`` bundles inside the artifact directory. The capture uses the isolated graphics context and leaves the
normal game profile untouched. Preview frames are limited to 1024 pixels on their longest edge.
Use ``--capture-route-native-frames`` for pixel-scale temporal defects. Native captures preserve the rendering resolution
by streaming each frame directly to a partial bundle and publishing the completed sequence. Each frame is limited to
64 million RGBA pixels; only the current frame and its shadow attachments are retained in memory.
Use ``--skip-graphics-capture`` to omit full attachment captures at route checkpoints.
Use ``--bypass-route-shadows`` with the same replay to produce a frame-aligned conventional-shadow comparison; shadow rendering
is restored before boundary graphics evidence is captured.

For temporal investigation, ``--frames`` limits replay to an exact route prefix and ``--performance-samples`` selects the
checkpoint interval up to 600 frames. Repeating isolated replays with increasing frame limits captures unperturbed graphics at
specific points along the same recorded motion without allowing an earlier graphics readback to influence a later checkpoint.

For single-state captures, ``--frames`` advances the exact fixed-step warm-up workload before capture.
``--performance-samples`` then collects a bounded future window without advancing the isolated simulation clock. The performance
report keeps every sampled OSG statistic and summarizes stable CPU,
cull, draw, camera-owned GPU-pass, and frame-duration metrics with median, p95, p99, and worst values. Named
GPU passes cover the main scene, shadow maps, water reflection, sky render target, post-processing, precipitation occlusion,
and ripple simulation. A sample count must not exceed the frame count or 600. Visual capture begins after the timed window so
synchronous readback does not contaminate its distributions.

Render-to-texture cameras join diagnostic collection when they are created during culling. This includes water reflections,
whose camera lifetime is managed per view outside the traversable scene graph.

Atmosphere captures can replace save-derived weather and game hour without changing the save or its region probabilities.
``--weather <name>`` locks one steady weather. ``--weather-from <name> --weather-to <name>
--weather-transition <progress>`` locks an exact transition, where progress is within ``[0, 1]`` from the first weather to the
second. ``--game-hour <hour>`` locks civil game time within ``[0, 24)`` while simulation time and cloud advection continue.
``--start-game-hour <hour>`` sets civil time once and then advances it at the save's time scale.
Use it with a route to reproduce evolving dawn or sunset history; it is mutually exclusive with ``--game-hour``.
The version-7 atmosphere scenario preserves this advancing-time control, and earlier scenarios retain their locked-time semantics.
``--cloud-seed <integer>`` selects a deterministic weather-episode identity, including its authored cloud family,
bounded variation, and field phase. Seed zero selects the authored baseline without episode realization.
Visible clouds, reflections, and surface-shadow placement consume the same identity; the seed does not change
the simulation clock or add a separate wind displacement.
``--recent-precipitation <weight>`` sets the condensate controller's bounded recent-rain memory within ``[0, 1]``. It is useful
for paired dry and post-rain mist captures because it changes mist formation without inventing a different current weather,
fog policy, or lighting path.
``--environment-traits <name=weight,...>`` replaces classified environment evidence with exact semantic weights in ``[0, 1]``.
``--environment-terrain-height <world-units>`` optionally replaces the local terrain datum. These isolated-child-only controls
apply the production palette and medium profile immediately, allowing deterministic wetland, coastal, volcanic, and future
environment scenarios without waiting for a regional transition or encoding region names in the renderer.
``--camera-pitch <radians>``, ``--camera-yaw <radians>``, and ``--camera-roll <radians>`` replace individual camera
angles after replay parity is established. Camera overrides are isolated-child-only, reset atmosphere history, and are
recorded in the resulting scenario capsule without changing the source save.
Every scenario change rejects temporal atmosphere history. ``--atmosphere-warmup-frames <count>`` then advances up to 600 exact
fixed-step frames before the measured workload, allowing the same command surface to capture either cold history or a controlled
settled history. The child reports requested-versus-observed scenario parity before advancing.

The initial ``capture.omwdiag`` retains the unmodified save state. A run with atmosphere overrides also writes
``scenario.omwdiag`` with ``configuration/atmosphere-scenario.yaml`` and the overridden world snapshot. Replaying that capsule
automatically reapplies and locks its scenario; explicit command-line atmosphere options replace the capsule scenario for a
controlled comparison.

Use ``--disable-main-clouds``, ``--disable-low-clouds``, ``--disable-mist``, and ``--disable-fog`` to isolate participating
media while retaining the selected production weather, lighting, shaders, assets, and scene. These masks are accepted only by
the isolated diagnostic child, are recorded in the versioned atmosphere scenario, and do not mutate weather data or the normal
profile.

``--disable-distance-fog`` independently removes exponential or linear view-path aerial perspective for cloud-shape diagnosis.
It does not disable volumetric fog, mist, or cloud media unless their corresponding controls are also supplied.

The ``atmosphere-cloud-terrain-v1`` attachment records the rendered world terrain cache. Its first RGBA texel
contains the world origin XY, lattice spacing, and side length; the second contains minimum height, maximum height, and side length.
Subsequent texels contain four scalar heights each in row-major order, with unused trailing channels set to zero.

Atmosphere bundles include ``atmosphere-cloud-models-v2``, a 14 by 3 RGBA attachment containing the published
outgoing, incoming, and environmental model controls in ``SceneUtil::packCloudLayer`` order. It records the actual
post-processing uniform snapshot, including gradients, deformation, noise scale, and optical coefficients.

Every isolated run writes its final ``gameplay.yaml`` evidence. Use ``--skip-graphics-capture`` for state or
performance-only runs that do not need production attachments. Use ``--normal-only-graphics-capture`` for deterministic
morphology sweeps that need the normal production frame but not every shader-route comparison. Full visual qualification uses
the default comparison capture or exact route-frame bundles.

Repeat ``--setting "Section:name=value"`` to apply a validated setting override only to the temporary profile used by that run
or replay. The controller leaves the source profile unchanged, making the option suitable for controlled configuration and
fallback comparisons.

Compare the real production attachments from both runs::

  python scripts\compare_diagnostic_graphics.py <capture-directory>\graphics-<frame> \
      <replay-directory>\graphics-<frame> --output <replay-directory>\graphics-parity.json \
      --heatmaps <replay-directory>\graphics-heatmaps

The comparison requires an explicit attachment-specific rule for every captured image, validates matching stages and dimensions,
and rejects non-finite values. Its machine-readable report records exact differing-value counts, mean, 99.9th-percentile, and
maximum absolute error, maximum relative error, and the bounds and size of localized differences. ``--heatmaps`` writes
full-resolution logarithmic difference images for the normal stage and any failed attachment. The command exits unsuccessfully
when an attachment exceeds its checked-in replay-parity limits.

Attach commands
---------------

The controller accepts ``status``, ``graphics``, ``gameplay``, ``performance``, ``capsule``, and ``reload-shaders`` against an
enabled session. Supply ``--endpoint <name>`` when the session does not use the default endpoint. ``graphics`` schedules the same
production pass capture used by Shift+F12. ``capsule --output <name>.omwdiag`` writes beneath the session artifact directory.
``performance --performance-samples <count>`` collects and reports a bounded future window of up to 600 frames. ``step`` and ``shutdown``
are child-only operations.

Benchmark scenarios
-------------------

Performance comparisons use named scenario roles rather than treating one save as representative of every subsystem:

* ``heavy-outdoor`` covers dense object paging, distant terrain, groundcover, shadows, water reflection/refraction, atmospheric
  effects, and post-processing in one production exterior.
* ``water-boundary`` covers above-water, near-plane, and below-water views with reflected, refracted, clipped, and transparent
  content.
* ``light-interior`` covers the non-clustered light-selection path with overlapping local lights and mixed object bounds.
* ``actor-crowd`` covers range queries, head tracking, combat discovery, collision avoidance, animation, skinning, and morphing.
* ``transparency-first-person`` covers blended ordering, alpha clipping, transparent post depth, first-person depth and FOV, and
  position-reconstructing post effects.
* ``capability-fallback`` exercises the legacy or reduced-capability path affected by a candidate optimization.

Each concrete scenario is a private capsule plus its ordered content hashes and effective settings. Record the capsule identity,
build, hardware, driver, fixed step, warm-up, sample window, and repetition count beside its artifacts. Use at least one nearby
non-target scenario when retaining an optimization. Loading, shader compilation, and streaming frames are reported separately;
they are not silently removed from steady-state distributions. ``inspect-capsule --entry state/world.yaml`` prints a bounded
text entry for scenario inventory without extracting or materializing the private world snapshot.

Providers and artifacts
-----------------------

The versioned providers expose these evidence domains:

* ``core@2`` records the effective video, camera, atmosphere, and complete shadow configuration used by the captured engine
  instance so replay evidence can attest to rendering-setting parity.
* ``graphics@3`` captures scene color and depth, resolved atmospheric volumes and metadata, shafts, and the final composite over
  normal and diagnostic-isolation stages. It also supports native-resolution route composites;
  ``graphics@1`` supports 1024-pixel route previews.
* ``gameplay@8`` records the cell, camera position and angles, first-person and underwater state, current and next weather,
  weather-transition progress, game hour, game and simulation clocks, state, continuous terrain-derived environment traits,
  sparse terrain sample center, local gradient, valley depth,
  and the applied fog range and resolved cloud, low-cloud, mist, fog, and terrain-atmosphere parameters. It also records the
  canonical sun direction, solar/sky/ground illumination, sun-disc state, resolved atmospheric media, and the exact world-space
  cloud-field offset used by procedural density and shadow evaluation. Readers also accept ``gameplay@4``, ``gameplay@5``,
  ``gameplay@6``, and ``gameplay@7`` capsules.
  The replay controller checks stable save fields before applying any explicit atmosphere scenario and advancing the simulation.
* ``performance@15`` records a bounded per-frame window plus median, p95, p99, and worst distributions for frame pacing, CPU
  scopes for scripts, Lua, mechanics, physics, world, and UI, resource caches and work queues, navigation and paging activity,
  scene statistics, delayed per-camera GPU/render timing, and camera-owned GPU passes for the main scene, shadow maps, water
  reflection, sky render target, post-processing, precipitation occlusion, ripples, and terrain-composite generation. It divides
  draw traversal wall time into
  camera-covered work and the intervals before, between, and after camera stages. Each surrounding interval is also attributed
  to its adjacent named camera boundaries, while aggregate and per-camera timing reports camera work outside the versioned
  named passes. The GPU timeline reports elapsed GPU-clock spans across instrumented cameras, their union, and named intervals
  between adjacent camera boundaries. These intervals may include time awaiting CPU submission and therefore do not by themselves
  measure GPU utilization. Unclassified cameras report both GPU and CPU time, and the report identifies the viewer threading
  model used for the capture. On Windows, camera passes also report render-thread hardware cycles so active execution can be
  distinguished from elapsed time spent blocked or descheduled. The report measures callback work outside its own camera
  intervals so delayed GPU-query maintenance can be separated from engine draw work. GPU queries are polled only after an
  eight-frame readback delay. After two additional rendered frames, collection reports once all recorded camera query pairs
  in the requested interval have retired. It continues rendering while pairs remain pending, stopping before the 1,024-frame
  statistics history can overwrite the first requested sample. These readback frames do not extend the measured interval;
  ``readback-rendered-frames`` records their count. Per-camera ``gpu-query-status`` counts distinguish
  unsupported timestamps, pending pairs, completed pairs, invalid results, and pairs dropped at the retention limit.
  ``not-recorded`` counts frames without a query lifecycle record, including frames when that camera did not render.
  Pending or unsupported results are not zero-duration samples; their durations remain unavailable.
  Groundcover attribution includes submitted model batches and drawables plus a diagnostic-only conservative per-instance
  frustum and distance test of instances accepted by cluster visibility. The additional instance test runs only during bounded
  scene-statistics collection and measures potential GPU rejection without changing submitted work.
  Each shadow map reports its own CPU time, GPU time, post-sort render leaves, state graphs, directional-light vertical
  magnitude, and scene-linear luminance so aggregate shadow cost can be traced to a specific map, its submitted work, and the
  low-sun light state. Terrain-composite detail reports the scheduler budget, queue
  depth before and after the pass, immediate and processed map counts, submitted drawables, completed maps, and drawable CPU
  duration so an over-budget map can be distinguished from queue pressure, drawable work, or GPU cost. Presentation detail
  reports time blocked in buffer swap. Main-camera render-bin intervals further
  identify the scene root and its nested early-sky, attachment-resolve, transparency, occlusion-query, first-person, sun-glare,
  and distortion intervals. Raw render-bin CPU statistics include a separate ``callback overhead`` duration for timestamp
  allocation, retirement, and timing publication, excluding delegated rendering and the overhead statistic's own publication.
  Groundcover submission and primitive near/far work are attributed separately to each named camera
  pass. It also reports a bounded stable counter schema for static rendering, attachment resolves,
  transparent and first-person replay, actors, lights, groundcover,
  deformation, uploads, caches, and optimized-path fallbacks. Counters and pass timings not produced by their owning subsystem
  are recorded as unavailable rather than inferred. Resolve byte counts describe the logical destination attachment volume
  written by the copies; they are not an estimate of hardware bus traffic. The endpoint remains passive between requests;
  expensive scene, timing-query, and work-counter collection is enabled only for a bounded performance capture and then restored.

A ``.omwdiag`` capsule contains a manifest, effective settings, full loadable world snapshot, stable world state, provider
versions, frame identity, and performance evidence. A scenario capsule also contains its versioned deterministic atmosphere
inputs, while a route capsule contains its versioned bounded gameplay route. The
manifest records the ordered content list and hashes. It does not embed installed game or mod assets. Graphics bundles remain
beside the capsule because they are evidence from the run rather than inputs required to reproduce it. The controller also
preserves the isolated engine and crash logs beneath the artifact directory's ``logs`` folder before removing its temporary
workspace.

Compatibility and failure behavior
----------------------------------

Capsules and commands carry explicit format versions. Replay fails before controlled execution when the capsule is corrupt,
unsupported, oversized, contains unsafe names, omits required state, or does not match the locally installed ordered content and
hashes. Artifact writes are bounded, validated, atomic, and refuse replacement. The controller reports structured protocol error
names and preserves engine error context.

Protocol version 2 adds the isolated-child atmosphere-scenario command. Protocol, capsule, provider, scenario, and render-bundle
versions are independent compatibility boundaries. A provider version describes
the names and semantics in one provider response. Graphics-provider bundle version 1 stores named, dimensioned float attachments;
adding a newly named optional attachment remains compatible, while changing an existing attachment's meaning, layout, numeric
representation, or coordinate convention requires a new bundle or provider version. Readers reject unsupported versions and
must not infer compatibility from the OpenMWSE release number.

Checkerboard atmosphere traces use the named attachments ``atmosphere-volume-checkerboard-raw`` and
``atmosphere-volume-checkerboard-metadata-raw``. Their texels represent one selected member of each 2x2 resolved block,
not a uniform ray grid. The optional 1x1 RGBA ``atmosphere-checkerboard-layout`` records frame phase (0–3), resolved width,
resolved height, and block size (2). Phase comes from the published post-processing uniform snapshot, not
the viewer's potentially newer frame stamp. Block (x,y) selects index (phase + (x+y) mod 2) mod 4, with offset
(index mod 2, floor(index/2)). Resolved radiance and metadata retain their uniform ray-grid attachment names.
The optional ``atmosphere-volume-metadata-reprojected`` attachment exposes the half-resolution reconstruction
decision before fresh-ray repair. Its alpha is negative only where an untraced pixel lacks usable history;
the repair pass integrates those rays and writes final metadata with the usual nonnegative medium identifiers.
The production comparison tool enforces numeric parity for these phase-dependent attachments when both
captures use the same recorded phase. When phases differ, it validates their dimensions and finite values,
records the phase mismatch, and continues to enforce parity on the resolved radiance, resolved metadata,
scene depth, and final composite attachments.

The fixed-step scheduler controls simulation and rendering clocks after loading the real save. Readiness and graphics-completion
barriers prevent capture races. A disconnected controller cancels pending work, and controller destruction terminates and reaps
its child. Engine commands run at the main-thread safe point after Lua garbage collection is quiescent and before world updates.
Render capture state is restored after every diagnostic stage.

Security and privacy
--------------------

The bridge has no network listener, shell, script-evaluation, arbitrary file, native-call, or memory-access command. Windows uses
a named pipe restricted to the current user and rejects remote clients; Unix uses a current-user Unix-domain socket. Protocol
frames, queues, connections, capsule entries, attachment sizes, step counts, paths, and timeouts are bounded.

Isolated runs copy only the configuration and save state needed to bootstrap OpenMW. Installed content remains in place and is
read through the normal virtual filesystem. The engine reports its selected graphics context and device-free input owner. In
windowless mode the controller independently checks that the child owns no visible or foreground desktop window; headed mode
explicitly permits the isolated child window. Temporary configuration, user data, materialized saves, and storage are removed
when the run ends; engine logs are retained as diagnostic artifacts. A recursive fingerprint proves that configuration, saves,
mods, and other writable profile state are unchanged; screenshots are excluded because the controller never writes to their
directory.

Treat artifact directories as private diagnostic data. Capsules contain the captured save state, content names and hashes, while
logs and traces can contain local paths, cell names, and runtime state. Review an artifact set before sharing it.

Maintenance
-----------

Add a provider only when its owning subsystem can expose a bounded, versioned snapshot at an established safe point. Keep policy
and serialization in ``components/diagnostics`` and domain collection in the owning engine subsystem. Extend protocol and capsule
tests for malformed, boundary, cancellation, and compatibility behavior. Validate full-scene changes with capture, rebuild,
replay, and the numerical comparison; do not reproduce production rendering or simulation in a synthetic parallel harness.
Use focused graphics-context tests for individual shaders, equations, permutations, asset-loading boundaries, and edge cases.
Use the bridge for full-scene images, cross-pass evidence, production-asset interactions, and performance measurements.
