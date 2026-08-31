#ifndef OPENMW_COMPONENTS_TERRAIN_DIAGNOSTICVIEWSELECTION_H
#define OPENMW_COMPONENTS_TERRAIN_DIAGNOSTICVIEWSELECTION_H

#include <string_view>

namespace osg
{
    class Camera;
}

namespace Terrain
{
    /// Return whether terrain selection diagnostics are active for the camera.
    bool diagnosticViewSelectionCollectionEnabled(const osg::Camera& camera);

    /// Describes the paged terrain selection submitted through one camera for a single frame.
    struct DiagnosticViewSelection
    {
        /// Counts every quadtree node selected for rendering.
        unsigned mSelectedNodes = 0;
        /// Counts selected nodes whose centers lie inside the active-cell grid.
        unsigned mActiveGridNodes = 0;
        /// Counts selected nodes that do not yet own renderable terrain.
        unsigned mPendingRenderingNodes = 0;
        /// Identifies the world revision used to build the selection.
        unsigned mWorldRevision = 0;
        /// Reports whether the view selection was recomputed for this frame.
        bool mSelectionUpdated = false;
        /// Reports whether recomputation changed the selected nodes.
        bool mSelectionChanged = false;
    };

    /// Stores the number of terrain nodes selected for one camera and frame.
    inline constexpr std::string_view diagnosticSelectedNodeCountStatistic = "OpenMW diagnostic terrain selected nodes";
    /// Stores the selected-node count inside the active-cell grid.
    inline constexpr std::string_view diagnosticActiveGridNodeCountStatistic
        = "OpenMW diagnostic terrain active-grid nodes";
    /// Stores the selected-node count still waiting for renderable terrain.
    inline constexpr std::string_view diagnosticPendingRenderingNodeCountStatistic
        = "OpenMW diagnostic terrain pending rendering nodes";
    /// Stores the world revision used by the terrain selection.
    inline constexpr std::string_view diagnosticViewWorldRevisionStatistic = "OpenMW diagnostic terrain world revision";
    /// Stores whether the terrain selection was recomputed for the frame.
    inline constexpr std::string_view diagnosticViewSelectionUpdatedStatistic
        = "OpenMW diagnostic terrain selection updated";
    /// Stores whether recomputation changed the selected terrain nodes.
    inline constexpr std::string_view diagnosticViewSelectionChangedStatistic
        = "OpenMW diagnostic terrain selection changed";

    /// Publish one camera's frame-synchronous paged terrain selection when diagnostic collection is active.
    void recordDiagnosticViewSelection(osg::Camera& camera, unsigned frame, const DiagnosticViewSelection& selection);
}

#endif
