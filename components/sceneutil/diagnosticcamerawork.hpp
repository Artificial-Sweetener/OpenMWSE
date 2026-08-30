#ifndef OPENMW_COMPONENTS_SCENEUTIL_DIAGNOSTICCAMERAWORK_H
#define OPENMW_COMPONENTS_SCENEUTIL_DIAGNOSTICCAMERAWORK_H

#include <memory>
#include <string_view>

namespace osg
{
    class Camera;
    class RenderInfo;
    class State;
}

namespace SceneUtil
{
    inline constexpr std::string_view diagnosticCpuPassThreadCycleStatistic
        = "OpenMW diagnostic CPU pass thread cycles";
    inline constexpr std::string_view diagnosticCpuPassSetupTimeStatistic = "OpenMW diagnostic CPU pass setup time";
    inline constexpr std::string_view diagnosticCpuPassSetupThreadCycleStatistic
        = "OpenMW diagnostic CPU pass setup thread cycles";
    inline constexpr std::string_view diagnosticCpuPassDrawTimeStatistic = "OpenMW diagnostic CPU pass draw time";
    inline constexpr std::string_view diagnosticCpuPassDrawThreadCycleStatistic
        = "OpenMW diagnostic CPU pass draw thread cycles";
    inline constexpr std::string_view diagnosticCpuPassCompletionTimeStatistic
        = "OpenMW diagnostic CPU pass completion time";
    inline constexpr std::string_view diagnosticCpuPassCompletionThreadCycleStatistic
        = "OpenMW diagnostic CPU pass completion thread cycles";

    /// Records elapsed time and active-thread cycles across one camera's draw phases.
    class DiagnosticCameraWork final
    {
    public:
        /// Associate phase samples with one instrumented camera.
        explicit DiagnosticCameraWork(osg::Camera& camera);

        /// Release the private per-context sample state.
        ~DiagnosticCameraWork();

        /// Begin one camera work sample for the current graphics context.
        void begin(osg::RenderInfo& renderInfo);

        /// Mark completion of camera setup immediately before its draw body.
        void beforeDraw(osg::RenderInfo& renderInfo);

        /// Mark completion of the camera draw body before completion work.
        void afterDraw(osg::RenderInfo& renderInfo);

        /// Complete one camera work sample and publish its phase distributions.
        void end(osg::RenderInfo& renderInfo);

        /// Release samples belonging to one graphics context, or every context when null.
        void release(osg::State* state);

        DiagnosticCameraWork(const DiagnosticCameraWork&) = delete;
        DiagnosticCameraWork& operator=(const DiagnosticCameraWork&) = delete;

    private:
        struct Impl;
        std::unique_ptr<Impl> mImpl;
    };
}

#endif
