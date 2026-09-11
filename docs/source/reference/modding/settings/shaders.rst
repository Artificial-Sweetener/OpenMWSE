Shaders Settings
################

.. omw-setting::
   :title: atmospheric scattering
   :type: boolean
   :range: true, false
   :default: true
   :location: :bdg-info:`In Game > Settings > Video > Atmosphere`

   Enables regional sky scattering, aerial perspective, and mist in exteriors.
   Mist and volumetric fog follow the same stable world-space terrain field as environmental clouds.
   Applies immediately. Ordinary distance fog remains when disabled. Volumetric clouds are controlled independently.

.. omw-setting::
   :title: atmosphere appearance
   :type: int
   :range: 0-1
   :default: 0
   :location: :bdg-info:`In Game > Settings > Video > Atmosphere`

   The Atmosphere appearance dropdown selects OpenMWSE (0) or MGE XE (1). OpenMWSE provides the colorful twilight response;
   MGE XE uses the original regional weather palettes, sky and in-scattering equation, and distance-fog transfer.
   Region changes blend over 20 seconds; regions without an assignment use the default palette. OpenMWSE uses
   environmental palette blending and height-dependent aerial perspective. Changing atmosphere appearance preserves the
   active cloud episode and its formation. Requires atmospheric scattering. Applies immediately.

.. omw-setting::
   :title: godrays
   :type: boolean
   :range: true, false
   :default: true
   :location: :bdg-info:`In Game > Settings > Video > Atmosphere`

   Adds MGE XE-style sunlight shafts formed by the scene depth mask. Volumetric clouds also attenuate the source
   through their resolved transmittance. Rays extend across the scene and fade as the sun moves off screen.
   Weather controls direct sunlight strength. This control is independent
   of atmospheric scattering, cloud sun occlusion, and the sun glare texture. Applies immediately.

.. omw-setting::
   :title: sun glare
   :type: boolean
   :range: true, false
   :default: false
   :location: :bdg-info:`In Game > Settings > Video > Atmosphere`

   Displays the mod-resolved sun glare texture. Clouds and scenery fade its visibility; landscape depth clips its
   footprint. Post-processing effects with the ``disable_sunglare`` flag suppress this texture. Applies immediately.

.. omw-setting::
   :title: cloud sun occlusion
   :type: boolean
   :range: true, false
   :default: true
   :location: :bdg-info:`In Game > Settings > Video > Atmosphere`

   Attenuates the solar disc through cloud optical depth. Textured skies also receive localized sunlight along thin
   cloud edges and a small angular bloom around those edges. The cloud response remains in the sky and applies
   immediately. Godrays, surface cloud shadows, and the glare texture have independent controls.

.. omw-setting::
   :title: volumetric clouds
   :type: boolean
   :range: true, false
   :default: true
   :location: :bdg-info:`In Game > Settings > Video > Atmosphere`

   Renders volumetric sky and low clouds using the active weather episode. Disabling this setting selects traditional
   textured sky clouds with their authored dome placement, texture mapping, and horizon fade. Solar occlusion
   follows that dome. Changes apply immediately and preserve the weather episode and cloud animation state.

.. omw-setting::
   :title: atmospheric quality
   :type: int
   :range: 0-3
   :default: 2
   :location: :bdg-info:`In Game > Settings > Video > Atmosphere`

   The Volumetric quality dropdown selects Low (0), Medium (1), High (2), or Ultra (3). It controls cloud and mist volume
   resolution and cloud ray samples. Changes rebuild the affected targets and reset temporal
   history immediately. Weather selection, cloud morphology parameters, and animation are preserved.

Existing configurations using ``atmospheric effects`` initialize both scattering and volumetric clouds from that value.
Explicit independent settings in the same configuration file take precedence.
These preferences live in the user ``settings.cfg``. OpenMW preserves unrecognized user settings when saving,
so the atmosphere preferences survive switching between OpenMWSE and upstream OpenMW.
Each installation uses its own matching executables and ``defaults.bin``.

.. omw-setting::
   :title: force per pixel lighting
   :type: boolean
   :range: true, false
   :default: false
   :location: :bdg-info:`In Game > Settings > Options > Video > Lights`

   Force the use of per-pixel lighting.
   By default, only bump- and normal-mapped objects use per-pixel lighting.
   Enabling per-pixel lighting results in visual differences to the original engine
   as certain lights in Morrowind rely on vertex lighting to look as intended.
   Note that groundcover shaders and particle effects ignore this setting.

.. omw-setting::
   :title: particle point lighting
   :type: boolean
   :range: true, false
   :default: true
   :location: :bdg-info:`In Game > Settings > Options > Video > Lights`

   Allows particle systems to be lit by point lights. When disabled, particle systems will only be lit by the sun.
   This feature is enabled by default in Morrowind, but disabling it can increase performance in particle dense scenes.

.. omw-setting::
   :title: clamp lighting
   :type: boolean
   :range: true, false
   :default: true

   Cap lighting brightness at (1, 1, 1) to replicate Morrowind's rendering.
   Prevents overly bright or shifted colors but can dull lighting.

.. omw-setting::
   :title: auto use object normal maps
   :type: boolean
   :range: true, false
   :default: false
   :location: :bdg-success:`Launcher > Settings > Visuals > Shaders`

   Automatically detect and use object normal maps named with pattern defined by :ref:`normal map pattern`.
   Otherwise normal maps must be explicitly listed in mesh files.

.. omw-setting::
   :title: auto use object specular maps
   :type: boolean
   :range: true, false
   :default: false
   :location: :bdg-success:`Launcher > Settings > Visuals > Shaders`

   Automatically detect and use object specular maps named with pattern defined by :ref:`specular map pattern`.
   Only supported in `.osg` files.

.. omw-setting::
   :title: auto use terrain normal maps
   :type: boolean
   :range: true, false
   :default: false
   :location: :bdg-success:`Launcher > Settings > Visuals > Shaders`

   Same as :ref:`auto use object normal maps`, but applies to terrain.

.. omw-setting::
   :title: auto use terrain specular maps
   :type: boolean
   :range: true, false
   :default: false
   :location: :bdg-success:`Launcher > Settings > Visuals > Shaders`

   Use terrain specular maps if matching :ref:`terrain specular map pattern` texture exists.
   Texture RGB is layer color, alpha is specular multiplier.

.. omw-setting::
   :title: normal map pattern
   :type: string
   :default: _n

   Filename pattern used to detect normal maps automatically.

.. omw-setting::
   :title: normal height map pattern
   :type: string
   :default: _nh

   Alternative pattern for normal maps containing height in alpha channel for parallax effects.

.. omw-setting::
   :title: specular map pattern
   :type: string
   :default: _spec

   Filename pattern to detect object specular maps.

.. omw-setting::
   :title: terrain specular map pattern
   :type: string
   :default: _diffusespec

   Filename pattern to detect terrain specular maps.

.. omw-setting::
   :title: apply lighting to environment maps
   :type: boolean
   :range: true, false
   :default: false
   :location: :bdg-success:`Launcher > Settings > Visuals > Shaders`

   Enable lighting effects on environment map reflections to prevent glowing in dark areas.

.. omw-setting::
   :title: clustered lighting
   :type: boolean
   :range: true, false
   :default: false
   :location: :bdg-info:`In Game > Settings > Options > Video > Lights` :bdg-success:`Launcher > Settings > Visuals > Lighting`

    Divides the screen into regions to assign lights, removing per-object light limits.

    Classic falloff and max light options do not apply in this mode.

    Enables point light specular highlights on the water plane when the water shader is enabled.

   .. note::

      It is highly recommended to use this with per-pixel lighting enabled as vertex lighting can cause light pop at screen edges.

.. omw-setting::
   :title: light radius multiplier
   :type: float32
   :range: 1.0-100.0
   :default: 1.75
   :location: :bdg-info:`In Game > Settings > Options > Video > Lights`

   Multiplier for point light radius. Larger values will increase the range of lights.

.. omw-setting::
   :title: classic falloff
   :type: boolean
   :range: true, false
   :default: false
   :location: :bdg-info:`In Game > Settings > Options > Video > Lights`

   Use traditional point light attenuation without early fade out.
   Reduces lighting seams but may darken the scene.

   .. note::

      This setting is only applicable when clustered lighting is disabled

.. omw-setting::
   :title: match sunlight to sun
   :type: boolean
   :range: true, false
   :default: false
   :location: :bdg-info:`In Game > Settings > Options > Video > Lights`

   Aligns the sun light source direction with the visible sun position for realism.

.. omw-setting::
   :title: maximum light distance
   :type: float32
   :range: full float range
   :default: 8192
   :location: :bdg-info:`In Game > Settings > Options > Video > Lights`

   Maximum distance at which lights illuminate objects.
   Set to ≤ 0 to disable fading for lights.

.. omw-setting::
   :title: light fade start
   :type: float32
   :range: 0.0-1.0
   :default: 0.85
   :location: :bdg-info:`In Game > Settings > Options > Video > Lights`

   Fraction of max distance where light fading begins.

.. omw-setting::
   :title: max lights
   :type: int
   :range: 2-64
   :default: 16
   :location: :bdg-info:`In Game > Settings > Options > Video > Lights`

   Maximum lights affecting each object.

   .. note::

      This setting is only applicable when clustered lighting is disabled

.. omw-setting::
   :title: minimum interior brightness
   :type: float32
   :range: 0.0-1.0
   :default: 0.08
   :location: :bdg-info:`In Game > Settings > Options > Video > Lights`

   Minimum ambient brightness inside interiors.
   Should be small to avoid unwanted visual changes.

.. omw-setting::
   :title: antialias alpha test
   :type: boolean
   :range: true, false
   :default: false
   :location: :bdg-success:`Launcher > Settings > Visuals > Shaders`

   Converts alpha testing to alpha-to-coverage for smoother edges with MSAA enabled.

.. omw-setting::
   :title: adjust coverage for alpha test
   :type: boolean
   :range: true, false
   :default: true
   :location: :bdg-success:`Launcher > Settings > Visuals > Shaders`

   Mitigates shrinking artifacts on alpha-tested textures without coverage-preserving mipmaps.

.. omw-setting::
   :title: soft particles
   :type: boolean
   :range: true, false
   :default: false
   :location: :bdg-success:`Launcher > Settings > Visuals > Shaders`

   Enables soft particles effect for smoother particle intersections.

.. omw-setting::
   :title: weather particle occlusion
   :type: boolean
   :range: true, false
   :default: false
   :location: :bdg-success:`Launcher > Settings > Visuals > Shaders`

   Prevents rain and snow clipping through ceilings by using an extra render pass.

   .. warning::

      Experimental and may cause visual oddities.
