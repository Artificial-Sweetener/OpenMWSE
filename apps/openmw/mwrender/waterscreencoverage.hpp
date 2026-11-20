#ifndef OPENMW_MWRENDER_WATERSCREENCOVERAGE_H
#define OPENMW_MWRENDER_WATERSCREENCOVERAGE_H

#include <cstdint>
#include <mutex>
#include <optional>

#include <osg/Referenced>
#include <osg/ref_ptr>

namespace osg
{
    class Drawable;
}

namespace MWRender
{
    /// Identify one completed depth-tested coverage query and the frame that produced it.
    struct WaterScreenCoverageResult
    {
        unsigned int mFrame = 0;
        std::uint64_t mSamples = 0;
    };

    /// Publish the most recent depth-tested main-view sample count for one water drawable.
    class WaterScreenCoverage final : public osg::Referenced
    {
    public:
        /// Return a completed query only while it remains representative of the current scene frame.
        std::optional<WaterScreenCoverageResult> recentResult(unsigned int currentFrame) const noexcept;

        /// Publish a completed query without allowing an older result to replace a newer frame.
        void publish(unsigned int frame, std::uint64_t samples) noexcept;

    private:
        mutable std::mutex mMutex;
        unsigned int mFrame = 0;
        std::uint64_t mSamples = 0;
        bool mAvailable = false;
    };

    /// Wrap a drawable's existing draw callback with an asynchronous main-view visibility query.
    osg::ref_ptr<WaterScreenCoverage> attachWaterScreenCoverageQuery(
        osg::Drawable& drawable, WaterScreenCoverage* sharedCoverage = nullptr);
}

#endif
