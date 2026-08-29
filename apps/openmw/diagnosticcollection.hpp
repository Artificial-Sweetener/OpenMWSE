#ifndef OPENMW_DIAGNOSTICCOLLECTION_H
#define OPENMW_DIAGNOSTICCOLLECTION_H

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <osg/ref_ptr>

namespace osg
{
    class Stats;
}

namespace osgViewer
{
    class Viewer;
}

namespace OMW
{
    /// Identifies one completed bounded performance collection.
    struct CompletedDiagnosticCollection
    {
        std::uint64_t mRequestId = 0;
        unsigned mFirstFrame = 0;
        unsigned mLastFrame = 0;
    };

    /// Owns request-scoped statistics collection while leaving the diagnostic transport passive when idle.
    class DiagnosticCollection
    {
    public:
        /// Register diagnostic callbacks without enabling their expensive collection paths.
        explicit DiagnosticCollection(osgViewer::Viewer& viewer);

        /// Restore every statistics category enabled by an unfinished collection.
        ~DiagnosticCollection();

        DiagnosticCollection(const DiagnosticCollection&) = delete;
        DiagnosticCollection& operator=(const DiagnosticCollection&) = delete;

        /// Begin a future bounded sample interval when no collection is active.
        bool start(std::uint64_t requestId, std::uint32_t sampleCount, unsigned currentFrame);

        /// Complete a collection after delayed render statistics have reached their owners.
        std::optional<CompletedDiagnosticCollection> update(unsigned currentFrame);

        /// Return whether a performance request currently owns collection.
        bool active() const noexcept;

    private:
        struct EnabledCategory
        {
            osg::ref_ptr<osg::Stats> mStats;
            std::string mCategory;
        };

        /// Enable one category and remember only changes owned by this collection.
        void enable(osg::Stats& stats, std::string category);

        /// Discover current cameras and enable every required bounded statistic.
        void beginCollection();

        /// Restore collection state without detaching reusable callbacks.
        void endCollection();

        osgViewer::Viewer& mViewer;
        std::vector<EnabledCategory> mEnabledCategories;
        std::optional<CompletedDiagnosticCollection> mPending;
        unsigned mCompletionFrame = 0;
    };
}

#endif
