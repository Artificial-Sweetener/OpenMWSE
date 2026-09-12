#include "riggeometry.hpp"

/// @file Own retained GPU skinning resources, frame snapshots, and CPU query reconstruction.

#include <components/resource/scenemanager.hpp>

#include "skeleton.hpp"

namespace SceneUtil
{
    osg::ref_ptr<osg::Geometry> RigGeometry::makeGpuGeometry() const
    {
        osg::ref_ptr<osg::Geometry> geometry = new osg::Geometry(*mSourceGeometry, osg::CopyOp::SHALLOW_COPY);
        geometry->getOrCreateUserDataContainer()->addUserObject(new Resource::TemplateRef(mSourceGeometry));
        return geometry;
    }

    osg::Geometry* RigGeometry::getGpuGeometry() const
    {
        if (mGpuGeometry == nullptr && mSourceGeometry != nullptr)
            mGpuGeometry = makeGpuGeometry();
        return mGpuGeometry;
    }

    const std::vector<std::uint32_t>& RigGeometry::getSkinGroups() const
    {
        return mData->mGroups;
    }

    std::size_t RigGeometry::getSkinData(std::vector<SkinRange>& ranges, std::vector<SkinWeight>& weights) const
    {
        ranges.clear();
        weights.clear();
        ranges.reserve(mData->mInfluences.size());
        for (const auto& influence : mData->mInfluences)
        {
            ranges.push_back(
                { static_cast<std::uint32_t>(weights.size()), static_cast<std::uint32_t>(influence.first.size()) });
            for (const auto& [bone, weight] : influence.first)
                weights.push_back({ static_cast<std::uint32_t>(bone), weight });
        }
        return mData->mBones.size();
    }

    bool RigGeometry::getLatestPalette(
        unsigned int& frame, std::vector<osg::Matrixf>& out, osg::Matrixf* local) const
    {
        std::lock_guard<std::mutex> lock(mGpuSkinMutex);
        const GpuSkinFrame* latest = nullptr;
        for (const GpuSkinFrame& snapshot : mGpuFrames)
            if (snapshot.mValid && (latest == nullptr || latest->mFrame < snapshot.mFrame))
                latest = &snapshot;
        if (latest == nullptr)
            return false;
        frame = latest->mFrame;
        out = latest->mMatrices;
        if (local != nullptr)
            *local = latest->mLocalTransform;
        return true;
    }

    bool RigGeometry::getLatestBound(unsigned int& frame, osg::BoundingBox& out) const
    {
        std::lock_guard<std::mutex> lock(mGpuSkinMutex);
        const GpuSkinFrame* latest = nullptr;
        for (const GpuSkinFrame& snapshot : mGpuFrames)
            if (snapshot.mValid && (latest == nullptr || latest->mFrame < snapshot.mFrame))
                latest = &snapshot;
        if (latest == nullptr || !latest->mBounds.valid())
            return false;
        frame = latest->mFrame;
        out = latest->mBounds;
        return true;
    }

    bool RigGeometry::getPalette(unsigned int frame, std::vector<osg::Matrixf>& out) const
    {
        std::lock_guard<std::mutex> lock(mGpuSkinMutex);
        for (const GpuSkinFrame& snapshot : mGpuFrames)
        {
            if (!snapshot.mValid || snapshot.mFrame != frame)
                continue;
            out = snapshot.mMatrices;
            return true;
        }
        return false;
    }

    bool RigGeometry::getBound(unsigned int frame, osg::BoundingBox& out) const
    {
        std::lock_guard<std::mutex> lock(mGpuSkinMutex);
        for (const GpuSkinFrame& snapshot : mGpuFrames)
        {
            if (!snapshot.mValid || snapshot.mFrame != frame || !snapshot.mBounds.valid())
                continue;
            out = snapshot.mBounds;
            return true;
        }
        return false;
    }

    void RigGeometry::buildSkinPalette(std::vector<osg::Matrixf>& out, const osg::Matrixf& transform) const
    {
        out.clear();
        out.reserve(mNodes.size() + 1);
        out.push_back(transform);
        osg::Matrixf zero(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
        for (std::size_t i = 0; i < mNodes.size(); ++i)
        {
            if (mNodes[i] != nullptr)
                out.push_back(mData->mBones[i].mInvBindMatrix * mNodes[i]->mMatrixInSkeletonSpace);
            else
                out.push_back(zero);
        }
    }

    bool RigGeometry::acceptQuery(osg::PrimitiveFunctor& func) const
    {
        if (!mGpuSkinning.load(std::memory_order_relaxed) || mData == nullptr || mSourceGeometry == nullptr)
            return false;

        unsigned int frame = 0;
        std::vector<osg::Matrixf> palette;
        osg::Matrixf local;
        if (!getLatestPalette(frame, palette, &local) || palette.size() != mNodes.size() + 1)
            return false;

        std::lock_guard<std::mutex> lock(mQueryMutex);
        if (mQueryGeometry == nullptr)
        {
            mQueryGeometry = new osg::Geometry(*mSourceGeometry, osg::CopyOp::SHALLOW_COPY);
            osg::ref_ptr<osg::Array> positions
                = static_cast<osg::Array*>(mSourceGeometry->getVertexArray()->clone(osg::CopyOp::DEEP_COPY_ALL));
            positions->setVertexBufferObject(new osg::VertexBufferObject);
            mQueryGeometry->setVertexArray(positions);
        }

        if (!mQueryValid || mQueryFrame != frame)
        {
            const auto* source = static_cast<const osg::Vec3Array*>(mSourceGeometry->getVertexArray());
            auto* target = static_cast<osg::Vec3Array*>(mQueryGeometry->getVertexArray());
            for (const auto& influence : mData->mInfluences)
            {
                osg::Matrixf result(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1);
                for (const auto& [bone, weight] : influence.first)
                {
                    if (bone + 1 >= palette.size())
                        return false;
                    const float* matrix = palette[bone + 1].ptr();
                    float* value = result.ptr();
                    for (int i = 0; i < 16; ++i, ++value, ++matrix)
                        if (i % 4 != 3)
                            *value += *matrix * weight;
                }
                result *= local;
                for (const unsigned short vertex : influence.second)
                {
                    if (vertex >= source->size() || vertex >= target->size())
                        return false;
                    (*target)[vertex] = result.preMult((*source)[vertex]);
                }
            }
            mQueryFrame = frame;
            mQueryValid = true;
        }

        mQueryGeometry->accept(func);
        return true;
    }
}
