#include "riggeometry.hpp"

#include <osg/MatrixTransform>

#include <osgUtil/CullVisitor>

#include <algorithm>
#include <limits>

#include <components/debug/debuglog.hpp>
#include <components/misc/strings/algorithm.hpp>

#include "deformationstats.hpp"
#include "skeleton.hpp"
#include "util.hpp"

namespace SceneUtil
{

    RigGeometry::RigGeometry()
    {
        setNumChildrenRequiringUpdateTraversal(1);
        // update done in accept(NodeVisitor&)
    }

    RigGeometry::RigGeometry(const RigGeometry& copy, const osg::CopyOp& copyop)
        : Drawable(copy, copyop)
        , mData(copy.mData)
    {
        setSourceGeometry(copy.mSourceGeometry);
        setNumChildrenRequiringUpdateTraversal(1);
    }

    void RigGeometry::setSourceGeometry(osg::ref_ptr<osg::Geometry> sourceGeometry)
    {
        mGpuGeometry = nullptr;
        for (unsigned int i = 0; i < 2; ++i)
            mGeometry[i] = nullptr;
        mQueryGeometry = nullptr;
        mQueryValid = false;

        mSourceGeometry = sourceGeometry;
        mSourceTangents = dynamic_cast<const osg::Vec4Array*>(sourceGeometry->getTexCoordArray(7));
    }

    osg::ref_ptr<osg::Geometry> RigGeometry::makeGeometry() const
    {
        osg::ref_ptr<osg::Geometry> geometry = makeGpuGeometry();
        geometry->setSupportsDisplayList(false);
        geometry->setUseVertexBufferObjects(true);
        geometry->setCullingActive(false);
        osg::ref_ptr<CopyBoundingBoxCallback> box = new CopyBoundingBoxCallback;
        box->boundingBox = _boundingBox;
        geometry->setComputeBoundingBoxCallback(box);
        osg::ref_ptr<CopyBoundingSphereCallback> sphere = new CopyBoundingSphereCallback;
        sphere->boundingSphere = _boundingSphere;
        geometry->setComputeBoundingSphereCallback(sphere);
        copyArrays(*geometry);
        return geometry;
    }

    void RigGeometry::copyArrays(osg::Geometry& geometry) const
    {
        // Source arrays outlive both clones.

        // Mutable arrays use dedicated VBOs.
        osg::ref_ptr<osg::VertexBufferObject> vbo = new osg::VertexBufferObject;
        vbo->setUsage(GL_DYNAMIC_DRAW_ARB);

        osg::ref_ptr<osg::Array> vertices
            = static_cast<osg::Array*>(mSourceGeometry->getVertexArray()->clone(osg::CopyOp::DEEP_COPY_ALL));
        vertices->setVertexBufferObject(vbo);
        geometry.setVertexArray(vertices);

        if (const osg::Array* source = mSourceGeometry->getNormalArray())
        {
            osg::ref_ptr<osg::Array> normals = static_cast<osg::Array*>(source->clone(osg::CopyOp::DEEP_COPY_ALL));
            normals->setVertexBufferObject(vbo);
            geometry.setNormalArray(normals, osg::Array::BIND_PER_VERTEX);
        }
        else
            geometry.setNormalArray(nullptr);

        if (mSourceTangents != nullptr)
        {
            osg::ref_ptr<osg::Array> tangents
                = static_cast<osg::Array*>(mSourceTangents->clone(osg::CopyOp::DEEP_COPY_ALL));
            tangents->setVertexBufferObject(vbo);
            geometry.setTexCoordArray(7, tangents, osg::Array::BIND_PER_VERTEX);
        }
    }

    void RigGeometry::ensureGeometry()
    {
        if (mGeometry[0] != nullptr || mSourceGeometry == nullptr || !mCpuSkinning)
            return;
        mGeometry[0] = makeGeometry();
        mGeometry[1] = makeGeometry();
        mLastFrameNumber = 0;
    }

    void RigGeometry::setCpuSkinning(bool enabled)
    {
        mCpuSkinning = enabled;
        if (enabled || mGeometry[0] == nullptr)
            return;
        mGeometry[0] = nullptr;
        mGeometry[1] = nullptr;
        std::vector<osg::Matrixf>().swap(mCpuMatrices);
    }

    osg::ref_ptr<osg::Geometry> RigGeometry::getSourceGeometry() const
    {
        return mSourceGeometry;
    }

    bool RigGeometry::initFromParentSkeleton(osg::NodeVisitor* nv)
    {
        const osg::NodePath& path = nv->getNodePath();
        for (osg::NodePath::const_reverse_iterator it = path.rbegin() + 1; it != path.rend(); ++it)
        {
            osg::Node* node = *it;
            if (node->asTransform())
                continue;
            if (Skeleton* skel = dynamic_cast<Skeleton*>(node))
            {
                mSkeleton = skel;
                break;
            }
        }

        if (!mSkeleton)
        {
            Log(Debug::Error) << "Error: A RigGeometry did not find its parent skeleton";
            return false;
        }

        if (!mData)
        {
            Log(Debug::Error) << "Error: No influence data set on RigGeometry";
            return false;
        }

        mNodes.clear();
        for (const BoneInfo& info : mData->mBones)
        {
            mNodes.push_back(mSkeleton->getBone(info.mName));
            if (!mNodes.back())
                Log(Debug::Error) << "Error: RigGeometry did not find bone " << info.mName;
        }

        return true;
    }

    void RigGeometry::cull(osg::NodeVisitor* nv)
    {
        if (!mCpuSkinning)
            return;
        if (mOuterCull != nullptr && mOuterCull->cull(nv, this, static_cast<osg::RenderInfo*>(nullptr)))
            return;
        ensureGeometry();

        if (!mSkeleton)
        {
            Log(Debug::Error)
                << "Error: RigGeometry rendering with no skeleton, should have been initialized by UpdateVisitor";
            // try to recover anyway, though rendering is likely to be incorrect.
            if (!initFromParentSkeleton(nv))
                return;
        }

        unsigned int traversalNumber = nv->getTraversalNumber();
        if (mLastFrameNumber == traversalNumber || (mLastFrameNumber != 0 && !mSkeleton->getActive()))
        {
            osg::Geometry& geom = *getGeometry(mLastFrameNumber);
            nv->pushOntoNodePath(&geom);
            nv->apply(geom);
            nv->popFromNodePath();
            return;
        }
        mLastFrameNumber = traversalNumber;
        osg::Geometry& geom = *getGeometry(mLastFrameNumber);
        DeformationStats deformationStats(*nv);

        // skinning
        const osg::Vec3Array* positionSrc = static_cast<osg::Vec3Array*>(mSourceGeometry->getVertexArray());
        const osg::Vec3Array* normalSrc = static_cast<osg::Vec3Array*>(mSourceGeometry->getNormalArray());
        const osg::Vec4Array* tangentSrc = mSourceTangents;

        osg::Vec3Array* positionDst = static_cast<osg::Vec3Array*>(geom.getVertexArray());
        osg::Vec3Array* normalDst = static_cast<osg::Vec3Array*>(geom.getNormalArray());
        osg::Vec4Array* tangentDst = static_cast<osg::Vec4Array*>(geom.getTexCoordArray(7));

        mSkeleton->updateBoneMatrices(nv->getTraversalNumber());
        buildSkinMatrices(mCpuMatrices);
        std::size_t group = 0;
        for (const auto& influence : mData->mInfluences)
        {
            const osg::Matrixf& resultMat = mCpuMatrices[group++];

            for (unsigned short vertex : influence.second)
            {
                (*positionDst)[vertex] = resultMat.preMult((*positionSrc)[vertex]);
                if (normalDst)
                    (*normalDst)[vertex] = osg::Matrixf::transform3x3((*normalSrc)[vertex], resultMat);

                if (tangentDst)
                {
                    const osg::Vec4f& srcTangent = (*tangentSrc)[vertex];
                    osg::Vec3f transformedTangent = osg::Matrixf::transform3x3(
                        osg::Vec3f(srcTangent.x(), srcTangent.y(), srcTangent.z()), resultMat);
                    (*tangentDst)[vertex] = osg::Vec4f(transformedTangent, srcTangent.w());
                }
            }
        }

        positionDst->dirty();
        if (normalDst)
            normalDst->dirty();
        if (tangentDst)
            tangentDst->dirty();

        geom.osg::Drawable::dirtyGLObjects();

        std::size_t uploadBytes = positionDst->getTotalDataSize();
        if (normalDst)
            uploadBytes += normalDst->getTotalDataSize();
        if (tangentDst)
            uploadBytes += tangentDst->getTotalDataSize();
        deformationStats.record(DeformationPath::NativeSkinning, positionSrc->size(), uploadBytes);

        nv->pushOntoNodePath(&geom);
        nv->apply(geom);
        nv->popFromNodePath();
    }

    void RigGeometry::buildSkinMatrices(std::vector<osg::Matrixf>& out) const
    {
        std::vector<osg::Matrixf> boneMatrices(mNodes.size());
        for (std::size_t i = 0; i < boneMatrices.size(); ++i)
            if (mNodes[i] != nullptr)
                boneMatrices[i] = mData->mBones[i].mInvBindMatrix * mNodes[i]->mMatrixInSkeletonSpace;

        osg::Matrixf transform;
        if (mSkinToSkelMatrix)
            transform = (*mSkinToSkelMatrix) * mData->mTransform;
        else
            transform = mData->mTransform;
        out.clear();
        out.reserve(mData->mInfluences.size());
        for (const auto& influence : mData->mInfluences)
        {
            osg::Matrixf result(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1);
            for (const auto& [index, weight] : influence.first)
            {
                if (mNodes[index] == nullptr)
                    continue;
                const float* bone = boneMatrices[index].ptr();
                float* value = result.ptr();
                for (int i = 0; i < 16; ++i, ++value, ++bone)
                    if (i % 4 != 3)
                        *value += *bone * weight;
            }
            result *= transform;
            out.push_back(result);
        }
    }

    void RigGeometry::updateBounds(osg::NodeVisitor* nv)
    {
        if (!mSkeleton)
        {
            if (!initFromParentSkeleton(nv))
                return;
        }

        if (!mSkeleton->getActive() && !mBoundsFirstFrame)
            return;
        mBoundsFirstFrame = false;

        mSkeleton->updateBoneMatrices(nv->getTraversalNumber());

        const osg::Matrixf pathTransform = updateSkinToSkelMatrix(nv->getNodePath());
        const bool gpuSkinning = mGpuSkinning.load(std::memory_order_relaxed);
        osg::Matrixf transform;
        if (mSkinToSkelMatrix)
            transform = (*mSkinToSkelMatrix) * mData->mTransform;
        else
            transform = mData->mTransform;
        std::vector<osg::Matrixf> matrices;
        // GPU skinning works in skeleton space.
        if (gpuSkinning)
            buildSkinPalette(matrices, transform * pathTransform);

        osg::BoundingBox box;
        osg::BoundingBox skeletonBox;

        size_t index = 0;
        for (const BoneInfo& info : mData->mBones)
        {
            const Bone* bone = mNodes[index++];
            if (bone == nullptr)
                continue;

            osg::BoundingSpheref bs = info.mBoundSphere;
            transformBoundingSphere(bone->mMatrixInSkeletonSpace * transform, bs);
            box.expandBy(bs);

            if (gpuSkinning)
            {
                transformBoundingSphere(pathTransform, bs);
                skeletonBox.expandBy(bs);
            }
        }

        if (gpuSkinning)
        {
            std::lock_guard<std::mutex> lock(mGpuSkinMutex);
            GpuSkinFrame& snapshot = mGpuFrames[nv->getTraversalNumber() % mGpuFrames.size()];
            snapshot.mFrame = nv->getTraversalNumber();
            snapshot.mMatrices.swap(matrices);
            snapshot.mBounds = skeletonBox;
            snapshot.mLocalTransform = transform;
            snapshot.mValid = true;
        }

        if (box != _boundingBox)
        {
            _boundingBox = box;
            _boundingSphere = osg::BoundingSphere(_boundingBox);
            _boundingSphereComputed = true;
            for (unsigned int i = 0; i < getNumParents(); ++i)
                getParent(i)->dirtyBound();

            const auto copyBound = [&](osg::Geometry* geometry) {
                if (geometry == nullptr)
                    return;
                static_cast<CopyBoundingBoxCallback*>(geometry->getComputeBoundingBoxCallback())->boundingBox
                    = _boundingBox;
                static_cast<CopyBoundingSphereCallback*>(geometry->getComputeBoundingSphereCallback())->boundingSphere
                    = _boundingSphere;
                geometry->dirtyBound();
            };
            for (const osg::ref_ptr<osg::Geometry>& geometry : mGeometry)
            {
                copyBound(geometry);
            }
        }
    }

    osg::Matrixf RigGeometry::updateSkinToSkelMatrix(const osg::NodePath& nodePath)
    {
        if (mSkinToSkelMatrix)
            mSkinToSkelMatrix->makeIdentity();
        auto skeletonRoot = std::find(nodePath.begin(), nodePath.end(), mSkeleton);
        if (skeletonRoot == nodePath.end())
            return osg::Matrixf::identity();
        skeletonRoot++;
        auto skinRoot = nodePath.end();
        if (!mData->mRootBone.empty())
            skinRoot = std::find_if(skeletonRoot, nodePath.end(),
                [&](const osg::Node* node) { return Misc::StringUtils::ciEqual(node->getName(), mData->mRootBone); });
        if (skinRoot == nodePath.end())
        {
            // Failed to find skin root, cancel out everything up till the trishape.
            // Our parent node is the trishape's transform
            skinRoot = nodePath.end() - 2;
            if ((*skinRoot)->getName() != getName()) // but maybe it can get optimized out
                skinRoot++;
        }
        else
            skinRoot++;
        for (auto it = skeletonRoot; it != skinRoot; ++it)
        {
            const osg::Node* node = *it;
            if (const osg::Transform* trans = node->asTransform())
            {
                const osg::MatrixTransform* matrixTrans = trans->asMatrixTransform();
                if (matrixTrans && matrixTrans->getMatrix().isIdentity())
                    continue;
                if (!mSkinToSkelMatrix)
                    mSkinToSkelMatrix = new osg::RefMatrix;
                trans->computeWorldToLocalMatrix(*mSkinToSkelMatrix, nullptr);
            }
        }
        if (mCpuSkinning)
            return osg::Matrixf::identity();
        osg::Matrix path;
        path.makeIdentity();
        for (auto it = skeletonRoot; it != nodePath.end(); ++it)
            if (const osg::Transform* transform = (*it)->asTransform())
                transform->computeLocalToWorldMatrix(path, nullptr);
        return osg::Matrixf(path);
    }

    void RigGeometry::setBoneInfo(std::vector<BoneInfo>&& bones)
    {
        if (!mData)
            mData = new InfluenceData;

        mData->mBones = std::move(bones);
    }

    namespace
    {
        constexpr std::size_t sMaxInfluences = 4;

        // Four per vertex, as hardware skinning does.
        RigGeometry::BoneWeights capInfluences(RigGeometry::BoneWeights weights)
        {
            std::erase_if(weights, [](const RigGeometry::BoneWeight& weight) { return weight.second <= 0.f; });
            std::stable_sort(weights.begin(), weights.end(),
                [](const RigGeometry::BoneWeight& lhs, const RigGeometry::BoneWeight& rhs) {
                    return lhs.second > rhs.second;
                });
            if (weights.size() > sMaxInfluences)
                weights.resize(sMaxInfluences);
            float sum = 0.f;
            for (const auto& [bone, weight] : weights)
                sum += weight;
            if (sum > 0.f)
                for (auto& [bone, weight] : weights)
                    weight /= sum;
            return weights;
        }
    }

    void RigGeometry::setInfluences(const std::vector<BoneWeights>& influences)
    {
        if (!mData)
            mData = new InfluenceData;

        std::map<BoneWeights, VertexList> influencesToVertices;
        for (size_t i = 0; i < influences.size(); i++)
            influencesToVertices[capInfluences(influences[i])].emplace_back(static_cast<VertexList::value_type>(i));
        influencesToVertices.erase(BoneWeights());

        mData->mInfluences.reserve(influencesToVertices.size());
        mData->mInfluences.assign(influencesToVertices.begin(), influencesToVertices.end());
        mData->mGroups.assign(influences.size(), std::numeric_limits<std::uint32_t>::max());
        for (std::uint32_t group = 0; group < mData->mInfluences.size(); ++group)
            for (const unsigned short vertex : mData->mInfluences[group].second)
                mData->mGroups[vertex] = group;
    }

    void RigGeometry::setTransform(osg::Matrixf&& transform)
    {
        if (!mData)
            mData = new InfluenceData;
        mData->mTransform = transform;
    }

    void RigGeometry::setRootBone(std::string_view name)
    {
        if (!mData)
            mData = new InfluenceData;
        mData->mRootBone = name;
    }

    void RigGeometry::accept(osg::NodeVisitor& nv)
    {
        if (!nv.validNodeMask(*this))
            return;

        nv.pushOntoNodePath(this);

        if (nv.getVisitorType() == osg::NodeVisitor::CULL_VISITOR)
        {
            // The cull visitor won't be applied to the node itself,
            // but we want to use its state to render the child geometry.
            osg::StateSet* stateset = getStateSet();
            osgUtil::CullVisitor* cv = static_cast<osgUtil::CullVisitor*>(&nv);
            if (stateset)
                cv->pushStateSet(stateset);

            cull(&nv);
            if (stateset)
                cv->popStateSet();
        }
        else if (nv.getVisitorType() == osg::NodeVisitor::UPDATE_VISITOR)
            updateBounds(&nv);
        else
            nv.apply(*this);

        nv.popFromNodePath();
    }

    void RigGeometry::accept(osg::PrimitiveFunctor& func) const
    {
        if (acceptQuery(func))
            return;
        if (osg::Geometry* geometry = getGeometry(mLastFrameNumber))
            geometry->accept(func);
        else if (mSourceGeometry != nullptr)
            mSourceGeometry->accept(func);
    }

    osg::Geometry* RigGeometry::getGeometry(unsigned int frame) const
    {
        return mGeometry[frame % 2].get();
    }

}
