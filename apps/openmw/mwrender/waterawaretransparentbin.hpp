#ifndef OPENMW_MWRENDER_WATERAWARETRANSPARENTBIN_H
#define OPENMW_MWRENDER_WATERAWARETRANSPARENTBIN_H

#include <osgUtil/RenderBin>

namespace osg
{
    class StateSet;
}

namespace MWRender
{
    class OpaqueColorBinCallback;
    class Water;

    /// Identify which side of the water draw a transparent bound belongs to for the current camera.
    enum class WaterDrawSide
    {
        Opposite,
        Camera,
        Straddling,
    };

    /// Classify a transparent bound against the same offset plane used while drawing it.
    WaterDrawSide classifyWaterDrawSide(
        double minimumHeight, double maximumHeight, double clipHeight, bool cameraUnderwater) noexcept;

    class WaterAwareTransparentBin : public osgUtil::RenderBin
    {
    public:
        WaterAwareTransparentBin(OpaqueColorBinCallback* opaqueColorResolve, const Water* water);
        WaterAwareTransparentBin(const WaterAwareTransparentBin& rhs, const osg::CopyOp& copyop);

        osg::Object* cloneType() const override { return new WaterAwareTransparentBin(mOpaqueColorResolve, mWater); }
        osg::Object* clone(const osg::CopyOp& copyop) const override
        {
            return new WaterAwareTransparentBin(*this, copyop);
        }

        void sortImplementation() override;
        void drawImplementation(osg::RenderInfo& renderInfo, osgUtil::RenderLeaf*& previous) override;

        /// Return the mesh-water leaves submitted by the most recent primary draw on this bin.
        std::size_t lastMeshWaterDrawCount() const { return mLastMeshWaterDrawCount; }

    private:
        void drawLeavesWithClippedStraddlers(const osgUtil::RenderBin::RenderLeafList& leaves,
            const osgUtil::RenderBin::RenderLeafList& straddlingLeaves, osg::RenderInfo& renderInfo,
            osgUtil::RenderLeaf*& previous, const osg::Plane& worldPlane, const osg::Matrixd& inverseView,
            bool decrementStraddlingDynamicObjects);

        bool isSceneCamera() const;

        osg::ref_ptr<OpaqueColorBinCallback> mOpaqueColorResolve;
        osgUtil::RenderBin::RenderLeafList mOppositeSideLeaves;
        osgUtil::RenderBin::RenderLeafList mWaterLeaves;
        osgUtil::RenderBin::RenderLeafList mMeshWaterLeaves;
        osgUtil::RenderBin::RenderLeafList mCameraSideLeaves;
        osgUtil::RenderBin::RenderLeafList mStraddlingLeaves;
        osg::ref_ptr<osg::StateSet> mClipStateSet;
        bool mCameraUnderwater = false;
        std::size_t mLastMeshWaterDrawCount = 0;
        const Water* mWater;
    };
}

#endif
