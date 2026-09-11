#include "groundcoverbatching.hpp"

#include "renderworkcounter.hpp"

#include <limits>
#include <type_traits>
#include <utility>

#include <osg/BufferObject>
#include <osg/GLExtensions>
#include <osg/Geometry>
#include <osg/PrimitiveSetIndirect>
#include <osg/Stats>
#include <osg/buffered_value>
#include <osgUtil/CullVisitor>

#include <components/sceneutil/nodecallback.hpp>

namespace
{
    template <class Primitive>
    /// Draw visible instance ranges through one indirect batch while retaining the original primitive data.
    class GroundcoverClusteredPrimitive final : public Primitive
    {
    public:
        /// Copy the source primitive and retain its geometry's shared visibility state.
        GroundcoverClusteredPrimitive(const Primitive& source, MWRender::GroundcoverBatch* batch)
            : Primitive(source, osg::CopyOp::SHALLOW_COPY)
            , mBatch(batch)
        {
        }

        /// Copy a range-aware primitive without duplicating its immutable visibility state.
        GroundcoverClusteredPrimitive(
            const GroundcoverClusteredPrimitive& source, const osg::CopyOp& copyOp = osg::CopyOp::SHALLOW_COPY)
            : Primitive(source, copyOp)
            , mBatch(source.mBatch)
        {
        }

        /// Create an empty primitive of this concrete wrapper type.
        osg::Object* cloneType() const override { return new GroundcoverClusteredPrimitive; }

        /// Clone this wrapper while retaining its exact fallback and visibility behavior.
        osg::Object* clone(const osg::CopyOp& copyOp) const override
        {
            return new GroundcoverClusteredPrimitive(*this, copyOp);
        }

        /// Identify the range-aware primitive implementation to OSG diagnostics.
        const char* className() const override { return "GroundcoverClusteredPrimitive"; }

        /// Submit only ranges accepted by the matching cull traversal, or use the exact original draw as fallback.
        void draw(osg::State& state, bool useVertexBufferObjects) const override
        {
            osg::GLExtensions* extensions = osg::GLExtensions::Get(state.getContextID(), false);
            std::vector<MWRender::GroundcoverInstanceRange> visibleRanges;
            if (!extensions || !mBatch || !mBatch->getVisibleRanges(state, visibleRanges))
            {
                Primitive::draw(state, useVertexBufferObjects);
                return;
            }

            if constexpr (std::is_same_v<Primitive, osg::DrawArrays>)
            {
                if (!extensions->glMultiDrawArraysIndirect)
                {
                    Primitive::draw(state, useVertexBufferObjects);
                    return;
                }

                CommandResources& resources = mCommands[state.getContextID()];
                if (!resources.mArrayCommands)
                {
                    resources.mArrayCommands = new osg::DefaultIndirectCommandDrawArrays;
                    resources.mArrayCommands->getBufferObject()->setUsage(GL_STREAM_DRAW);
                }
                if (resources.mRanges != visibleRanges)
                {
                    resources.mArrayCommands->clear();
                    resources.mArrayCommands->reserve(visibleRanges.size());
                    for (const MWRender::GroundcoverInstanceRange& range : visibleRanges)
                        resources.mArrayCommands->push_back(
                            { static_cast<unsigned int>(this->getCount()), static_cast<unsigned int>(range.mCount),
                                static_cast<unsigned int>(this->getFirst()), static_cast<unsigned int>(range.mFirst) });
                    resources.mArrayCommands->dirty();
                    resources.mRanges = visibleRanges;
                }
                drawArrayCommands(state, *extensions, *resources.mArrayCommands);
            }
            else
            {
                if (!extensions->glMultiDrawElementsIndirect || !useVertexBufferObjects)
                {
                    Primitive::draw(state, useVertexBufferObjects);
                    return;
                }

                osg::GLBufferObject* indexBuffer = this->getGLBufferObject(state.getContextID());
                if (!indexBuffer)
                {
                    Primitive::draw(state, useVertexBufferObjects);
                    return;
                }
                constexpr GLenum dataType = std::is_same_v<Primitive, osg::DrawElementsUByte> ? GL_UNSIGNED_BYTE
                    : std::is_same_v<Primitive, osg::DrawElementsUShort>                      ? GL_UNSIGNED_SHORT
                                                                                              : GL_UNSIGNED_INT;
                constexpr unsigned int elementSize = std::is_same_v<Primitive, osg::DrawElementsUByte> ? 1u
                    : std::is_same_v<Primitive, osg::DrawElementsUShort>                               ? 2u
                                                                                                       : 4u;
                const unsigned int firstIndex
                    = static_cast<unsigned int>(indexBuffer->getOffset(this->getBufferIndex()) / elementSize);

                CommandResources& resources = mCommands[state.getContextID()];
                if (!resources.mElementCommands)
                {
                    resources.mElementCommands = new osg::DefaultIndirectCommandDrawElements;
                    resources.mElementCommands->getBufferObject()->setUsage(GL_STREAM_DRAW);
                }
                if (resources.mRanges != visibleRanges || resources.mFirstIndex != firstIndex)
                {
                    resources.mElementCommands->clear();
                    resources.mElementCommands->reserve(visibleRanges.size());
                    for (const MWRender::GroundcoverInstanceRange& range : visibleRanges)
                        resources.mElementCommands->push_back(
                            { static_cast<unsigned int>(this->getNumIndices()), static_cast<unsigned int>(range.mCount),
                                firstIndex, 0u, static_cast<unsigned int>(range.mFirst) });
                    resources.mElementCommands->dirty();
                    resources.mRanges = visibleRanges;
                    resources.mFirstIndex = firstIndex;
                }
                drawElementCommands(state, *extensions, *resources.mElementCommands, *indexBuffer, dataType);
            }
        }

        /// Release per-context indirect command buffers alongside the copied source primitive.
        void releaseGLObjects(osg::State* state = nullptr) const override
        {
            Primitive::releaseGLObjects(state);
            if (state)
            {
                CommandResources& resources = mCommands[state->getContextID()];
                if (resources.mArrayCommands)
                    resources.mArrayCommands->releaseGLObjects(state);
                if (resources.mElementCommands)
                    resources.mElementCommands->releaseGLObjects(state);
                resources = {};
                return;
            }
            for (unsigned int context = 0; context < mCommands.size(); ++context)
            {
                CommandResources& resources = mCommands[context];
                if (resources.mArrayCommands)
                    resources.mArrayCommands->releaseGLObjects();
                if (resources.mElementCommands)
                    resources.mElementCommands->releaseGLObjects();
                resources = {};
            }
        }

    private:
        /// Retain per-context commands and the ranges from which they were generated.
        struct CommandResources
        {
            std::vector<MWRender::GroundcoverInstanceRange> mRanges;
            osg::ref_ptr<osg::DefaultIndirectCommandDrawArrays> mArrayCommands;
            osg::ref_ptr<osg::DefaultIndirectCommandDrawElements> mElementCommands;
            unsigned int mFirstIndex = 0;
        };

        /// Construct the empty value used only by OSG's clone-type protocol.
        GroundcoverClusteredPrimitive() = default;

        /// Upload and issue the current array command batch.
        void drawArrayCommands(
            osg::State& state, osg::GLExtensions& extensions, osg::DefaultIndirectCommandDrawArrays& commands) const
        {
            if (commands.empty())
                return;
            osg::GLBufferObject* commandBuffer = commands.getOrCreateGLBufferObject(state.getContextID());
            state.bindDrawIndirectBufferObject(commandBuffer);
            const GLvoid* offset = reinterpret_cast<const GLvoid*>(commandBuffer->getOffset(commands.getBufferIndex()));
            extensions.glMultiDrawArraysIndirect(this->getMode(), offset, static_cast<GLsizei>(commands.size()), 0);
        }

        /// Upload and issue the current indexed command batch.
        void drawElementCommands(osg::State& state, osg::GLExtensions& extensions,
            osg::DefaultIndirectCommandDrawElements& commands, osg::GLBufferObject& indexBuffer, GLenum dataType) const
        {
            if (commands.empty())
                return;
            osg::GLBufferObject* commandBuffer = commands.getOrCreateGLBufferObject(state.getContextID());
            state.bindDrawIndirectBufferObject(commandBuffer);
            state.getCurrentVertexArrayState()->bindElementBufferObject(&indexBuffer);
            const GLvoid* offset = reinterpret_cast<const GLvoid*>(commandBuffer->getOffset(commands.getBufferIndex()));
            extensions.glMultiDrawElementsIndirect(
                this->getMode(), dataType, offset, static_cast<GLsizei>(commands.size()), 0);
        }

        osg::ref_ptr<MWRender::GroundcoverBatch> mBatch;
        mutable osg::buffered_object<CommandResources> mCommands;
    };

    /// Return whether the primitive type supports exact base-instance range drawing.
    bool isSupportedPrimitive(const osg::PrimitiveSet& primitive)
    {
        return dynamic_cast<const osg::DrawArrays*>(&primitive)
            || dynamic_cast<const osg::DrawElementsUByte*>(&primitive)
            || dynamic_cast<const osg::DrawElementsUShort*>(&primitive)
            || dynamic_cast<const osg::DrawElementsUInt*>(&primitive);
    }

    /// Evaluate one shared groundcover visibility decision at the model root.
    class GroundcoverBatchNodeCallback final
        : public SceneUtil::NodeCallback<GroundcoverBatchNodeCallback, osg::Node*, osgUtil::CullVisitor*>
    {
    public:
        /// Retain the batch shared by every drawable below the model node.
        explicit GroundcoverBatchNodeCallback(MWRender::GroundcoverBatch* batch)
            : mBatch(batch)
        {
        }

        /// Traverse the model only when its shared visibility result contains work or requires fallback.
        void operator()(osg::Node* node, osgUtil::CullVisitor* visitor)
        {
            if (!mBatch->cull(*visitor))
                traverse(node, visitor);
        }

    private:
        osg::ref_ptr<MWRender::GroundcoverBatch> mBatch;
    };

    /// Wrap one supported primitive with the corresponding range-aware concrete type.
    osg::ref_ptr<osg::PrimitiveSet> wrapPrimitive(const osg::PrimitiveSet& primitive, MWRender::GroundcoverBatch* batch)
    {
        if (const auto* value = dynamic_cast<const osg::DrawArrays*>(&primitive))
            return new GroundcoverClusteredPrimitive<osg::DrawArrays>(*value, batch);
        if (const auto* value = dynamic_cast<const osg::DrawElementsUByte*>(&primitive))
            return new GroundcoverClusteredPrimitive<osg::DrawElementsUByte>(*value, batch);
        if (const auto* value = dynamic_cast<const osg::DrawElementsUShort*>(&primitive))
            return new GroundcoverClusteredPrimitive<osg::DrawElementsUShort>(*value, batch);
        if (const auto* value = dynamic_cast<const osg::DrawElementsUInt*>(&primitive))
            return new GroundcoverClusteredPrimitive<osg::DrawElementsUInt>(*value, batch);
        return nullptr;
    }
}

namespace MWRender
{
    GroundcoverBatch::GroundcoverBatch(std::vector<GroundcoverDrawCluster> clusters, osg::Vec4Array* transforms,
        float modelExtent, GroundcoverDistance* distance)
        : mVisibility(new GroundcoverVisibility(std::move(clusters), transforms, modelExtent, distance))
    {
    }

    bool GroundcoverBatch::configure(osg::Geometry& geometry)
    {
        ++mDrawableCount;
        bool needsArrayDrawing = false;
        bool needsElementDrawing = false;
        for (unsigned int index = 0; index < geometry.getNumPrimitiveSets(); ++index)
        {
            const osg::PrimitiveSet& primitive = *geometry.getPrimitiveSet(index);
            if (!isSupportedPrimitive(primitive))
            {
                mRangeDrawingCompatible = false;
                return false;
            }
            needsArrayDrawing = needsArrayDrawing || dynamic_cast<const osg::DrawArrays*>(&primitive);
            needsElementDrawing = needsElementDrawing || dynamic_cast<const osg::DrawElements*>(&primitive);
        }

        mNeedsArrayDrawing = mNeedsArrayDrawing || needsArrayDrawing;
        mNeedsElementDrawing = mNeedsElementDrawing || needsElementDrawing;
        for (unsigned int index = 0; index < geometry.getNumPrimitiveSets(); ++index)
            geometry.setPrimitiveSet(index, wrapPrimitive(*geometry.getPrimitiveSet(index), this));
        return true;
    }

    void GroundcoverBatch::attach(osg::Node& node)
    {
        osg::ref_ptr<osg::Callback> existingCallback = node.getCullCallback();
        osg::ref_ptr<GroundcoverBatchNodeCallback> clusterCallback = new GroundcoverBatchNodeCallback(this);
        clusterCallback->setNestedCallback(existingCallback);
        node.setCullCallback(clusterCallback);
    }

    bool GroundcoverBatch::cull(osgUtil::CullVisitor& cullVisitor) const
    {
        osg::Camera* camera = cullVisitor.getCurrentCamera();
        osg::State* state = camera && camera->getGraphicsContext() ? camera->getGraphicsContext()->getState() : nullptr;
        osg::GLExtensions* extensions = state ? osg::GLExtensions::Get(state->getContextID(), false) : nullptr;
        const bool supported = mRangeDrawingCompatible && extensions
            && (!mNeedsArrayDrawing || extensions->glMultiDrawArraysIndirect)
            && (!mNeedsElementDrawing || extensions->glMultiDrawElementsIndirect);
        const unsigned int contextId = state ? state->getContextID() : std::numeric_limits<unsigned int>::max();
        const std::size_t visibleInstances
            = supported ? mVisibility->update(cullVisitor, contextId) : mVisibility->totalInstances();

        osg::Stats* stats = camera ? camera->getStats() : nullptr;
        if (stats)
        {
            addDiagnosticRenderWorkCounter(*stats, cullVisitor.getTraversalNumber(), groundcoverInstanceRenderCounter,
                static_cast<double>(visibleInstances) * static_cast<double>(mDrawableCount));
            if (visibleInstances > 0)
            {
                addDiagnosticRenderWorkCounter(
                    *stats, cullVisitor.getTraversalNumber(), groundcoverBatchRenderCounter, 1.0);
                addDiagnosticRenderWorkCounter(*stats, cullVisitor.getTraversalNumber(),
                    groundcoverDrawableRenderCounter, static_cast<double>(mDrawableCount));
            }
        }
        return supported && visibleInstances == 0;
    }

    bool GroundcoverBatch::getVisibleRanges(
        const osg::State& state, std::vector<GroundcoverInstanceRange>& result) const
    {
        return mVisibility->getVisibleRanges(state, result);
    }
}
