#ifndef OPENMW_COMPONENTS_SCENEUTIL_ATMOSPHERETERRAINGRID_H
#define OPENMW_COMPONENTS_SCENEUTIL_ATMOSPHERETERRAINGRID_H

/// @file Cache broad cloud-forming terrain on an observer-independent world lattice.

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>

#include <osg/Vec2f>
#include <osg/Vec3f>
#include <osg/Vec4f>

namespace SceneUtil
{
    /**
     * Cache fixed world samples over the complete cloud visibility and solar-shadow range.
     * Sliding the cache retains overlapping samples exactly; it does not move the terrain field.
     * Broad formation samples cover the full visible field while fitting the compatibility uniform budget.
     * Four heights share each slot in both the std140 and individual-uniform interfaces.
     */
    struct AtmosphereTerrainGrid
    {
        static constexpr int sSide = 15;
        static constexpr float sSpacingWorldUnits = 163840.f;
        static constexpr std::size_t sPackedSize = (sSide * sSide + 3) / 4;
        using Heights = std::array<osg::Vec4f, sPackedSize>;

        Heights mHeights{};
        osg::Vec4f mOriginSpacing{ -(sSide / 2) * sSpacingWorldUnits, -(sSide / 2) * sSpacingWorldUnits,
            sSpacingWorldUnits, static_cast<float>(sSide) };
        osg::Vec2f mHeightBounds;
        bool mValid = false;

        /// Fill newly exposed lattice nodes and retain identical samples while moving the cache window.
        template <class SampleHeight>
        void update(const osg::Vec2f& center, SampleHeight&& sampleHeight, bool invalidate = false)
        {
            if (!std::isfinite(center.x()) || !std::isfinite(center.y()))
                throw std::invalid_argument("Cloud terrain cache requires a finite world position");
            const osg::Vec2f origin((std::floor(center.x() / sSpacingWorldUnits) - sSide / 2) * sSpacingWorldUnits,
                (std::floor(center.y() / sSpacingWorldUnits) - sSide / 2) * sSpacingWorldUnits);
            if (mValid && !invalidate && origin.x() == mOriginSpacing.x() && origin.y() == mOriginSpacing.y())
                return;

            Heights next{};
            const bool reuse = mValid && !invalidate;
            const osg::Vec2f displacement((origin.x() - mOriginSpacing.x()) / sSpacingWorldUnits,
                (origin.y() - mOriginSpacing.y()) / sSpacingWorldUnits);
            osg::Vec2f bounds{ std::numeric_limits<float>::max(), std::numeric_limits<float>::lowest() };
            for (int y = 0; y < sSide; ++y)
                for (int x = 0; x < sSide; ++x)
                {
                    const float oldX = static_cast<float>(x) + displacement.x();
                    const float oldY = static_cast<float>(y) + displacement.y();
                    float height;
                    if (reuse && oldX >= 0.f && oldX < sSide && oldY >= 0.f && oldY < sSide)
                    {
                        const int oldIndex = static_cast<int>(oldY) * sSide + static_cast<int>(oldX);
                        height = mHeights[oldIndex / 4][oldIndex % 4];
                    }
                    else
                        height = sampleHeight(
                            osg::Vec2f(origin.x() + x * sSpacingWorldUnits, origin.y() + y * sSpacingWorldUnits));
                    if (!std::isfinite(height))
                        throw std::runtime_error("Cloud terrain cache received a non-finite terrain height");
                    const int index = y * sSide + x;
                    next[index / 4][index % 4] = height;
                    bounds.x() = std::min(bounds.x(), height);
                    bounds.y() = std::max(bounds.y(), height);
                }
            mHeights = next;
            mHeightBounds = bounds;
            mOriginSpacing = { origin.x(), origin.y(), sSpacingWorldUnits, static_cast<float>(sSide) };
            mValid = true;
        }
    };
}

#endif
