/// Evaluate uniform local-medium illumination at fullscreen vertices while preserving the fragment sky equations.
#if OMW_MULTIVIEW
layout(num_views = 2) in;
#endif
#if OMW_USE_BINDINGS
omw_In vec2 omw_Vertex;
#endif
omw_Out vec2 omw_TexCoord;
#if __VERSION__ >= 130 || defined(GL_EXT_gpu_shader4)
flat
#endif
omw_Out vec3 atmosphereLowSolarIrradiance;
#if __VERSION__ >= 130 || defined(GL_EXT_gpu_shader4)
flat
#endif
omw_Out vec3 atmosphereLowDiffuseRadiance;
#if __VERSION__ >= 130 || defined(GL_EXT_gpu_shader4)
flat
#endif
omw_Out vec3 atmosphereBelowCloudDiffuse;

/// Supply scene-linear illumination at the low-cloud altitude and below the clear-air recovery transition.
void main()
{
    omw_Position = vec4(omw_Vertex.xy, 0.0, 1.0);
    omw_TexCoord = omw_Position.xy * 0.5 + 0.5;
    OpenMWSEAerialPerspectiveParameters atmosphere = aerialPerspectiveParameters();
    vec4 lowParameters = openmwseLowCloudParameters();
    OpenMWSEVolumeIllumination lowLighting = openmwseLocalVolumeIllumination(omw.atmosphereSolarRadiance,
        omw.atmosphereGroundIrradiance,
        omw.waterHeight + lowParameters.z + lowParameters.w * 0.5, atmosphere);
    // Recovery starts at most 1800 world units below the cloud top; this altitude is wholly below it.
    OpenMWSEVolumeIllumination belowLighting = openmwseLocalVolumeIllumination(omw.atmosphereSolarRadiance,
        omw.atmosphereGroundIrradiance, atmosphere.weatherCloudTopHeight - 9000.0, atmosphere);
    atmosphereLowSolarIrradiance = lowLighting.solarIrradiance;
    atmosphereLowDiffuseRadiance = lowLighting.diffuseRadiance;
    atmosphereBelowCloudDiffuse = belowLighting.diffuseRadiance;
}
