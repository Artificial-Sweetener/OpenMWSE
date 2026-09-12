#ifndef OPENMW_COMPONENTS_NIFOSG_RIGGEOMETRY_H
#define OPENMW_COMPONENTS_NIFOSG_RIGGEOMETRY_H

#include <osg/Geometry>
#include <osg/Matrixf>

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string_view>
#include <vector>

namespace SceneUtil
{
    class Skeleton;
    class Bone;

    // TODO: This class has a lot of issues.
    // - We require too many workarounds to ensure safety.
    // - mSourceGeometry should be const, but can not be const because of a use case in shadervisitor.cpp.
    // - We create useless mGeometry clones in template RigGeometries.
    // - We do not support compileGLObjects.
    // - We duplicate some code in MorphGeometry.

    /// @brief Mesh skinning implementation.
    /// @note A RigGeometry may be attached directly to a Skeleton, or somewhere below a Skeleton.
    /// Note though that the RigGeometry ignores any transforms below the Skeleton, so the attachment point is not that
    /// important.
    /// @note The internal Geometry used for rendering is double buffered, this allows updates to be done in a thread
    /// safe way while not compromising rendering performance. This is crucial when using osg's default threading model
    /// of DrawThreadPerContext.
    class RigGeometry : public osg::Drawable
    {
    public:
        /// Create an unbound skinned drawable.
        RigGeometry();
        /// Copy immutable rig data while rebuilding per-instance render state on demand.
        RigGeometry(const RigGeometry& copy, const osg::CopyOp& copyop);

        META_Object(SceneUtil, RigGeometry)

        // Currently empty as this is difficult to implement. Technically we would need to compile both internal
        // geometries in separate frames but this method is only called once. Alternatively we could compile just the
        // static parts of the model.
        void compileGLObjects(osg::RenderInfo& renderInfo) const override {}

        /// Describe one source skeleton bone and its bind-space influence bound.
        struct BoneInfo
        {
            std::string mName;
            osg::BoundingSpheref mBoundSphere;
            osg::Matrixf mInvBindMatrix;
        };

        using BoneWeight = std::pair<size_t, float>;
        using BoneWeights = std::vector<BoneWeight>;

        /// Locate one vertex's contiguous weights in a GPU skin-weight buffer.
        struct SkinRange
        {
            std::uint32_t mFirst;
            std::uint32_t mCount;
        };

        /// Associate a source bone with its normalized influence weight.
        struct SkinWeight
        {
            std::uint32_t mBone;
            float mWeight;
        };

        /// Replace the immutable bone metadata used by CPU and GPU skinning.
        void setBoneInfo(std::vector<BoneInfo>&& bones);
        /// Preserve source influences for CPU skinning and derive bounded GPU influence groups.
        void setInfluences(const std::vector<BoneWeights>& influences);

        /// Initialize this geometry from the source geometry.
        /// @note The source geometry will not be modified.
        void setSourceGeometry(osg::ref_ptr<osg::Geometry> sourceGeom);

        /// Set the source geometry-to-skin transform.
        void setTransform(osg::Matrixf&& transform);

        /// Select the optional skeleton node that anchors the skinning transform.
        void setRootBone(std::string_view name);

        /// Return the immutable source geometry retained for rendering and queries.
        osg::ref_ptr<osg::Geometry> getSourceGeometry() const;
        /// Return the shallow GPU geometry view, creating it on first use.
        osg::Geometry* getGpuGeometry() const;
        /// Newest bone palette captured by cull.
        bool getLatestPalette(unsigned int& frame, std::vector<osg::Matrixf>& out, osg::Matrixf* local = nullptr) const;
        /// Palettes and these bounds use skeleton space.
        bool getLatestBound(unsigned int& frame, osg::BoundingBox& out) const;
        /// Copy the palette captured for an exact traversal frame.
        bool getPalette(unsigned int frame, std::vector<osg::Matrixf>& out) const;
        /// Copy the skeleton-space bound captured for an exact traversal frame.
        bool getBound(unsigned int frame, osg::BoundingBox& out) const;
        /// Return the compact per-vertex influence group identifiers.
        const std::vector<std::uint32_t>& getSkinGroups() const;
        /// Flatten source influences into GPU ranges and weights and return the bone count.
        std::size_t getSkinData(std::vector<SkinRange>& ranges, std::vector<SkinWeight>& weights) const;
        /// Enable or disable palette capture for GPU skinning and reconstructed queries.
        void setGpuSkinning(bool enabled) { mGpuSkinning.store(enabled, std::memory_order_relaxed); }
        /// Enable CPU-skinned render copies, releasing them immediately when disabled.
        void setCpuSkinning(bool enabled);
        /// Set an optional outer cull policy for CPU-skinned geometry.
        void setOuterCull(osg::DrawableCullCallback* callback) { mOuterCull = callback; }
        /// Return the optional outer cull policy.
        osg::DrawableCullCallback* getOuterCull() { return mOuterCull; }

        void accept(osg::NodeVisitor& nv) override;
        bool supports(const osg::PrimitiveFunctor&) const override { return true; }
        void accept(osg::PrimitiveFunctor&) const override;

        /// Publish the owner-computed bounding box to an internal render copy.
        struct CopyBoundingBoxCallback : osg::Drawable::ComputeBoundingBoxCallback
        {
            osg::BoundingBox boundingBox;

            osg::BoundingBox computeBound(const osg::Drawable&) const override { return boundingBox; }
        };

        /// Publish the owner-computed bounding sphere to an internal render copy.
        struct CopyBoundingSphereCallback : osg::Node::ComputeBoundingSphereCallback
        {
            osg::BoundingSphere boundingSphere;

            osg::BoundingSphere computeBound(const osg::Node&) const override { return boundingSphere; }
        };

    private:
        void cull(osg::NodeVisitor* nv);
        void updateBounds(osg::NodeVisitor* nv);
        osg::ref_ptr<osg::Geometry> makeGeometry() const;
        osg::ref_ptr<osg::Geometry> makeGpuGeometry() const;
        void copyArrays(osg::Geometry& geometry) const;
        void ensureGeometry();

        mutable osg::ref_ptr<osg::Geometry> mGpuGeometry;
        osg::ref_ptr<osg::Geometry> mGeometry[2];
        osg::Geometry* getGeometry(unsigned int frame) const;
        bool acceptQuery(osg::PrimitiveFunctor& func) const;
        osg::ref_ptr<osg::DrawableCullCallback> mOuterCull;

        osg::ref_ptr<osg::Geometry> mSourceGeometry;
        osg::ref_ptr<const osg::Vec4Array> mSourceTangents;
        Skeleton* mSkeleton{ nullptr };

        osg::ref_ptr<osg::RefMatrix> mSkinToSkelMatrix;
        std::vector<osg::Matrixf> mCpuMatrices;
        mutable std::mutex mGpuSkinMutex;
        struct GpuSkinFrame
        {
            unsigned int mFrame{ 0 };
            std::vector<osg::Matrixf> mMatrices;
            /// Skin transform in the geometry frame.
            osg::Matrixf mLocalTransform;
            osg::BoundingBox mBounds;
            bool mValid{ false };
        };
        std::array<GpuSkinFrame, 3> mGpuFrames;
        std::atomic<bool> mGpuSkinning{ false };
        bool mCpuSkinning{ true };
        mutable std::mutex mQueryMutex;
        mutable osg::ref_ptr<osg::Geometry> mQueryGeometry;
        mutable unsigned int mQueryFrame{ 0 };
        mutable bool mQueryValid{ false };

        using VertexList = std::vector<unsigned short>;
        struct InfluenceData : public osg::Referenced
        {
            std::vector<BoneInfo> mBones;
            std::vector<std::pair<BoneWeights, VertexList>> mInfluences;
            std::vector<std::pair<BoneWeights, VertexList>> mGpuInfluences;
            std::vector<std::uint32_t> mGroups;
            osg::Matrixf mTransform;
            std::string mRootBone;
        };
        osg::ref_ptr<InfluenceData> mData;
        std::vector<Bone*> mNodes;

        unsigned int mLastFrameNumber{ 0 };
        bool mBoundsFirstFrame{ true };

        bool initFromParentSkeleton(osg::NodeVisitor* nv);
        void buildSkinMatrices(std::vector<osg::Matrixf>& out) const;
        void buildSkinPalette(std::vector<osg::Matrixf>& out, const osg::Matrixf& transform) const;

        osg::Matrixf updateSkinToSkelMatrix(const osg::NodePath& nodePath);
    };

}

#endif
