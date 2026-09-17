#version 120

varying float waterRelativeHeight;
uniform float waterWaveMaximumClearance;
uniform float waterCausticMaximumClearance;
uniform float waterWaveSurfaceTolerance;

/// Preserve nearby surface coverage without consulting material opacity or exact placement offsets.
void main()
{
    if (waterRelativeHeight < -waterWaveSurfaceTolerance || waterRelativeHeight > waterCausticMaximumClearance)
        discard;
    float waveCoverage = 1.0 - step(waterWaveMaximumClearance, waterRelativeHeight);
    float encodedHeight = (waterRelativeHeight + waterWaveSurfaceTolerance)
        / (waterCausticMaximumClearance + waterWaveSurfaceTolerance);
    gl_FragColor = vec4(waveCoverage, encodedHeight, 1.0, 1.0);
}
