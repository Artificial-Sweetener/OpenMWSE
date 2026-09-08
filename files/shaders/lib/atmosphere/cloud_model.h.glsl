#ifndef OPENMWSE_CLOUD_MODEL_H_GLSL
#define OPENMWSE_CLOUD_MODEL_H_GLSL

/// Carry the independent modelling controls of one volumetric cloud layer.
struct CloudModelLayer
{
    float physicalScale;
    vec3 albedo;
    vec3 extinctionPerMeter;
    float skewAlongWindDirection;
    float totalNoiseScale;
    float curlScale;
    float curlNoiseHeightFraction;
    float curlNoiseModifier;
    float detailScale;
    float detailNoiseHeightFraction;
    float detailNoiseModifier;
    float skewAlongCoverageWindDirection;
    float weatherScale;
    float coverageAmount;
    float coverageMinimum;
    float typeAmount;
    float typeMinimum;
    float rainAmount;
    float rainMinimum;
    vec4 gradientSmall;
    vec4 gradientMedium;
    vec4 gradientLarge;
    vec4 anvilDeformationSmall;
    vec4 anvilDeformationMedium;
    vec4 anvilDeformationLarge;
    vec3 windDirection;
    vec3 windOffset;
    vec2 coverageWindDirection;
    vec2 coverageWindOffset;
};

/// Decode SceneUtil's fourteen-vec4 publication and integrate each layer's independent source wind.
CloudModelLayer cloudModelDecodeLayer(vec4 parameters[14], float animationSeconds);
/// Select the continuous small, medium, and large type weights from weather-map G.
vec3 cloudModelWeatherTypeMask(vec3 weatherData);
/// Blend the layer's authored height support using the weather-map type.
vec4 cloudModelHeightGradient(vec3 weatherData, CloudModelLayer layer);
/// Evaluate the black-white-white-black height gradient before coverage erosion.
float cloudModelSampleHeightGradient(float heightFraction, vec3 weatherData, CloudModelLayer layer);
/// Apply the source's independent top and bottom coverage deformations within the typed gradient.
float cloudModelSampleAnvilDeformation(float heightFraction, vec3 weatherData, CloudModelLayer layer);
/// Keep the weather-map domain independent from shape advection and shape-noise frequency.
vec2 cloudModelWeatherCoordinates(vec3 positionMeters, float heightFraction, CloudModelLayer layer);
/// Apply independent amount/minimum remaps to coverage, type, and precipitation.
vec3 cloudModelRemapWeather(vec3 weatherData, CloudModelLayer layer);
/// Reject empty weather and unsupported heights before sampling either three-dimensional noise volume.
bool cloudModelValidDensity(float heightFraction, vec3 weatherData, CloudModelLayer layer);
/// Warp the stationary shape domain before adding the independent shape-wind displacement.
vec3 cloudModelShapeCoordinates(vec3 positionMeters, float heightFraction, CloudModelLayer layer);
/// Keep curl fixed in the unadvected horizontal noise domain.
vec2 cloudModelCurlCoordinates(vec3 positionMeters, CloudModelLayer layer);
/// Displace the independently advected detail domain with the decoded curl vector.
vec3 cloudModelDetailCoordinates(vec3 positionMeters, float heightFraction, vec3 encodedCurl,
    CloudModelLayer layer);
/// Reconstruct Perlin-Worley mass, then apply the source's height, coverage, and anvil operations in order.
float cloudModelShapeDensity(vec4 shapeNoise, float heightFraction, vec3 weatherData, CloudModelLayer layer);
/// Erode every positive shape sample using the layer's full height-dependent Worley detail response.
float cloudModelErodeDensity(float density, vec3 detailNoise, float heightFraction, CloudModelLayer layer);

/// Evaluate per-layer beer-powder albedo and independent precipitation absorption.
vec3 cloudModelSampleAlbedo(float density, float precipitation, CloudModelLayer layer);

#endif
