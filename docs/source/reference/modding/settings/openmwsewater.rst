OpenMWSE Water Settings
#######################

The ``OpenMWSE Water`` category owns authored water appearances and geometric-wave controls that upstream OpenMW does not
interpret. Existing ``Water`` settings remain authoritative for enabling shader water, reflection, refraction, and render-target
quality. Appearance and enhanced-water choices apply immediately and remain saved while their prerequisite is disabled.
Optical controls are shared with nearby material receivers independently from geometric displacement, so flat water can
still focus and reflect light and future water-surface providers can supply their own optical and displacement policies.

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
   equation. Zero retains OpenMW's flat, optimized water geometry path. When reflected caustics are also disabled, it does not
   allocate or render surface-coverage resources. Positive values constrain crests beneath nearby landscape and static geometry. Water becomes geometrically flat
   when its local 512-world-unit neighborhood is fully covered, keeping dense surface mats flat without suppressing waves beside
   shores, hulls, and partially covering structures. Near-coplanar coverings within four world units of the water surface are
   treated as surface mats for both wave obstruction and transparent draw ordering. A translucent mat texture must still be
   used by a mesh with an alpha property so its color composites over the reflective water instead of replacing it.

.. omw-setting::
   :title: caustics
   :type: boolean
   :range: true, false
   :default: false
   :location: :bdg-info:`In Game > Options > Video > Water`

   Enables sunlight focusing on submerged terrain, statics, foliage, and other compatibility-shader receivers. Each appearance
   derives its caustics from its own animated optical normals and, when enabled, the shared geometric-wave slope rather than a
   projected caustic texture. A depth- and pixel-footprint-aware ray differential produces stable bright focuses and darker
   troughs without screen-space sparkle. Receiver shadow visibility suppresses the effect beneath roofs and other sunlight
   blockers. It works when geometric wave height is zero and performs no wave-field evaluation while disabled.

.. omw-setting::
   :title: reflected caustics
   :type: boolean
   :range: true, false
   :default: false
   :location: :bdg-info:`In Game > Options > Video > Water`

   Enables directional light reflected by the active optical water field on nearby above-water walls, ceilings, terrain, and
   objects. The receiver-facing response and bounded ray differential derive the moving focus pattern from the selected water
   appearance rather than a projected texture. Directional cell lighting also supports interior water scenes. The contribution
   is strongest on shaded receivers and attenuates with height and distance from the water. It works when geometric wave height
   is zero. Fully covered surface mats suppress the reflected contribution, while exposed water beside river banks, walls, cave
   geometry, shores, and partially covering structures retains it. The disabled path performs no wave-field evaluation.

.. omw-setting::
   :title: caustics strength
   :type: float32
   :range: 0.0 to 2.0
   :default: 1.0
   :location: :bdg-info:`In Game > Options > Video > Water`

   Scales both submerged and reflected caustic response without changing the selected appearance or geometric wave height.

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
