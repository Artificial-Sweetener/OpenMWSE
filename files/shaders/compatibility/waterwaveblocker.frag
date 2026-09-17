#version 120

varying float waterRelativeHeight;
uniform float waterWaveMaximumClearance;
uniform float waterWaveSurfaceTolerance;

/// Preserve nearby surface coverage without consulting material opacity or exact placement offsets.
void main()
{
    if (waterRelativeHeight < -waterWaveSurfaceTolerance || waterRelativeHeight > waterWaveMaximumClearance)
        discard;
    gl_FragColor = vec4(1.0);
}
