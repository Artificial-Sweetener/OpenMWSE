#include "waterawaretransparentbin.hpp"

#include <osg/BoundingBox>
#include <osg/ClipPlane>
#include <osg/Geometry>
#include <osg/Plane>
#include <osg/State>
#include <osg/StateSet>
#include <osg/Stats>
#include <osg/View>
#include <osgUtil/RenderStage>
#include <osgUtil/StateGraph>

#include <components/misc/constants.hpp>
#include <components/sceneutil/waterutil.hpp>

#include "opaqueblit.hpp"
#include "renderworkcounter.hpp"
#include "water.hpp"

namespace
{
    osg::BoundingBox worldBound(const osgUtil::RenderLeaf* leaf, const osg::Matrixd& inverseView)
    {
        const osg::Matrixd localToWorld = *leaf->_modelview * inverseView;
        const osg::BoundingBox& localBound = leaf->_drawable->getBoundingBox();

        osg::BoundingBox bound;
        for (unsigned int i = 0; i < 8; ++i)
            bound.expandBy(localBound.corner(i) * localToWorld);

        return bound;
    }
}

namespace MWRender
{
    WaterDrawSide classifyWaterDrawSide(
        double minimumHeight, double maximumHeight, double clipHeight, bool cameraUnderwater) noexcept
    {
        if (cameraUnderwater)
        {
            if (minimumHeight > clipHeight)
                return WaterDrawSide::Opposite;
            if (maximumHeight <= clipHeight)
                return WaterDrawSide::Camera;
        }
        else
        {
            if (maximumHeight < clipHeight)
                return WaterDrawSide::Opposite;
            if (minimumHeight >= clipHeight)
                return WaterDrawSide::Camera;
        }
        return WaterDrawSide::Straddling;
    }

    WaterDrawPlan makeWaterDrawPlan(bool hasMeshWater, bool hasWorldWater) noexcept
    {
        WaterDrawPlan result;
        if (!hasMeshWater && !hasWorldWater)
            return result;
        result.mStages[result.mSize++] = WaterDrawStage::ResolveScene;
        if (hasMeshWater)
            result.mStages[result.mSize++] = WaterDrawStage::MeshWater;
        if (hasWorldWater)
            result.mStages[result.mSize++] = WaterDrawStage::WorldWater;
        return result;
    }

    WaterAwareTransparentBin::WaterAwareTransparentBin(OpaqueColorBinCallback* opaqueColorResolve, const Water* water)
        : osgUtil::RenderBin(osgUtil::RenderBin::SORT_BACK_TO_FRONT)
        , mOpaqueColorResolve(opaqueColorResolve)
        , mClipStateSet(new osg::StateSet)
        , mWater(water)
    {
    }

    WaterAwareTransparentBin::WaterAwareTransparentBin(const WaterAwareTransparentBin& rhs, const osg::CopyOp& copyop)
        : osgUtil::RenderBin(rhs, copyop)
        , mOpaqueColorResolve(rhs.mOpaqueColorResolve)
        , mClipStateSet(new osg::StateSet)
        , mWater(rhs.mWater)
    {
    }

    bool WaterAwareTransparentBin::isSceneCamera() const
    {
        const osg::Camera* camera = getStage()->getCamera();

        return camera && camera->getName() == Constants::SceneCamera;
    }

    void WaterAwareTransparentBin::sortImplementation()
    {
        osgUtil::RenderBin::sortBackToFront();

        if (!isSceneCamera())
            return;

        mOppositeSideLeaves.clear();
        mWaterLeaves.clear();
        mMeshWaterLeaves.clear();
        mCameraSideLeaves.clear();
        mStraddlingLeaves.clear();

        const osg::Matrixd inverseView = osg::Matrixd::inverse(*getStage()->getInitialViewMatrix());
        const double waterHeight = mWater->getHeight();
        const osg::Vec3d cameraPosition = osg::Vec3d() * inverseView;

        mCameraUnderwater = cameraPosition.z() < waterHeight;
        const double clipHeight = waterHeight
            + (mCameraUnderwater ? SceneUtil::waterSurfaceInteractionTolerance
                                 : -SceneUtil::waterSurfaceInteractionTolerance);

        std::size_t classifiedLeafCount = 0;
        std::size_t meshWaterPrimitiveCount = 0;
        for (osgUtil::RenderLeaf* leaf : _renderLeafList)
        {
            if (!leaf || !leaf->_drawable || !leaf->_modelview)
                continue;

            ++classifiedLeafCount;
            if (mWater->isWorldWaterDrawable(leaf->_drawable))
                mWaterLeaves.push_back(leaf);
            else if (mWater->isWaterDrawable(leaf->_drawable))
            {
                mMeshWaterLeaves.push_back(leaf);
                if (const auto* geometry = leaf->_drawable->asGeometry())
                    for (unsigned int index = 0; index < geometry->getNumPrimitiveSets(); ++index)
                        meshWaterPrimitiveCount += geometry->getPrimitiveSet(index)->getNumPrimitives();
            }
            else
            {
                const osg::BoundingBox bound = worldBound(leaf, inverseView);

                switch (classifyWaterDrawSide(bound.zMin(), bound.zMax(), clipHeight, mCameraUnderwater))
                {
                    case WaterDrawSide::Opposite:
                        mOppositeSideLeaves.push_back(leaf);
                        break;
                    case WaterDrawSide::Camera:
                        mCameraSideLeaves.push_back(leaf);
                        break;
                    case WaterDrawSide::Straddling:
                        mStraddlingLeaves.push_back(leaf);
                        break;
                }
            }
        }

        osg::Camera* camera = getStage()->getCamera();
        osg::View* view = camera ? camera->getView() : nullptr;
        if (osg::Stats* stats = camera ? camera->getStats() : nullptr; view && view->getFrameStamp())
        {
            addDiagnosticRenderWorkCounter(*stats, view->getFrameStamp()->getFrameNumber(),
                transparentClassifiedLeafCounter, static_cast<double>(classifiedLeafCount));
            addDiagnosticRenderWorkCounter(*stats, view->getFrameStamp()->getFrameNumber(), meshWaterLeafCounter,
                static_cast<double>(mMeshWaterLeaves.size()));
            addDiagnosticRenderWorkCounter(*stats, view->getFrameStamp()->getFrameNumber(), meshWaterPrimitiveCounter,
                static_cast<double>(meshWaterPrimitiveCount));
        }

        if (mWaterLeaves.empty())
        {
            mCameraUnderwater = false;
            mOppositeSideLeaves.clear();
            mStraddlingLeaves.clear();
            mCameraSideLeaves.clear();
            for (osgUtil::RenderLeaf* leaf : _renderLeafList)
                if (leaf && !mWater->isWaterDrawable(leaf->_drawable))
                    mCameraSideLeaves.push_back(leaf);
        }
    }

    void WaterAwareTransparentBin::drawImplementation(osg::RenderInfo& renderInfo, osgUtil::RenderLeaf*& previous)
    {
        mLastMeshWaterDrawCount = 0;
        const bool sceneCamera = isSceneCamera();
        osgUtil::RenderBin::RenderLeafList meshWaterLeaves;
        for (osgUtil::RenderLeaf* leaf : _renderLeafList)
            if (leaf && !mWater->isWorldWaterDrawable(leaf->_drawable) && mWater->isWaterDrawable(leaf->_drawable))
                meshWaterLeaves.push_back(leaf);

        // Fallback only when this camera has no renderer-owned water surface to schedule around the opaque resolve.
        if (!sceneCamera || (mWaterLeaves.empty() && meshWaterLeaves.empty()))
        {
            osgUtil::RenderBin::drawImplementation(renderInfo, previous);
            return;
        }

        const osg::Matrixd inverseView = osg::Matrixd::inverse(*getStage()->getInitialViewMatrix());
        const double waterHeight = mWater->getHeight();

        // Does a good job at hiding blending artifacts at the water plane intersection, though it's not perfect
        const double clipHeight = waterHeight
            + (mCameraUnderwater ? SceneUtil::waterSurfaceInteractionTolerance
                                 : -SceneUtil::waterSurfaceInteractionTolerance);

        const double direction = mCameraUnderwater ? -1.0 : 1.0;

        osg::Plane nearSidePlane(0.0, 0.0, direction, -direction * clipHeight);
        osg::Plane farSidePlane(0.0, 0.0, -direction, direction * clipHeight);

        if (!mWaterLeaves.empty())
            drawLeavesWithClippedStraddlers(
                mOppositeSideLeaves, mStraddlingLeaves, renderInfo, previous, farSidePlane, inverseView, false);

        // Capture the scene once after far-side transparency and before any water draw. Both mesh and world water
        // sample this immutable pre-water result, preventing either surface from feeding back into the other's
        // refraction input.
        const WaterDrawPlan plan = makeWaterDrawPlan(!meshWaterLeaves.empty(), !mWaterLeaves.empty());
        for (std::size_t stageIndex = 0; stageIndex < plan.mSize; ++stageIndex)
        {
            switch (plan.mStages[stageIndex])
            {
                case WaterDrawStage::ResolveScene:
                    mOpaqueColorResolve->resolve(this, renderInfo);
                    break;
                case WaterDrawStage::MeshWater:
                    for (osgUtil::RenderLeaf* leaf : meshWaterLeaves)
                    {
                        leaf->render(renderInfo, previous);
                        previous = leaf;
                        ++mLastMeshWaterDrawCount;
                    }
                    break;
                case WaterDrawStage::WorldWater:
                    for (osgUtil::RenderLeaf* leaf : mWaterLeaves)
                    {
                        leaf->render(renderInfo, previous);
                        previous = leaf;
                    }
                    break;
            }
        }

        if (mWaterLeaves.empty())
        {
            for (osgUtil::RenderLeaf* leaf : mCameraSideLeaves)
            {
                leaf->render(renderInfo, previous);
                previous = leaf;
            }
            return;
        }

        drawLeavesWithClippedStraddlers(
            mCameraSideLeaves, mStraddlingLeaves, renderInfo, previous, nearSidePlane, inverseView, true);
    }

    void WaterAwareTransparentBin::drawLeavesWithClippedStraddlers(const osgUtil::RenderBin::RenderLeafList& leaves,
        const osgUtil::RenderBin::RenderLeafList& straddlingLeaves, osg::RenderInfo& renderInfo,
        osgUtil::RenderLeaf*& previous, const osg::Plane& worldPlane, const osg::Matrixd& inverseView,
        bool decrementStraddlingDynamicObjects)
    {
        if (leaves.empty() && straddlingLeaves.empty())
            return;

        osg::State& state = *renderInfo.getState();

        osg::Plane viewPlane = worldPlane;
        viewPlane.transformProvidingInverse(inverseView);
        mClipStateSet->setAttributeAndModes(new osg::ClipPlane(1, viewPlane),
            osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE | osg::StateAttribute::PROTECTED);

        if (previous)
            osgUtil::StateGraph::moveToRootStateGraph(state, previous->_parent->_parent);

        state.applyModelViewMatrix(osg::Matrixd::identity());
        state.pushStateSet(mClipStateSet);
        state.apply();

        previous = nullptr;

        std::size_t leafIndex = 0;
        std::size_t straddlingIndex = 0;

        while (leafIndex < leaves.size() || straddlingIndex < straddlingLeaves.size())
        {
            const bool drawStraddling = straddlingIndex < straddlingLeaves.size()
                && (leafIndex == leaves.size()
                    || straddlingLeaves[straddlingIndex]->_depth >= leaves[leafIndex]->_depth);

            osgUtil::RenderLeaf* leaf;
            if (drawStraddling)
                leaf = straddlingLeaves[straddlingIndex++];
            else
                leaf = leaves[leafIndex++];

            const bool dynamic = leaf->_dynamic;
            if (drawStraddling && !decrementStraddlingDynamicObjects)
                leaf->_dynamic = false;

            leaf->render(renderInfo, previous);

            leaf->_dynamic = dynamic;
            previous = leaf;
        }

        osgUtil::StateGraph::moveToRootStateGraph(state, previous->_parent->_parent);
        state.popStateSet();
        state.apply();

        osgUtil::StateGraph::moveStateGraph(state, nullptr, previous->_parent->_parent);
        state.apply(previous->_parent->getStateSet());
    }
}
