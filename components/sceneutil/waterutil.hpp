#ifndef OPENMW_COMPONENTS_WATERUTIL_H
#define OPENMW_COMPONENTS_WATERUTIL_H

#include <array>
#include <compare>
#include <cstddef>
#include <optional>
#include <span>
#include <vector>

#include <osg/BoundingBox>
#include <osg/Matrixd>
#include <osg/PolygonOffset>
#include <osg/StateSet>
#include <osg/Vec2f>
#include <osg/Vec3f>
#include <osg/ref_ptr>

#include "depth.hpp"

namespace osg
{
    class Geometry;
    class PrimitiveSet;
    class StateSet;
}

namespace SceneUtil
{
    /// Describe an oriented planar water surface in world space with a stable local parameterization.
    struct WaterSurfacePlane
    {
        osg::Vec3f mNormal{ 0.f, 0.f, 1.f };
        float mDistance = 0.f;
        osg::Vec3f mTangent{ 1.f, 0.f, 0.f };
        osg::Vec3f mBitangent{ 0.f, 1.f, 0.f };

        /// Return the signed world-space distance from a point to the surface.
        float signedDistance(const osg::Vec3f& point) const;

        /// Project a world-space point into the surface's stable tangent frame.
        osg::Vec2f project(const osg::Vec3f& point) const;

        /// Lift one surface-local point back onto the world-space plane.
        osg::Vec3f unproject(const osg::Vec2f& point) const;
    };

    /// Identify approximately coplanar authored surfaces without reducing their identity to a world height.
    struct WaterSurfacePlaneKey
    {
        int mNormalX = 0;
        int mNormalY = 0;
        int mNormalZ = 0;
        int mDistance = 0;

        auto operator<=>(const WaterSurfacePlaneKey&) const = default;
    };

    /// Describe one unmatched triangle edge on the perimeter of a planar water component.
    struct WaterSurfaceBoundaryEdge
    {
        osg::Vec3f mStart;
        osg::Vec3f mEnd;
    };

    /// Describe one meaningful coplanar region extracted from authored triangle geometry.
    struct WaterSurfacePlanarPatch
    {
        WaterSurfacePlane mPlane;
        std::vector<std::size_t> mTriangleIndices;
    };

    /// Separate planar water regions from residual curved, degenerate, and vertical triangles.
    struct WaterSurfacePartition
    {
        std::vector<WaterSurfacePlanarPatch> mPlanarPatches;
        std::vector<std::size_t> mResidualTriangleIndices;
    };

    /// Describe the conservative normalized screen rectangle occupied by one water surface.
    struct WaterReflectionScreenRect
    {
        double mLeft = 0.0;
        double mBottom = 0.0;
        double mRight = 1.0;
        double mTop = 1.0;

        /// Return the larger pixel dimension covered in the supplied viewport.
        double projectedDiameter(double viewportWidth, double viewportHeight) const;
    };

    /// Allow authored surface coverings to sit slightly above or below the nominal water plane.
    inline constexpr float waterSurfaceInteractionTolerance = 4.f;

    /// Treat near-coplanar materials with the same footprint as visual layers of one physical surface.
    inline constexpr float waterMaterialLayerHeightTolerance = 8.f;

    /// Bound cached obstruction heights used by reflected and transmitted water-light paths.
    inline constexpr float waterCausticInteractionHeight = 1800.f;

    /// Return the conservative above-plane clearance that can interact with the configured geometric waves.
    float waterWaveInteractionHeight(float waveHeight);

    /// Select a bounded planar-reflection bucket from the surface's projected screen diameter.
    unsigned int waterReflectionResolution(double projectedDiameter, unsigned int maximumResolution);

    /// Keep a visibility-driven reflection active across the scene-to-pre-render cull ordering boundary.
    bool waterReflectionVisibleAtFrame(unsigned int lastVisibleFrame, unsigned int currentFrame) noexcept;

    /// Render a new reflection target once before applying ordinary visibility-driven culling.
    bool waterReflectionShouldRender(
        bool hasRendered, unsigned int lastVisibleFrame, unsigned int currentFrame) noexcept;

    /// Project an exact world-space surface bound into a conservative normalized screen rectangle.
    WaterReflectionScreenRect waterReflectionScreenRect(
        const osg::BoundingBox& worldBound, const osg::Matrixd& viewMatrix, const osg::Matrixd& projectionMatrix);

    /// Return whether two horizontal material layers describe the same physical surface footprint.
    bool waterSurfaceLayersCoincide(
        const osg::BoundingBox& first, float firstHeight, const osg::BoundingBox& second, float secondHeight);

    /// Fit one connected triangle component to a non-vertical plane within the supplied world-space tolerance.
    std::optional<WaterSurfacePlane> fitWaterSurfacePlane(
        std::span<const osg::Vec3f> triangleVertices, float maximumDeviation, float minimumUpwardNormal);

    /// Partition triangle geometry into wholly planar connected components and preserve every bent component.
    WaterSurfacePartition partitionWaterSurfaceTriangles(std::span<const osg::Vec3f> triangleVertices,
        float maximumDeviation, float minimumUpwardNormal, float positionTolerance);

    /// Expand one supported indexed primitive set into consistently wound triangles.
    std::vector<std::array<unsigned int, 3>> triangulateWaterSurfacePrimitives(const osg::PrimitiveSet& primitives);

    /// Return whether a triangle must reverse its winding to face the water plane's authored front side.
    bool waterSurfaceTriangleNeedsReversedWinding(
        const osg::Vec3f& a, const osg::Vec3f& b, const osg::Vec3f& c, const WaterSurfacePlane& plane);

    /// Quantize a fitted plane for deterministic sharing across separately loaded statics.
    WaterSurfacePlaneKey waterSurfacePlaneKey(const WaterSurfacePlane& plane);

    /// Extract deterministic boundary edges from a planar world-space triangle list.
    std::vector<WaterSurfaceBoundaryEdge> waterSurfaceBoundaryEdges(
        std::span<const osg::Vec3f> triangleVertices, float positionTolerance);

    /// Build two bridge triangles when separate boundary edges describe a small lateral authoring seam.
    std::optional<std::array<osg::Vec3f, 6>> waterSurfaceSeamTriangles(const WaterSurfaceBoundaryEdge& first,
        const WaterSurfaceBoundaryEdge& second, const WaterSurfacePlane& plane, float maximumGap);

    /// Return whether two fitted surfaces may share topology and reflection work.
    bool waterSurfacePlanesCompatible(const WaterSurfacePlane& first, const WaterSurfacePlane& second,
        float maximumAngularDifferenceRadians, float maximumPlaneDistance);

    /// Return whether neighboring authored surfaces can use one optical plane at their nearest horizontal point.
    bool waterSurfacePlanesSpatiallyCompatible(const WaterSurfacePlane& first, const osg::BoundingBox& firstBound,
        const WaterSurfacePlane& second, const osg::BoundingBox& secondBound, float maximumAngularDifferenceRadians,
        float maximumSurfaceDistance, float maximumHorizontalGap);

    /// Build the world-space reflection transform for one fitted water plane.
    osg::Matrixd waterSurfaceReflectionMatrix(const WaterSurfacePlane& plane);

    /// Return whether a horizontal point lies inside one water-surface triangle, including its boundary.
    bool waterSurfaceTriangleContainsPoint(
        const osg::Vec2f& point, const osg::Vec2f& a, const osg::Vec2f& b, const osg::Vec2f& c);

    /// Select a bounded uniform subdivision count for geometric waves on one world-space triangle.
    unsigned int waterTriangleSubdivisionCount(
        const osg::Vec3f& a, const osg::Vec3f& b, const osg::Vec3f& c, float maximumEdgeLength);

    /// Stabilize projected-size reflection allocations across neighboring quality buckets.
    class WaterReflectionResolutionPolicy
    {
    public:
        /// Accumulate the largest resolution requested by visible coplanar surfaces for one frame.
        void markVisible(unsigned int frameNumber, unsigned int requestedResolution);

        /// Return a stable transition after two upgrade frames or 45 downgrade frames.
        std::optional<unsigned int> consume(unsigned int currentResolution);

    private:
        unsigned int mRequestFrame = 0;
        unsigned int mConsumedRequestFrame = 0;
        unsigned int mRequestedResolution = 128;
        unsigned int mUpgradeFrames = 0;
        unsigned int mDowngradeFrames = 0;
    };

    /// Describe how a requested reflection resolution changes an in-progress target handoff.
    enum class WaterReflectionHandoffAction
    {
        None,
        Stage,
        Replace,
        Cancel,
    };

    /// Return the staging action and any superseded target that the caller must release.
    struct WaterReflectionHandoffRequest
    {
        WaterReflectionHandoffAction mAction = WaterReflectionHandoffAction::None;
        std::optional<unsigned int> mReleaseResolution;
    };

    /// Keep the last complete reflection active while a differently sized target is prepared.
    class WaterReflectionHandoffPolicy
    {
    public:
        /// Begin with one publishable reflection resolution.
        explicit WaterReflectionHandoffPolicy(unsigned int activeResolution)
            : mActiveResolution(activeResolution)
        {
        }

        /// Return the resolution currently safe for material sampling.
        unsigned int activeResolution() const { return mActiveResolution; }

        /// Return the resolution being prepared, if any.
        std::optional<unsigned int> pendingResolution() const { return mPendingResolution; }

        /// Reconcile a desired resolution without changing the active target.
        WaterReflectionHandoffRequest request(unsigned int desiredResolution);

        /// Publish the prepared resolution after its render completion and return the replaced resolution.
        std::optional<unsigned int> publish();

    private:
        unsigned int mActiveResolution;
        std::optional<unsigned int> mPendingResolution;
    };

    /// Create the existing uniformly subdivided flat water plane.
    osg::ref_ptr<osg::Geometry> createWaterGeometry(float size, int segments, float textureRepeats);

    /// Create the camera-centered radial topology used by MGE XE for geometric water displacement.
    osg::ref_ptr<osg::Geometry> createDisplacedWaterGeometry(float size, float textureRepeats);

    /// Create fixed-function-compatible simple-water render state.
    osg::ref_ptr<osg::StateSet> createSimpleWaterStateSet(float alpha, int renderBin);

    /// Keep a finite water sheet in front of a coincident basin floor while preserving ordinary depth occlusion.
    inline void applyFiniteWaterDepthSeparation(osg::StateSet& stateSet)
    {
        osg::ref_ptr<osg::PolygonOffset> offset = new osg::PolygonOffset;
        offset->setFactor(AutoDepth::isReversed() ? 0.65f : -0.65f);
        offset->setUnits(AutoDepth::isReversed() ? 1.f : -1.f);
        stateSet.setAttributeAndModes(offset, osg::StateAttribute::ON);
    }
}

#endif
