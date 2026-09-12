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
   one geometry lookup while enabling stable projection, smooth cascade transitions, smooth distance falloff, and natural
   diffuse cloud shadows. ``medium`` uses four geometry samples, ``high`` uses nine weighted samples, and ``ultra`` uses
   sixteen weighted samples.

   Filtering makes low-resolution shadow maps less blocky without increasing their allocation. Higher modes increase
   receiver shader work but do not add shadow-map render passes or texture memory. Changes apply immediately.

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
