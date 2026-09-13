#include "pagedlight.hpp"

#include "gpuscene.hpp"

#include <osg/Matrixf>
#include <osg/NodeVisitor>
#include <osgUtil/CullVisitor>

#include <components/esm3/loadligh.hpp>
#include <components/misc/constants.hpp>
#include <components/sceneutil/lightcommon.hpp>
#include <components/sceneutil/lightmanager.hpp>
#include <components/sceneutil/lightutil.hpp>
#include <components/sceneutil/util.hpp>
#include <components/sceneutil/visitor.hpp>

#include "vismask.hpp"

namespace MWRender
{
    namespace
    {
        /// Locate the optional model node that relocates an attached light.
        class AttachLightVisitor : public osg::NodeVisitor
        {
        public:
            /// Traverse every descendant until an attachment node is found.
            AttachLightVisitor()
                : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
            {
            }

            /// Record the local-to-world matrix for an AttachLight node.
            void apply(osg::Node& node) override
            {
                if (node.getName() == "AttachLight")
                {
                    mMatrix = osg::computeLocalToWorld(getNodePath());
                    mFound = true;
                    return;
                }
                traverse(node);
            }

            osg::Matrixf mMatrix;
            bool mFound = false;
        };
    }

    osg::Quat pagedObjectAttitude(const osg::Vec3f& rotation)
    {
        return osg::Quat(rotation.z(), osg::Vec3f(0, 0, -1)) * osg::Quat(rotation.y(), osg::Vec3f(0, -1, 0))
            * osg::Quat(rotation.x(), osg::Vec3f(-1, 0, 0));
    }

    DistantLightBatch::DistantLightBatch(const DistantLightBatch& copy, const osg::CopyOp& copyop)
        : osg::Node(copy, copyop)
        , mEntries(copy.mEntries)
        , mBound(copy.mBound)
        , mLightManager(copy.mLightManager)
        , mGpuScene(copy.mGpuScene)
    {
    }

    DistantLightBatch::DistantLightBatch(SceneUtil::LightManager* lightManager, GpuScene* gpuScene)
        : mLightManager(lightManager)
        , mGpuScene(gpuScene)
    {
    }

    void DistantLightBatch::addLight(const ESM::Light& record, ESM::RefNum refNum, const osg::Vec3f& position,
        const osg::Vec3f& rotation, float scale, const osg::Vec3f& worldCenter, const osg::Node* model)
    {
        osg::Matrixf matrix;
        matrix.preMultTranslate(position - worldCenter);
        matrix.preMultRotate(pagedObjectAttitude(rotation));
        matrix.preMultScale(osg::Vec3f(scale, scale, scale));
        if (model != nullptr)
        {
            AttachLightVisitor visitor;
            const_cast<osg::Node*>(model)->accept(visitor);
            if (visitor.mFound)
                matrix.preMult(visitor.mMatrix);
        }
        osg::ref_ptr<SceneUtil::LightSource> light
            = SceneUtil::createLightSource(SceneUtil::LightCommon(record), Mask_Lighting, true, osg::Vec4f(0, 0, 0, 1));
        light->setDistant(true);
        light->setEmpty(model == nullptr);
        light->setPagedId(SceneUtil::pagedLightId(refNum));

        osg::BoundingSphere bound = light->getBound();
        SceneUtil::transformBoundingSphere(matrix, bound);
        mEntries.push_back(Entry{ std::move(light), bound });
        mBound.expandBy(bound);
    }

    bool DistantLightBatch::empty() const
    {
        return mEntries.empty();
    }

    void DistantLightBatch::traverse(osg::NodeVisitor& visitor)
    {
        if (visitor.getVisitorType() != osg::NodeVisitor::CULL_VISITOR)
            return;
        osgUtil::CullVisitor& cv = static_cast<osgUtil::CullVisitor&>(visitor);
        if (mLightManager == nullptr || cv.getCurrentCamera()->getName() != Constants::SceneCamera
            || !mLightManager->getClusteredLighting() || mGpuScene == nullptr || !mGpuScene->isEnabled())
            return;
        const osg::Matrixf parentWorld = osg::computeLocalToWorld(cv.getNodePath());
        for (const Entry& entry : mEntries)
        {
            if (cv.isCulled(entry.mBound))
                continue;
            osg::BoundingSphere worldBound = entry.mBound;
            worldBound.radius() *= 0.5f;
            SceneUtil::transformBoundingSphere(parentWorld, worldBound);
            mLightManager->addDistantBound(entry.mLight, worldBound, cv.getTraversalNumber());
        }
    }

    osg::BoundingSphere DistantLightBatch::computeBound() const
    {
        return mBound;
    }
}
