#ifndef OPENMW_MWRENDER_PLANARREFLECTIONPROVIDER_H
#define OPENMW_MWRENDER_PLANARREFLECTIONPROVIDER_H

#include <memory>

#include <osg/Callback>
#include <osg/Referenced>
#include <osg/Texture>
#include <osg/ref_ptr>

#include <components/sceneutil/waterutil.hpp>

namespace osg
{
    class Group;
}

namespace osgUtil
{
    class CullVisitor;
}

namespace Resource
{
    class ResourceSystem;
}

namespace MWRender
{
    /// Select the independently rendered reference mode or the unified shared-capture mode.
    enum class PlanarReflectionMode
    {
        DirectReference,
        SharedCapture,
    };

    class PlanarReflectionProvider;
    class PlanarReflectionProviderImpl;
    class PlanarReflectionReceiverImpl;

    /// Retain the complete render-target state for one planar reflection receiver.
    class PlanarReflectionReceiver final : public osg::Referenced
    {
    public:
        /// Return the last frame-complete texture published for this receiver.
        osg::Texture* texture(osgUtil::CullVisitor& cv);

        /// Return the frozen prior-anchor texture sampled during an anchor handoff.
        osg::Texture* previousTexture(osgUtil::CullVisitor& cv);

        /// Return the contribution of the frozen prior-anchor texture during an anchor handoff.
        float previousTextureWeight() const;

        /// Update the receiver plane without changing its render-target allocation.
        void setPlane(const SceneUtil::WaterSurfacePlane& plane);

        /// Record projected impact and visibility for source selection and visibility-driven rendering.
        void markVisible(unsigned int frameNumber, double impact);

        /// Return whether the requested comparison path has completed a publishable frame.
        bool hasCompletedRender() const;

        /// Return the receiver's independently selected reconstruction resolution.
        unsigned int resolution() const;

        /// Return the receiver's current reflection plane.
        const SceneUtil::WaterSurfacePlane& plane() const;

    private:
        friend class PlanarReflectionProvider;
        friend class PlanarReflectionProviderImpl;

        /// Construct one provider-owned receiver with direct and shared render targets.
        PlanarReflectionReceiver(PlanarReflectionProvider& provider, const SceneUtil::WaterSurfacePlane& plane,
            unsigned int resolution, bool visibilityDriven, bool primary);

        /// Release render targets through the provider that owns their scene-graph lifetime.
        ~PlanarReflectionReceiver() override;

        std::unique_ptr<PlanarReflectionReceiverImpl> mImpl;
    };

    /// Own all planar-reflection cameras, shared capture state, and frame-complete mode handoffs.
    class PlanarReflectionProvider final : public osg::Referenced
    {
    public:
        /// Create the provider in the configured startup mode with a full-resolution shared source.
        PlanarReflectionProvider(osg::Group& parent, osg::Group& sceneRoot, Resource::ResourceSystem& resourceSystem,
            unsigned int sourceResolution, bool interior, PlanarReflectionMode initialMode);

        /// Detach every provider-owned camera before releasing renderer resources.
        ~PlanarReflectionProvider() override;

        /// Allocate one Direct Reference target and one shared reconstruction target for a water plane.
        osg::ref_ptr<PlanarReflectionReceiver> createReceiver(const SceneUtil::WaterSurfacePlane& plane,
            unsigned int resolution, bool visibilityDriven, bool primary = false);

        /// Detach a receiver and stop all rendering work associated with it.
        void releaseReceiver(PlanarReflectionReceiver& receiver);

        /// Stage the requested mode and publish it only after every visible receiver has a completed frame.
        void setMode(PlanarReflectionMode mode);

        /// Advance stable source selection, exact source ownership, and pending mode publication.
        void update(float dt);

        /// Apply interior reflection-detail constraints to every capture path.
        void setInterior(bool interior);

        /// Apply ToggleWorld filtering to every capture path.
        void showWorld(bool show);

        /// Enable or suspend all provider work without discarding completed textures.
        void setEnabled(bool enabled);

        /// Attach a renderer cull callback to every provider camera.
        void setCullCallback(osg::Callback* callback);

        /// Return the frame-complete mode currently sampled by water materials.
        PlanarReflectionMode publishedMode() const;

        /// Return whether this graphics configuration can execute the shared reconstruction shader.
        bool supportsSharedCapture() const;

    private:
        friend class PlanarReflectionReceiver;
        std::unique_ptr<PlanarReflectionProviderImpl> mImpl;
    };
}

#endif
