/// Share scene-uniform reflected cloud illumination between sky shader stages.
#ifndef OPENMW_SKY_CLOUD_LIGHTING_H_GLSL
#define OPENMW_SKY_CLOUD_LIGHTING_H_GLSL

#if @useGPUShader4
#extension GL_EXT_gpu_shader4: require
#endif

// Every sky vertex evaluates the same scene-linear light; legacy interpolation retains equal endpoint values.
#if @useGPUShader4
flat
#endif
varying vec3 skyLowCloudSolarIrradiance;
#if @useGPUShader4
flat
#endif
varying vec3 skyLowCloudDiffuseRadiance;

#endif
