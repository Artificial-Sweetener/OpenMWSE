#ifndef OPENMW_COMPONENTS_SCENEUTIL_GEOMETRYBATCH_H
#define OPENMW_COMPONENTS_SCENEUTIL_GEOMETRYBATCH_H

#include <osg/Geometry>

#include <cstdint>
#include <vector>

namespace SceneUtil
{
    /// Stores one vertex in the normalized layout consumed by Clockwork GPU scene batches.
    struct PackedVertex
    {
        float mPosition[3];
        float mNormal[3];
        unsigned char mColor[4];
        float mUv[7][2];
        float mTexCoord7[4];
        std::uint32_t mMaterial;
        std::uint32_t mPalette;
        std::uint32_t mGroup;
    };
    static_assert(sizeof(PackedVertex) == 112);

    /// Convert supported OSG geometry into the normalized Clockwork vertex and triangle-index layout.
    bool packGeometry(
        const osg::Geometry& geometry, std::vector<std::uint8_t>& vertices, std::vector<std::uint32_t>& indices);

    /// Accumulates compatible geometry into one OSG drawable for a shared GPU submission path.
    struct GeometryBatch
    {
        osg::ref_ptr<osg::Geometry> mGeometry = new osg::Geometry;

        /// Initialize an empty batch with the normalized vertex-array layout.
        GeometryBatch();

        /// Append geometry with a shared material index and the requested front-face winding.
        bool add(const osg::Geometry& geometry, std::uint32_t material, bool clockwise);
    };
}

#endif
