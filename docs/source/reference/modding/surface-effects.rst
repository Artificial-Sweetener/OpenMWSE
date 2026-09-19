Surface effects
###############

OpenMWSE can replace selected authored mesh materials with renderer-owned surface effects. Classification happens once when an
asset is loaded. The source mesh, material name, and texture paths identify the surface; their colors and animation do not tint
or otherwise alter the selected effect.

Rule files
==========

Mods add ``.yaml`` documents below ``material-effects`` in the virtual file system. Documents use version 1 and contain named
profiles followed by classification rules::

    version: 1

    profiles:
      calm-canal:
        effect: water
        geometric-wave-height: 0

    rules:
      - profile: calm-canal
        mesh: meshes/my_mod/canal_*.nif
        material: "*water*"
        textures:
          - textures/my_mod/water/*.dds

``mesh`` and ``material`` accept case-insensitive ``*`` and ``?`` globs. ``textures`` is a list of the same globs. Fields present
in one rule are combined with AND; matching any entry in ``textures`` satisfies that field. A rule must provide at least one
selector. Later matching rules take precedence, which lets a specific mod rule refine a broad community convention. Profiles
may be declared in one document and referenced by rules in another document.

The only implemented effect is ``water``. Classified water uses the live ``mesh geometric wave height`` setting by default.
An optional profile ``geometric-wave-height`` overrides that setting in Morrowind world units; an explicit value of zero makes
that material flat. This keeps community water surfaces under one player control while allowing later area- or material-specific
water behavior.

Mesh water behavior
===================

Classified indexed geometry is separated into horizontal coplanar groups when the placed object enters the live scene. Each
unique elevation shares one exact planar-reflection camera across all matching objects. Reflection resolution follows projected
screen coverage in stable 128, 256, and 512 pixel buckets, is capped by the existing water reflection texture-size setting, and
uses hysteresis to avoid reallocations near a boundary. Invisible planes do not render their reflection. The active scene is
limited to 16 mesh-water elevations. The number of planes comes from matching material uses and their placed elevations; there
is no asset-specific camera count. Repeated uses at the same elevation share one plane.

Positive mesh-water wave heights subdivide coarse authored triangles at a bounded 128-world-unit spacing before shader
displacement, so a large legacy quad forms a wave field instead of moving as one plate. Flat mesh water retains the source
topology. Changing the live mesh-water slider rebuilds only classified water bindings to switch between those paths.

Authored-surface reflections retain terrain, statics, objects, effects, actors, and the player at the corresponding reflection
detail. The groundcover-heavy maximum tier remains exclusive to the single world-water reflection.

Only horizontal triangles are converted to mesh water. This keeps the classification format usable for future non-horizontal
effects without treating a waterfall or vertical decorative face as a planar reflection surface. Classified objects remain on
the normal live-object path instead of being merged into object-paging batches, so their effect identity and exact placed
transform remain available to the renderer.

Authored materials sometimes stack multiple animated textures over the same water polygon. Matching layers whose horizontal
footprints coincide and whose elevations differ by at most eight world units collapse to one physical water surface. All source
layers are removed, so their colors and alpha cannot cover or tint the selected OpenMWSE appearance.

OpenMWSE includes a community-water profile for the primary water layer in ``Ex_Vivec_P_water_01.nif`` and common texture
signals used by Tamriel Rebuilt water meshes and numbered Dongle-style animated-water replacements. These texture rules identify
a surface only; the active OpenMWSE water appearance supplies its rendered color and optics, the mesh-water control supplies its
geometric wave height, and the shared ripple field supplies actor and weather disturbances.
