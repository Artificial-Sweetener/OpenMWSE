OpenMWSE Water Settings
#######################

The ``OpenMWSE Water`` category owns authored water appearances, classified mesh-water replacement, and geometric-wave controls
that upstream OpenMW does not interpret. Existing ``Water`` settings remain authoritative for enabling shader water, reflection,
refraction, and render-target quality. Appearance and enhanced-water choices apply immediately and remain saved while their
prerequisite is disabled.
The animated MGE XE water volume is shared with submerged material receivers independently from geometric displacement, so
flat water retains caustics and future water-surface providers can retain the same receiver-side projection contract.

.. omw-setting::
   :title: appearance
   :type: string
   :range: openmw, mge-xe, enhanced, vanilla
   :default: openmw
   :location: :bdg-info:`In Game > Options > Video > Water`

   Selects the authored surface presentation. ``openmw`` preserves the existing OpenMW shader without presentation changes.
   ``mge-xe`` follows the standard MGE XE balance, ``enhanced`` enables the credited Enhanced Water Shader treatment, and
   ``vanilla`` provides a high-resolution interpretation of Morrowind's pixel-shader water.

.. omw-setting::
   :title: geometric wave height
   :type: float32
   :range: 0.0 to 250.0
   :default: 0.0
   :location: :bdg-info:`In Game > Options > Video > Water`

   Sets geometric wave height in Morrowind world units for the default world water surface. The numeric scale matches MGE XE:
   a value such as ``30`` supplies the same ``30`` world-unit height directly to the translated MGE XE wave
   equation. Zero retains OpenMW's flat, optimized water geometry path. When caustics are also disabled, it does not allocate
   or render surface-coverage resources. Positive values constrain crests beneath nearby landscape and static geometry. Water becomes geometrically flat
   when its local 512-world-unit neighborhood is fully covered, keeping dense surface mats flat without suppressing waves beside
   shores, hulls, and partially covering structures. Near-coplanar coverings within four world units of the water surface are
   treated as surface mats for both wave obstruction and transparent draw ordering. A translucent mat texture must still be
   used by a mesh with an alpha property so its color composites over the reflective water instead of replacing it.

.. omw-setting::
   :title: mesh water
   :type: boolean
   :range: true, false
   :default: true
   :location: :bdg-info:`In Game > Options > Video > Water`

   Replaces classified authored water materials with renderer-owned water using the selected appearance. Disabling this setting
   immediately restores each source drawable, its original material, and its animated textures. Re-enabling it reclassifies the
   loaded surfaces without reloading the cell. Mesh-specific preferences remain saved while replacement is disabled.

.. omw-setting::
   :title: shared planar reflections
   :type: boolean
   :range: true, false
   :default: false
   :location: :bdg-info:`In Game > Options > Video > Water`

   When disabled, every visible world-water and classified mesh-water plane renders its own direct planar reflection.

   When enabled, uses one source capture at the full configured ``Water/rtt size`` for world water and classified mesh-water planes.
   The provider selects the most visually impactful visible plane from asynchronous depth-tested main-view sample counts,
   with a score advantage and minimum dwell interval. Occluded water and empty projected bounds do not contribute. The
   source uses that receiver's exact direct-reflection plane, clipping, scene eligibility, target format, and sampling
   contract. The anchor samples this complete source image directly at full resolution; it is never reconstructed. Other
   planes use independently sized colour-and-depth reconstructions with receiver-local clipping and reflected sky. Large
   receivers can use 1024- and 2048-pixel output buckets when ``rtt size`` permits. A newly dominant plane must exceed the
   current plane's impact by 20 percent after a 30-frame minimum dwell. The winner remains locked while a second exact source
   renders the new plane to completion. The provider then freezes the prior source and cross-fades the two reprojected endpoint
   textures for 0.3 seconds; no reflection camera moves through an intermediate plane. Either mode transition retains the last completed textures until
   the source and every visible non-anchor replacement target complete, then switches atomically at a frame boundary.

.. omw-setting::
   :title: mesh geometric wave height
   :type: float32
   :range: 0.0 to 250.0
   :default: 0.0
   :location: :bdg-info:`In Game > Options > Video > Water`

   Sets geometric wave height independently for water supplied by classified mesh materials while ``mesh water`` is enabled.
   The value uses the same Morrowind-world-unit scale as default water. Zero retains the authored mesh's flat geometry path.
   Positive values apply the selected water appearance's geometric displacement, while player and weather ripples remain shared
   with all water. A surface-effect profile can override this value for a deliberately calm or unusually rough material.

.. omw-setting::
   :title: caustics
   :type: boolean
   :range: true, false
   :default: false
   :location: :bdg-info:`In Game > Options > Video > Water`

   Enables MGE XE's animated caustic treatment on submerged terrain, statics, foliage, and other compatibility-shader
   receivers. The translated shader projects each receiver to the water plane along the light direction, samples the blue
   channel of the original 256 by 256 by 32 water volume at MGE XE's 1104-world-unit scale and 0.4 time rate, and applies its
   original depth, screen-footprint, fog, and ``caustic - 0.3`` contrast envelope. OpenMWSE additionally suppresses the effect
   through covered water paths and scene shadows. Bounded local lights reuse the same animated field and projection style in
   interiors and, when configured, exteriors. Caustics work when geometric wave height is zero and perform no receiver work
   while disabled.

.. omw-setting::
   :title: caustics strength
   :type: float32
   :range: 0.0 to 2.0
   :default: 1.0
   :location: :bdg-info:`In Game > Options > Video > Water`

   Scales submerged caustic response without changing the selected appearance or geometric wave height. ``1.0`` corresponds
   to MGE XE's default caustics intensity of ``50``; ``2.0`` corresponds to ``100``.

.. omw-setting::
   :title: interior local lights
   :type: integer
   :range: 0 to 4
   :default: 2
   :location: :bdg-info:`In Game > Options > Video > Water`

   Bounds the strongest nearby local lights used for water-surface highlights and transmitted caustics in
   true interiors. Selection reuses the renderer's canonical light lists and rejects lights whose influence does not reach the
   water. Zero disables local-light water work without changing sunlight or saved caustic choices.

.. omw-setting::
   :title: exterior local lights
   :type: integer
   :range: 0 to 4
   :default: 0
   :location: :bdg-info:`In Game > Options > Video > Water`

   Bounds local-light water illumination and caustics in exteriors and quasi-exteriors. The directional sun is evaluated
   independently and does not consume this budget. Zero retains the lowest-cost exterior path.

.. omw-setting::
   :title: enhanced color
   :type: string
   :range: green-blue, blue, green
   :default: green-blue
   :location: :bdg-info:`In Game > Options > Video > Water`

   Selects the paired surface and underwater color treatment used by the enhanced appearance.

.. omw-setting::
   :title: enhanced foam
   :type: boolean
   :range: true, false
   :default: true
   :location: :bdg-info:`In Game > Options > Video > Water`

   Adds shoreline foam and weather-sensitive wave-crest whitecaps to the enhanced appearance. Fully covered surface mats that
   flatten geometric waves also suppress foam, while exposed shoreline and partially covered water retain it.

.. omw-setting::
   :title: enhanced distant waves
   :type: boolean
   :range: true, false
   :default: true
   :location: :bdg-info:`In Game > Options > Video > Water`

   Retains broad enhanced-water normal movement at long viewing distances.

.. omw-setting::
   :title: enhanced sewer ripples
   :type: boolean
   :range: true, false
   :default: false
   :location: :bdg-info:`In Game > Options > Video > Water`

   Adds disturbances around Vivec and Molag Mar sewer outlets through the bounded shared ripple simulation.

Shader provenance
*****************

The ``mge-xe`` appearance ports MGE XE water at revision ``2697fdfa7fd5af48a321a8b6cee3303a0a256690``. The ``enhanced``
appearance ports Enhanced Water Shader 2.2 by Krokantor, with credited contributions from Hrnchamd, vtastek, phal, harnlarnm,
and abot. The ``vanilla`` appearance ports Pixel Shader Style Water 1.0 by Hrnchamd. All three shaders are incorporated with
permission and attribution. The MGE-derived appearances sample the original 256 by 256 by 32 ``water_NRM.dds`` volume with its
unchanged wrap and linear-filter behavior. Their HLSL equations are translated around OpenMW's view-space depth, fog, lighting,
reflection, and refraction inputs; bounded runtime systems replace MGE XE's Direct3D-only dependencies.
Clear depth retains MGE XE's effectively infinite sentinel semantics, and displaced surfaces keep reflection lookup anchored
to the rendered screen fragment so radial geometry detail transitions cannot stretch or cut the reflection. Enhanced water
retains its near-surface water-cut transition. Disabling the existing ``Water/refraction``
setting removes scene-color refraction from every appearance while preserving depth color, shoreline attenuation, reflection,
and lighting.
