#include "lib/light/struct.glsl"

struct LightGrid {
    uint offset;
    uint count;
};

struct Cluster {
    vec4 minPoint;
    vec4 maxPoint;
};

layout(std430, binding = 1) restrict buffer clusterSSBO {
    Cluster clusters[];
};

layout(std430, binding = 2) restrict buffer pointLightSSBO {
    PointLight pointLight[];
};

layout(std430, binding = 3) restrict buffer lightGridSSBO {
    LightGrid lightGrid[];
};

layout(std430, binding = 4) restrict buffer lightIndexListSSBO {
    uint lightIndexList[];
};

layout(std430, binding = 5) restrict buffer lightIndexCounterSSBO {
    uint globalLightIndexCount;
};

uniform DirectionalLight sun;

LightGrid clusterLights(int index)
{
#if @clusteredClassic
    if (uint(index) >= uint(lightGrid.length()))
        return LightGrid(0u, 0u);
    LightGrid grid = lightGrid[index];
    // Empty receivers inherit their parent's list.
    while (grid.count == 0u && grid.offset > 0u && grid.offset < uint(index))
    {
        index = int(grid.offset);
        grid = lightGrid[index];
    }
    return grid;
#else
    return lightGrid[index];
#endif
}
