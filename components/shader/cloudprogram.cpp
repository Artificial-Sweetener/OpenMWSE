#include "cloudprogram.hpp"

#include <osg/State>

#include <utility>

namespace Shader
{
    CloudProgram::CloudProgram(const osg::Program& other, const osg::CopyOp& copy)
        : osg::Program(other, copy)
    {
        for (const auto& [name, binding] : other.getUniformBlockBindingList())
            addBindUniformBlock(name, binding);
        if ((copy.getCopyFlags() & osg::CopyOp::DEEP_COPY_STATEATTRIBUTES) != 0)
        {
            // OSG's deep program copy also copies shaders without retaining their parsed import metadata.
            for (unsigned int index = 0; index < getNumShaders(); ++index)
                getShader(index)->setShaderSource(other.getShader(index)->getShaderSource());
        }
    }

    CloudProgram::CloudProgram(const CloudProgram& other, const osg::CopyOp& copy)
        : CloudProgram(static_cast<const osg::Program&>(other), copy)
    {
    }

    void CloudProgram::compileGLObjects(osg::State& state) const
    {
        osg::Program::compileGLObjects(state);
        if (getShaderDefines().count(sSteadyCloudDefine) == 0)
            return;

        /// Restore both stacked and directly applied defines after compiling alternate programs.
        struct RestoreDefines
        {
            osg::State::DefineMap& target;
            osg::State::DefineMap saved;

            /// Restore the exact caller state even if shader compilation throws.
            ~RestoreDefines() { target = std::move(saved); }
        };
        auto& defines = state.getDefineMap();
        RestoreDefines restore{ defines, defines };
        static_cast<void>(state.getDefineString(getShaderDefines()));
        // State::apply can publish defines without pushing a StateSet; changing the effective map preserves that case.
        for (const char* value : { "0", "1" })
        {
            defines.currentDefines[sSteadyCloudDefine] = { value, osg::StateAttribute::ON };
            osg::Program::compileGLObjects(state);
        }
    }
}
