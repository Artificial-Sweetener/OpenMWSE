OpenMWSE Shadows Settings
#########################

The ``OpenMWSE Shadows`` category owns presentation controls that upstream OpenMW does not interpret. Keeping these keys
in a separate category lets the same user configuration move between OpenMWSE and upstream OpenMW without changing the
meaning of upstream-owned shadow settings. Both engines preserve unrecognized keys when saving the shared configuration.

.. omw-setting::
   :title: quality
   :type: string
   :range: legacy, low, medium, high, ultra
   :default: medium
   :location: :bdg-info:`In Game > Settings > Options > Video > Shadows` :bdg-success:`Launcher > Settings > Visuals > Shadows`

   Selects the complete shadow presentation. ``legacy`` uses OpenMW's existing one-lookup geometry receiver sampling,
   hard cascade selection, linear distance fade, unsnapped projection, and original cloud-shadow transfer. ``low`` retains
   one geometry lookup while enabling stable nested world-space projection, smooth cascade transitions, smooth distance
   falloff, stabilized alpha-tested foliage, and natural diffuse cloud shadows. ``medium`` uses four geometry samples.
   ``high`` and ``ultra`` use an edge-adaptive weighted filter that costs five lookups over uniform light or shadow and at
   most nine lookups along an edge; ``ultra`` covers a wider penumbra.

   Filtering makes low-resolution shadow maps less blocky without increasing their allocation. Higher modes increase
   receiver shader work but do not add shadow-map render passes or texture memory. Changes apply immediately.

.. omw-setting::
   :title: distant shadows
   :type: boolean
   :range: true, false
   :default: false
   :location: :bdg-info:`In Game > Settings > Options > Video > Shadows` :bdg-success:`Launcher > Settings > Visuals > Shadows`

   Preserves the configured detailed shadow distance while extending outdoor geometry shadows to the camera's visible
   horizon. Additional cascades use the selected ``distant shadow quality``, omit insignificant projected casters, and
   crossfade in camera depth. Their power-of-two world grids remain aligned with the detailed cascades.

   The renderer adds only the distant levels needed for the current viewing distance, up to eight total shadow maps.
   Increasing viewing distance can therefore increase caster traversal, GPU render work, and shadow texture memory.
   ``legacy`` ignores this setting and retains the configured OpenMW map count and distance behavior. Changes apply
   immediately.

.. omw-setting::
   :title: distant shadow quality
   :type: string
   :range: low, medium, high, ultra
   :default: medium
   :location: :bdg-info:`In Game > Settings > Options > Video > Shadows` :bdg-success:`Launcher > Settings > Visuals > Shadows`

   Controls how quickly horizon cascades expand, reduce resolution relative to ``shadow map resolution``, and discard
   casters that occupy too little of a distant shadow map to remain useful.

   ``low`` expands each distant range fourfold, uses one-quarter resolution for the first distant map and one-eighth
   thereafter, and applies aggressive projected-size culling. ``medium`` uses twofold range growth, one-half then
   one-quarter resolution, and moderate culling. ``high`` retains one-half resolution throughout the distant range and
   more small casters. ``ultra`` uses full resolution for the first distant map, one-half resolution thereafter, and the
   least projected-size culling.

   Higher qualities may require more distant maps because their ranges grow more gradually. The renderer still observes
   the eight-map total limit and always covers the visible horizon with its last available cascade. ``legacy`` and a
   disabled ``distant shadows`` setting ignore this control. Changes apply immediately.

.. omw-setting::
   :title: cloud shadow intensity
   :type: float32
   :range: [0, 1]
   :default: 1.0
   :location: :bdg-info:`In Game > Settings > Options > Video > Shadows` :bdg-success:`Launcher > Settings > Visuals > Shadows`

   Scales the optical depth of textured and volumetric cloud shadows on terrain and objects. ``0`` disables their
   lighting effect without disabling cloud rendering, while ``1`` uses the full bounded response. Textured cloud shadows
   use a wrap-aware, prefiltered form of the active weather textures so their edges remain broad and continuous across
   the repeating weather tile. Cloud shadows weaken smoothly as the sun approaches the horizon. The ``legacy`` shadow
   quality preserves OpenMW's original cloud response and ignores this control. Changes apply immediately.
