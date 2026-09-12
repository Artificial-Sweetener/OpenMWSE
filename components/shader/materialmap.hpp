#ifndef OPENMW_COMPONENTS_SHADER_MATERIALMAP_H
#define OPENMW_COMPONENTS_SHADER_MATERIALMAP_H

#include <array>
#include <cstddef>

namespace Shader
{
    // Map order shared with the shader defines.
    enum class MaterialMap
    {
        Diffuse,
        Dark,
        Detail,
        Decal,
        Emissive,
        Normal,
        Env,
        Specular,
        Bump,
        Gloss,
        Blend,
        Count
    };

    constexpr std::size_t sMaterialMapCount = static_cast<std::size_t>(MaterialMap::Count);

    // Object maps precede the terrain blend map.
    constexpr std::size_t sObjectMapCount = static_cast<std::size_t>(MaterialMap::Blend);

    // Sampler and define names in MaterialMap order.
    constexpr std::array<const char*, sMaterialMapCount> sMaterialMapNames = { "diffuseMap", "darkMap", "detailMap",
        "decalMap", "emissiveMap", "normalMap", "envMap", "specularMap", "bumpMap", "glossMap", "blendMap" };

    constexpr const char* materialMapName(MaterialMap map)
    {
        return sMaterialMapNames[static_cast<std::size_t>(map)];
    }
}

#endif
