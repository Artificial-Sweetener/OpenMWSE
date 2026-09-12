#ifndef OPENMW_COMPONENTS_SCENEUTIL_TEXTUREDCLOUDS_HPP
#define OPENMW_COMPONENTS_SCENEUTIL_TEXTUREDCLOUDS_HPP

#include <array>

#include <osg/Image>
#include <osg/Node>
#include <osg/StateSet>
#include <osg/Texture2D>
#include <osg/Vec2f>
#include <osg/Vec4f>

namespace SceneUtil
{
    /// Share authored and prefiltered weather opacity, dome mapping, and animation with lighting shaders.
    struct TexturedCloudState
    {
        osg::ref_ptr<osg::Texture2D> mOpacity;
        osg::ref_ptr<osg::Texture2D> mDomeMapping;
        // Inverse storm rotation (cosine, sine), followed by the authored UV animation offset.
        std::array<osg::Vec4f, 2> mTransforms{ osg::Vec4f(1, 0, 0, 0), osg::Vec4f(1, 0, 0, 0) };
        osg::Vec2f mWeights{ 1.f, 0.f };
        bool mEnabled = false;
    };

    /// Cache immutable authored and prefiltered opacity channels; weather transitions do not rebuild them every frame.
    class TexturedClouds
    {
    public:
        /// Derive an immutable directional UV/fade lookup from the loaded, alpha-adjusted cloud dome.
        void setDome(osg::Node& dome);

        /// Return the camera-relative lookup retained by frame-local rendering state.
        osg::Texture2D* getDomeMapping() const { return mDomeMapping.get(); }

        /// Build bounded periodic opacity channels from the outgoing and incoming weather assets.
        void setImages(osg::Image* outgoing, osg::Image* incoming);

        /// Return the opacity texture retained by frame-local rendering state.
        osg::Texture2D* getOpacity() const { return mOpacity.get(); }

    private:
        std::array<osg::ref_ptr<osg::Image>, 2> mImages;
        osg::ref_ptr<osg::Texture2D> mOpacity;
        osg::ref_ptr<osg::Texture2D> mDomeMapping;
    };

    /// Bind identical projection uniforms and opacity to a frame-owned state set in either renderer.
    void applyTexturedCloudState(
        osg::StateSet& stateset, const TexturedCloudState& clouds, int textureUnit, int domeTextureUnit);
}

#endif
