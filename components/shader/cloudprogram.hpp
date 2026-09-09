#ifndef OPENMW_COMPONENTS_SHADER_CLOUDPROGRAM_H
#define OPENMW_COMPONENTS_SHADER_CLOUDPROGRAM_H

/// @file Prepare optical weather states and reflected cloud passes before their first draw.

#include <osg/Program>

namespace Shader
{
    /// Identify the imported define that removes an incoming episode with zero optical weight.
    inline constexpr char sSteadyCloudDefine[] = "OPENMWSE_CLOUD_STEADY";
    /// Separate direct sky drawing, reflected cloud transport, and its full-resolution composition.
    inline constexpr char sReflectedCloudModeDefine[] = "OPENMWSE_REFLECTED_CLOUD_MODE";

    /// Compile cloud weather states and reflection passes together; unrelated programs retain ordinary compilation.
    class CloudProgram : public osg::Program
    {
    public:
        /// Create an empty program whose shader imports determine whether cloud variants are required.
        CloudProgram() = default;

        /// Preserve shader imports and uniform-block bindings under both shallow and deep OSG copies.
        CloudProgram(const osg::Program& other, const osg::CopyOp& copy = osg::CopyOp::SHALLOW_COPY);

        /// Clone program state while leaving graphics objects owned by their OSG context.
        CloudProgram(const CloudProgram& other, const osg::CopyOp& copy = osg::CopyOp::SHALLOW_COPY);

        META_Object(Shader, CloudProgram)

        /// Prepare imported cloud weather and reflection variants while preserving the caller's define state.
        void compileGLObjects(osg::State& state) const override;
    };
}

#endif
