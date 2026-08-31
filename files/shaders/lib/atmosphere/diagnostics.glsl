#ifndef OPENMWSE_ATMOSPHERE_DIAGNOSTICS_GLSL
#define OPENMWSE_ATMOSPHERE_DIAGNOSTICS_GLSL

const int openmwseAtmosphereDiagnosticNormal = 0;
const int openmwseAtmosphereDiagnosticCoverageBypassed = 1;
const int openmwseAtmosphereDiagnosticFogBypassed = 2;
const int openmwseAtmosphereDiagnosticFogVisibility = 3;
const int openmwseAtmosphereDiagnosticFogInScattering = 4;
const int openmwseAtmosphereDiagnosticPreFogRadiance = 5;
const int openmwseAtmosphereDiagnosticSampledSurfaceColor = 6;
const int openmwseAtmosphereDiagnosticCloudSurfaceShadowsBypassed = 7;

uniform int openmwseAtmosphereDiagnosticMode;

#endif
