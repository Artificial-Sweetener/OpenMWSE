#ifndef OPENMW_COMPONENTS_SCENEUTIL_CLOUDLAYER_H
#define OPENMW_COMPONENTS_SCENEUTIL_CLOUDLAYER_H

/// @file Own cloud-layer parameters and their shared graphics interface in metres.

#include <array>

#include <osg/Vec3f>
#include <osg/Vec4f>

namespace SceneUtil
{
    /// Describe one density and optical layer independently of placement and weather-episode identity.
    struct CloudLayer
    {
        /// Scale source sampling distances independently of authored noise-frequency controls.
        float mPhysicalScale = 1.f;
        osg::Vec3f mAlbedo{ .9f, .9f, .9f };
        osg::Vec3f mExtinctionPerMeter{ .071f, .086f, .1f };
        float mShapeSkewMeters = 700.f;
        float mNoiseScalePerMeter = .0006f;
        float mCurlScale = .3f;
        float mCurlHeightExponent = 5.f;
        float mCurlDisplacementMeters = 500.f;
        float mDetailScale = 4.f;
        float mDetailHeightFraction = 10.f;
        float mDetailErosion = .3f;
        float mCoverageSkewMeters = 2500.f;
        float mWeatherScalePerMeter = .00002f;
        float mCoverageAmount = 1.f;
        float mCoverageMinimum = 0.f;
        float mTypeAmount = 1.f;
        float mTypeMinimum = 0.f;
        float mRainAmount = 0.f;
        float mRainMinimum = 0.f;
        osg::Vec4f mGradientSmall{ .01f, .1f, .11f, .2f };
        osg::Vec4f mGradientMedium{ .01f, .08f, .3f, .4f };
        osg::Vec4f mGradientLarge{ .01f, .06f, .75f, .95f };
        osg::Vec4f mAnvilSmall{ 0.f, 0.f, 0.f, 0.f };
        osg::Vec4f mAnvilMedium{ 15.f, .1f, 15.f, .1f };
        osg::Vec4f mAnvilLarge{ 5.f, .25f, 5.f, .15f };
        float mWindSpeedMetersPerSecond = 15.f;
        float mWindAngleRadians = .75f;
        float mWindUpAmount = .5f;
        float mCoverageWindSpeedMetersPerSecond = 30.f;
        float mCoverageWindAngleRadians = 0.f;
    };

    /// Return the terrain cloud preset.
    inline CloudLayer fairWeatherCloudLayer()
    {
        // Source: wiTerrain.cpp::Terrain, same revision and MIT provenance as the primary layer.
        CloudLayer layer;
        layer.mExtinctionPerMeter = { .71f * .05f, .86f * .05f, .05f };
        layer.mNoiseScalePerMeter = .0009f;
        layer.mCurlScale = .15f;
        layer.mDetailErosion = .5f;
        layer.mWeatherScalePerMeter = .000035f;
        layer.mCoverageMinimum = .25f;
        layer.mTypeAmount = 0.f;
        layer.mRainAmount = .9f;
        layer.mGradientSmall = { .01f, .07f, .08f, .14f };
        layer.mWindSpeedMetersPerSecond = 20.f;
        layer.mCoverageWindSpeedMetersPerSecond = 35.f;
        return layer;
    }

    /// Return the secondary layer, whose elevated gradients and reversed erosion form thin high clouds.
    inline CloudLayer secondaryCloudLayer()
    {
        CloudLayer layer;
        layer.mExtinctionPerMeter = { .0071f, .0086f, .01f };
        layer.mShapeSkewMeters = 400.f;
        layer.mCurlScale = .1f;
        layer.mCurlHeightExponent = 500.f;
        layer.mCurlDisplacementMeters = 250.f;
        layer.mDetailScale = 2.f;
        layer.mDetailHeightFraction = 0.f;
        layer.mDetailErosion = 1.f;
        layer.mCoverageSkewMeters = 0.f;
        layer.mWeatherScalePerMeter = .000025f;
        layer.mCoverageAmount = 0.f;
        layer.mGradientSmall = { .6f, .62f, .63f, .65f };
        layer.mGradientMedium = { .6f, .64f, .66f, .7f };
        layer.mGradientLarge = { .6f, .66f, .69f, .75f };
        layer.mAnvilMedium = {};
        layer.mAnvilLarge = {};
        layer.mWindSpeedMetersPerSecond = 10.f;
        layer.mWindAngleRadians = 1.f;
        layer.mWindUpAmount = .1f;
        layer.mCoverageWindSpeedMetersPerSecond = 50.f;
        layer.mCoverageWindAngleRadians = 1.f;
        return layer;
    }

    /// Pack source parameters into vec4-aligned storage shared by compatibility shaders and the postprocess UBO.
    inline std::array<osg::Vec4f, 14> packCloudLayer(const CloudLayer& layer)
    {
        return { osg::Vec4f(layer.mAlbedo, 0.f), osg::Vec4f(layer.mExtinctionPerMeter, 0.f),
            osg::Vec4f(layer.mShapeSkewMeters, layer.mNoiseScalePerMeter, layer.mCurlScale, layer.mCurlHeightExponent),
            osg::Vec4f(
                layer.mCurlDisplacementMeters, layer.mDetailScale, layer.mDetailHeightFraction, layer.mDetailErosion),
            osg::Vec4f(
                layer.mCoverageSkewMeters, layer.mWeatherScalePerMeter, layer.mCoverageAmount, layer.mCoverageMinimum),
            osg::Vec4f(layer.mTypeAmount, layer.mTypeMinimum, layer.mRainAmount, layer.mRainMinimum),
            layer.mGradientSmall, layer.mGradientMedium, layer.mGradientLarge, layer.mAnvilSmall, layer.mAnvilMedium,
            layer.mAnvilLarge,
            osg::Vec4f(layer.mWindSpeedMetersPerSecond, layer.mWindAngleRadians, layer.mWindUpAmount,
                layer.mCoverageWindSpeedMetersPerSecond),
            osg::Vec4f(layer.mCoverageWindAngleRadians, layer.mPhysicalScale, 0.f, 0.f) };
    }

    /// Hold outgoing, incoming, and environmental density models in one graphics publication.
    using CloudLayerUniforms = std::array<osg::Vec4f, 42>;

    /// Publish each episode's complete parameters without interpolating its density model with another episode.
    inline CloudLayerUniforms packCloudLayers(
        const CloudLayer& outgoing, const CloudLayer& incoming, const CloudLayer& environmental)
    {
        CloudLayerUniforms result;
        const std::array layers{ packCloudLayer(outgoing), packCloudLayer(incoming), packCloudLayer(environmental) };
        for (std::size_t layer = 0; layer < layers.size(); ++layer)
            for (std::size_t field = 0; field < layers[layer].size(); ++field)
                result[layer * layers[layer].size() + field] = layers[layer][field];
        return result;
    }
}

#endif
