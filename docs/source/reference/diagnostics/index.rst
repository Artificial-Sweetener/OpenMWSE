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
workload while making repeated candidate comparisons independent of physical input timing. The controller measures every route
frame in bounded segments, writes ``performance-segment-*.txt``, captures full graphics evidence at each segment boundary, and
then closes and cleans the isolated child. Use the same capsule, build configuration, driver state, and replay command for every
baseline and candidate repetition.

Add ``--capture-route-frames`` when diagnosing temporal rendering defects. The controller stores the exact final composite for
every executed route frame together with frame-matched shadow-map depth and projection metadata in bounded, versioned
``route-frames-*.omwdiag`` bundles inside the artifact directory. The capture uses the isolated graphics context and leaves the
normal game profile untouched.
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
* ``graphics@1`` captures scene color and depth, resolved atmospheric volumes and metadata, shafts, and the final composite over
  normal and diagnostic-isolation stages.
* ``gameplay@2`` records the cell, camera position and angles, first-person and underwater state, weather, game and simulation
  clocks, and state. The replay controller checks these stable fields before advancing the simulation.
* ``performance@13`` records a bounded per-frame window plus median, p95, p99, and worst distributions for frame pacing, CPU
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
  eight-frame readback delay, and collection retains two
  additional rendered frames before reporting the final sample.
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
  and distortion intervals. Groundcover submission and primitive near/far work are attributed separately to each named camera
  pass. It also reports a bounded stable counter schema for static rendering, attachment resolves,
  transparent and first-person replay, actors, lights, groundcover,
  deformation, uploads, caches, and optimized-path fallbacks. Counters and pass timings not produced by their owning subsystem
  are recorded as unavailable rather than inferred. Resolve byte counts describe the logical destination attachment volume
  written by the copies; they are not an estimate of hardware bus traffic. The endpoint remains passive between requests;
  expensive scene, timing-query, and work-counter collection is enabled only for a bounded performance capture and then restored.

A ``.omwdiag`` capsule contains a manifest, effective settings, full loadable world snapshot, stable world state, provider
versions, frame identity, and performance evidence. A route capsule also contains its versioned bounded gameplay route. The
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

Protocol, capsule, provider, and render-bundle versions are independent compatibility boundaries. A provider version describes
the names and semantics in one provider response. Graphics-provider bundle version 1 stores named, dimensioned float attachments;
adding a newly named optional attachment remains compatible, while changing an existing attachment's meaning, layout, numeric
representation, or coordinate convention requires a new bundle or provider version. Readers reject unsupported versions and
must not infer compatibility from the OpenMWSE release number.

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
