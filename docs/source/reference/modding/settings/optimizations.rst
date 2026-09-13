Optimizations Settings
######################

The ``[Optimizations]`` category controls optional Project Clockwork-derived behavior.
``enabled`` is a prerequisite for every setting in this category. Turning it off restores baseline OpenMW rendering while
retaining the subordinate selections for later use. Unsupported graphics capabilities fall back to baseline behavior.

.. omw-setting::
   :title: enabled
   :type: boolean
   :range: true, false
   :default: false
   :location: :bdg-info:`In Game > Settings > Options > Video > Optimizations`

   Permit individually selected optional optimizations to become effective.

.. omw-setting::
   :title: gpu driven paging
   :type: boolean
   :range: true, false
   :default: false
   :location: :bdg-info:`In Game > Settings > Options > Video > Optimizations`

   Keep eligible paged objects, loaded-cell statics, and terrain chunks in GPU-managed buffers.
   Requires ``object paging`` and OpenGL 4.3 compute, storage-buffer, and indirect-drawing capabilities.
   Content that cannot be represented uses baseline rendering. OpenMWSE validation observed a substantial regression in one
   dense scene when packed textures were disabled, so measure this path with ``gpu texture arrays`` both off and on.

.. omw-setting::
   :title: gpu skinning
   :type: boolean
   :range: true, false
   :default: false
   :location: :bdg-info:`In Game > Settings > Options > Video > Optimizations`

   Skin eligible actor geometry in the GPU scene. Requires ``gpu driven paging`` and ``gpu actors``.
   CPU skinning remains active for unsupported geometry.

.. omw-setting::
   :title: gpu occlusion culling
   :type: boolean
   :range: true, false
   :default: false
   :location: :bdg-info:`In Game > Settings > Options > Video > Optimizations`

   Reject hidden GPU-scene instances with the scene depth pyramid.
   Compute and depth-generation cost can outweigh the saved drawing in sparse scenes or on slower GPUs.

.. omw-setting::
   :title: gpu actors
   :type: boolean
   :range: true, false
   :default: false
   :location: :bdg-info:`In Game > Settings > Options > Video > Optimizations`

   Render and raycast eligible actors through the GPU scene. Requires ``gpu driven paging``.
   Unsupported actor content and queries use baseline fallbacks.

.. omw-setting::
   :title: gpu texture arrays
   :type: boolean
   :range: true, false
   :default: false
   :location: :bdg-info:`In Game > Settings > Options > Video > Optimizations`

   Pack compatible GPU-scene textures into arrays or bindless storage. Requires ``gpu driven paging``.
   The selected mode depends on GPU and driver capabilities and consumes managed graphics memory.

.. omw-setting::
   :title: indexed light lists
   :type: boolean
   :range: true, false
   :default: false
   :location: :bdg-info:`In Game > Settings > Options > Video > Optimizations`

   Use spatial indexing and pooled state for repeated per-object light-list queries.
   The benefit depends on the number of visible lights, receivers, and views.

.. omw-setting::
   :title: gpu scene lighting
   :type: boolean
   :range: true, false
   :default: false
   :location: :bdg-info:`In Game > Settings > Options > Video > Optimizations`

   Evaluate compatible GPU-scene receivers through clustered lighting.
   Requires ``gpu driven paging`` and clustered-light graphics capabilities.

.. omw-setting::
   :title: gpu storage budget
   :type: boolean
   :range: true, false
   :default: false
   :location: :bdg-info:`In Game > Settings > Options > Video > Optimizations`

   Bound GPU-scene texture and mesh storage using ``object paging gpu texture budget`` from the ``[Terrain]`` category.
   Requires ``gpu driven paging``.
   Automatic budgets use a driver free-memory report only as an initial limit; the report is not usage telemetry.

.. omw-setting::
   :title: low end indirect batching
   :type: boolean
   :range: true, false
   :default: false
   :location: :bdg-info:`In Game > Settings > Options > Video > Optimizations`

   Reduce GPU-scene shadow and indirect-command work through active command ranges and terrain-shadow batching.
   Requires ``gpu driven paging``.
   Clockwork targets lower-throughput GPUs; the extra range bookkeeping may not benefit faster GPUs or every scene.

.. omw-setting::
   :title: gpu light occlusion
   :type: boolean
   :range: true, false
   :default: false
   :location: :bdg-info:`In Game > Settings > Options > Video > Optimizations`

   Cull hidden classic-light receiver work with the GPU-scene depth pyramid.
   Requires ``gpu scene lighting`` and ``gpu occlusion culling``. Compute overhead may outweigh savings in sparse scenes.

.. omw-setting::
   :title: gpu distant paging
   :type: boolean
   :range: true, false
   :default: false
   :location: :bdg-info:`In Game > Settings > Options > Video > Optimizations`

   Page eligible distant day/night geometry and exterior lights through the GPU scene.
   This can increase resident geometry and light memory. OpenMWSE validation observed a substantial regression in one dense
   scene when packed textures were disabled. Unsupported or animated content remains on the baseline path.

.. omw-setting::
   :title: skip dormant ripples
   :type: boolean
   :range: true, false
   :default: false
   :location: :bdg-info:`In Game > Settings > Options > Video > Optimizations`

   Skip water-ripple simulation passes until an emitter activates them while preserving active decay and queued emitters.
   Clockwork observed a small gain on H700 hardware; desktop gains may be negligible.
