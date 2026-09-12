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
   Content that cannot be represented uses baseline rendering.

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

   Pack compatible GPU-scene textures into arrays or bindless storage.
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
