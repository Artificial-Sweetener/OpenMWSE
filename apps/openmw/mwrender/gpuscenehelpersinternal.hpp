#ifndef OPENMW_MWRENDER_GPUSCENEHELPERSINTERNAL_H
#define OPENMW_MWRENDER_GPUSCENEHELPERSINTERNAL_H

namespace MWRender
{
    /// Runs after draws on the context thread.
    class GpuScene::RetireOperation : public osg::GraphicsOperation
    {
    public:
        explicit RetireOperation(GpuScene& scene)
            : osg::GraphicsOperation("GpuSceneRetire", true)
            , mScene(&scene)
        {
        }

        void operator()(osg::GraphicsContext* context) override
        {
            if (GpuScene* const scene = mScene.load(std::memory_order_acquire))
                scene->finishFrame(*context->getState());
        }

        void detach()
        {
            mScene.store(nullptr, std::memory_order_release);
            setKeep(false);
        }

    private:
        std::atomic<GpuScene*> mScene;
    };

    namespace
    {
        using GpuVertex = SceneUtil::PackedVertex;
        using SceneUtil::packGeometry;

        constexpr const char* sMaterialUniforms[] = { "material.diffuse", "material.ambient", "material.specular",
            "material.emission", "material.shininess", "material.emissiveMult", "material.specStrength",
            "material.vertexColorMode", "alphaRef", "envMapColor", "envMapLumaBias", "bumpMapMatrix", "texMat0",
            "texMat1", "texMat2", "texMat3", "texMat4", "texMat5", "texMat6", "texMat7", "texMat8", "texMat9" };
        constexpr const char* sTexMatNames[]
            = { "texMat0", "texMat1", "texMat2", "texMat3", "texMat4", "texMat5", "texMat6", "texMat7" };
        constexpr GLint sMaxStorageBinding = 23;
        constexpr GLint sVertexStorageBlocks = 6;
        constexpr GLint sFragmentStorageBlocks = 4;

        struct GpuCommand
        {
            std::uint32_t mCount;
            std::uint32_t mInstanceCount;
            std::uint32_t mFirstIndex;
            std::int32_t mBaseVertex;
            std::uint32_t mBaseInstance;
        };
        // Mirrors GL draw-indirect commands.
        static_assert(sizeof(GpuCommand) == 20);
        static_assert(offsetof(GpuCommand, mBaseInstance) == 16);

        constexpr unsigned int sFlagSmallFeature = 1;
        constexpr unsigned int sFlagLod = 2;
        // Bits encode paged night-day modes.
        constexpr unsigned int sFlagModeShift = 2;
        constexpr unsigned int sModeCount = 3;
        constexpr unsigned int sFlagTerrainExtra = 1 << 5;
        constexpr unsigned int sFlagOpaqueShadow = 1 << 6;
        constexpr unsigned int sFlagGroundcoverLod = 1 << 7;
        constexpr unsigned int sWorkGroupSize = 256;
        /// Projected size that seeds the depth prepass.
        constexpr float sOccluderPixelSize = 256.f;
        constexpr unsigned int sSkinGroupSize = 64;
        constexpr unsigned int sMaxPlanes = 16;
        /// One chunk mask bit per view.
        constexpr std::size_t sMaxGroupViews = 32;

        struct CullPath
        {
            const GpuScene* mScene = nullptr;
            unsigned int mFrame = 0;
            const osg::Camera* mCamera = nullptr;
            std::vector<const osg::StateSet*> mStates;
        };

        thread_local CullPath sCullPath;

        using ClearBufferDataFn = void(GL_APIENTRY*)(GLenum, GLenum, GLenum, GLenum, const void*);
        using CopyBufferSubDataFn = void(GL_APIENTRY*)(GLenum, GLenum, GLintptr, GLintptr, GLsizeiptr);

        inline bool prepassBucket(const osg::StateSet& state)
        {
            return state.getAttribute(osg::StateAttribute::ALPHAFUNC) == nullptr
                && (state.getMode(GL_BLEND) & osg::StateAttribute::ON) == 0 && state.getBinNumber() <= 0
                && state.getRenderingHint() < osg::StateSet::TRANSPARENT_BIN;
        }

        inline bool orderDependentPath(const std::vector<const osg::StateSet*>& statePath)
        {
            bool ordered = false;
            bool blended = false;
            for (const osg::StateSet* state : statePath)
            {
                if (state->useRenderBinDetails())
                {
                    const std::string& name = state->getBinName();
                    ordered = state->getBinNumber() == RenderBin_DepthSorted || name == "DepthSortedBin"
                        || name == "SORT_BACK_TO_FRONT" || name == "TraversalOrderBin";
                }
                const osg::StateAttribute::GLModeValue blend = state->getMode(GL_BLEND);
                if (blend != osg::StateAttribute::INHERIT)
                    blended = (blend & osg::StateAttribute::ON) != 0;
            }
            return ordered || blended;
        }

        inline osg::ref_ptr<osg::StateSet> makePrepassState(const osg::StateSet& source)
        {
            osg::ref_ptr<osg::StateSet> result = new osg::StateSet;
            for (const auto& [mode, value] : source.getModeList())
                result->setMode(mode, value);
            for (const auto& [typeMember, attribute] : source.getAttributeList())
            {
                const osg::StateAttribute::Type type = typeMember.first;
                if (type == osg::StateAttribute::PROGRAM || type == osg::StateAttribute::MATERIAL
                    || type == osg::StateAttribute::COLORMASK || type == osg::StateAttribute::POLYGONMODE)
                    continue;
                result->setAttribute(const_cast<osg::StateAttribute*>(attribute.first.get()), attribute.second);
            }
            return result;
        }

        template <class Pair, class Find>
        const Pair* resolvePair(const std::vector<const osg::StateSet*>& path, Find&& find)
        {
            const Pair* result = nullptr;
            for (const osg::StateSet* stateSet : path)
            {
                const Pair* incoming = find(*stateSet);
                if (incoming == nullptr)
                    continue;
                if (result == nullptr || !(result->second & osg::StateAttribute::OVERRIDE)
                    || (incoming->second & osg::StateAttribute::PROTECTED))
                    result = incoming;
            }
            return result;
        }

        template <class T>
        T uniformValue(const osg::StateSet::RefUniformPair* pair, const T& fallback)
        {
            T value = fallback;
            if (pair != nullptr)
                pair->first->get(value);
            return value;
        }

        inline void put(float* out, const osg::Vec4f& v)
        {
            for (int i = 0; i < 4; ++i)
                out[i] = v[i];
        }

        bool hasUpdater(const osg::Callback* callback)
        {
            for (; callback != nullptr; callback = callback->getNestedCallback())
                if (dynamic_cast<const SceneUtil::StateSetUpdater*>(callback) != nullptr
                    || dynamic_cast<const NifOsg::VisController*>(callback) != nullptr)
                    return true;
            return false;
        }

        using LODRange = osg::LOD::MinMaxPair;

        inline LODRange intersection(const LODRange& left, const LODRange& right)
        {
            return { std::max(left.first, right.first), std::min(left.second, right.second) };
        }

        inline bool empty(const LODRange& r)
        {
            return r.first >= r.second;
        }

        float uniformScale(const osg::Matrixf& matrix)
        {
            const float x = osg::Matrixf::transform3x3(osg::Vec3f(1, 0, 0), matrix).length();
            const float y = osg::Matrixf::transform3x3(osg::Vec3f(0, 1, 0), matrix).length();
            const float z = osg::Matrixf::transform3x3(osg::Vec3f(0, 0, 1), matrix).length();
            const float maxScale = std::max({ x, y, z });
            const float minScale = std::min({ x, y, z });
            if (maxScale <= 0.f || (maxScale - minScale) > maxScale * 1e-3f)
                return -1.f;
            return maxScale;
        }

        inline osg::BoundingBox worldBound(const osg::BoundingBox& local, const osg::Matrixf& world)
        {
            osg::BoundingBox result;
            for (unsigned int corner = 0; corner < 8; ++corner)
                result.expandBy(local.corner(corner) * world);
            return result;
        }

        inline bool intersects(const osg::BoundingBox& box, const osg::Vec3f& from, const osg::Vec3f& to)
        {
            if (!box.valid())
                return false;
            const osg::Vec3f delta = to - from;
            float near = 0.f;
            float far = 1.f;
            for (int axis = 0; axis < 3; ++axis)
            {
                if (std::abs(delta[axis]) < 1e-7f)
                {
                    if (from[axis] < box._min[axis] || from[axis] > box._max[axis])
                        return false;
                    continue;
                }
                const float inverse = 1.f / delta[axis];
                float first = (box._min[axis] - from[axis]) * inverse;
                float last = (box._max[axis] - from[axis]) * inverse;
                if (first > last)
                    std::swap(first, last);
                near = std::max(near, first);
                far = std::min(far, last);
                if (near > far)
                    return false;
            }
            return true;
        }

        inline void restoreProxy(osg::Node& node)
        {
            if (node.getNodeMask() == MWRender::Mask_GpuProxy)
                node.setNodeMask(~0u);
            osg::Group* group = node.asGroup();
            if (group != nullptr)
                for (unsigned int i = 0; i < group->getNumChildren(); ++i)
                    restoreProxy(*group->getChild(i));
        }

        const osg::Callback* foreignCullCallback(const osg::Drawable& drawable)
        {
            const osg::Callback* callback = drawable.getCullCallback();
            if (const auto* capture = dynamic_cast<const GpuCaptureCallback*>(callback))
                return capture->getWrapped();
            return callback;
        }

        class GpuExtractor
        {
        public:
            GpuExtractor(
                GpuScene& scene, osg::Node::NodeMask mask, bool bakeBillboards, bool nightDay, GpuTemplate& out)
                : mScene(scene)
                , mMask(mask)
                , mBakeBillboards(bakeBillboards)
                , mNightDay(nightDay)
                , mOut(out)
            {
            }

            void visit(const osg::Node* node, const osg::Matrixf& matrix, const osg::LOD* lod, unsigned int lodChild,
                const osg::Vec3f& lodCenter, float lodScale, bool root = false)
            {
                if (!(node->getNodeMask() & mMask))
                    return;
                if (dynamic_cast<const osgParticle::ParticleProcessor*>(node) != nullptr
                    || dynamic_cast<const osgParticle::ParticleSystemUpdater*>(node) != nullptr)
                    return;
                const bool trackedSwitch = node->asSwitch() != nullptr && node->getName() == Constants::NightDayLabel;
                // Paged copies discard node update callbacks.
                if ((!mBakeBillboards && !root && node->getUpdateCallback() != nullptr && !trackedSwitch)
                    || (node->getStateSet() != nullptr && node->getStateSet()->requiresUpdateTraversal()))
                {
                    mOut.mLegacy = true;
                    return;
                }
                const osg::Callback* cullCallback = node->asDrawable() != nullptr
                    ? foreignCullCallback(*node->asDrawable())
                    : node->getCullCallback();
                osg::ref_ptr<const osg::StateSet> live;
                if (cullCallback != nullptr && !root)
                {
                    if (node->asDrawable() == nullptr)
                        live = mScene.liveStateSet(*cullCallback);
                    if (live == nullptr)
                    {
                        mOut.mLegacy = true;
                        return;
                    }
                }

                const std::size_t depth = mStates.size();
                if (node->getStateSet() != nullptr)
                    mStates.push_back(node->getStateSet());
                if (live != nullptr)
                {
                    mStates.push_back(live.get());
                    mLive.push_back(live.get());
                }

                if (const osg::Drawable* drawable = node->asDrawable())
                    visitDrawable(drawable, matrix, lod, lodChild, lodCenter, lodScale);
                else if (const osg::Switch* sw = node->asSwitch())
                {
                    // Capture each night-day child.
                    if (mNightDay && trackedSwitch)
                    {
                        const unsigned int outer = mModes;
                        for (unsigned int i = 0; i < sw->getNumChildren(); ++i)
                        {
                            const unsigned int modes = GpuScene::switchModes(sw->getNumChildren(), i);
                            if (modes == 0)
                                continue;
                            mModes = modes;
                            visit(sw->getChild(i), matrix, lod, lodChild, lodCenter, lodScale);
                        }
                        mModes = outer;
                    }
                    else
                    {
                        for (unsigned int i = 0; i < sw->getNumChildren(); ++i)
                            if (sw->getValue(i))
                                visit(sw->getChild(i), matrix, lod, lodChild, lodCenter, lodScale);
                    }
                }
                else if (const osg::LOD* nested = dynamic_cast<const osg::LOD*>(node))
                {
                    const float scale = uniformScale(matrix);
                    if (lod != nullptr || nested->getRangeMode() != osg::LOD::DISTANCE_FROM_EYE_POINT || scale < 0.f)
                        mOut.mLegacy = true;
                    else
                    {
                        const osg::Vec3f center = nested->getCenter() * matrix;
                        for (unsigned int i = 0; i < nested->getNumChildren(); ++i)
                            visit(nested->getChild(i), matrix, nested, i, center, scale);
                    }
                }
                else if (const osg::Sequence* sq = dynamic_cast<const osg::Sequence*>(node))
                    visit(sq->getChild(sq->getValue() != -1 ? sq->getValue() : 0), matrix, lod, lodChild, lodCenter,
                        lodScale);
                else if (const osg::Transform* transform = node->asTransform())
                {
                    const auto* autoTransform = dynamic_cast<const NifOsg::AutoTransform*>(node);
                    if (autoTransform != nullptr && !mBakeBillboards)
                        mOut.mLegacy = true;
                    else
                    {
                        osg::Matrix local(matrix);
                        if (autoTransform != nullptr)
                            local.preMult(autoTransform->computeMatrix(nullptr));
                        else
                            transform->computeLocalToWorldMatrix(local, nullptr);
                        const osg::Matrixf localf(local);
                        for (unsigned int i = 0; i < transform->getNumChildren(); ++i)
                            visit(transform->getChild(i), localf, lod, lodChild, lodCenter, lodScale);
                    }
                }
                else if (const osg::Group* group = node->asGroup())
                {
                    for (unsigned int i = 0; i < group->getNumChildren(); ++i)
                        visit(group->getChild(i), matrix, lod, lodChild, lodCenter, lodScale);
                }
                else
                    mOut.mLegacy = true;

                mStates.resize(depth);
                if (live != nullptr)
                    mLive.pop_back();
            }

        private:
            void visitDrawable(const osg::Drawable* drawable, const osg::Matrixf& matrix, const osg::LOD* lod,
                unsigned int lodChild, const osg::Vec3f& lodCenter, float lodScale)
            {
                if (dynamic_cast<const osgParticle::ParticleSystem*>(drawable) != nullptr
                    || dynamic_cast<const SceneUtil::OsgaRigGeometry*>(drawable) != nullptr)
                    return;
                const osg::Geometry* geometry = nullptr;
                if (const auto* rig = dynamic_cast<const SceneUtil::RigGeometry*>(drawable))
                    geometry = rig->getSourceGeometry();
                else if (const auto* morph = dynamic_cast<const SceneUtil::MorphGeometry*>(drawable))
                    geometry = morph->getSourceGeometry();
                else
                    geometry = drawable->asGeometry();

                const float scale = geometry != nullptr ? uniformScale(matrix) : -1.f;
                if (geometry == nullptr || scale < 0.f || foreignCullCallback(*geometry) != nullptr)
                {
                    mOut.mLegacy = true;
                    return;
                }

                const std::size_t depth = mStates.size();
                if (geometry != drawable && geometry->getStateSet() != nullptr)
                    mStates.push_back(geometry->getStateSet());
                if (orderDependentPath(mStates))
                {
                    mOut.mLegacy = true;
                    return;
                }
                const GpuScene::Id mesh = mScene.getMesh(*geometry);
                const GpuScene::MaterialUpdate updates
                    = mLive.empty() ? GpuScene::MaterialUpdate::None : GpuScene::MaterialUpdate::Record;
                const GpuScene::Surface surface
                    = mesh != GpuScene::sInvalid ? mScene.getSurface(mStates, 0, updates) : GpuScene::Surface();
                mStates.resize(depth);
                if (mesh == GpuScene::sInvalid || surface.mBucket == GpuScene::sInvalid)
                {
                    mOut.mLegacy = true;
                    return;
                }

                mOut.mClaimed.insert(drawable);
                for (const osg::StateSet* live : mLive)
                    mScene.bindLiveMaterial(*live, surface.mMaterial);
                mOut.mGeometries.push_back(GpuGeometry{ geometry, surface, matrix, geometry->getBoundingBox(), lod,
                    lodChild, lodCenter, lodScale, drawable, mModes });
            }

            GpuScene& mScene;
            osg::Node::NodeMask mMask;
            bool mBakeBillboards;
            bool mNightDay;
            GpuTemplate& mOut;
            std::vector<const osg::StateSet*> mStates;
            std::vector<const osg::StateSet*> mLive;
            unsigned int mModes = 0;
        };

        class CaptureVisitor : public osg::NodeVisitor
        {
        public:
            struct CapturedRig
            {
                SceneUtil::RigGeometry* mRig;
                std::vector<const osg::StateSet*> mStates;
                bool mUpdater;
            };

            struct Part
            {
                osg::Drawable* mDrawable;
                osg::Group* mParent;
                std::vector<const osg::StateSet*> mStates;
                std::vector<osg::ref_ptr<osg::Transform>> mTransforms;
            };

            explicit CaptureVisitor(osg::Node* skip, bool actor = false)
                : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
                , mSkip(skip)
                , mActor(actor)
            {
            }

            void apply(osg::Node& node) override
            {
                if (&node == mSkip)
                    return;
                const std::size_t depth = mStates.size();
                if (node.getStateSet() != nullptr)
                    mStates.push_back(node.getStateSet());
                const bool updater = hasUpdater(node.getUpdateCallback()) || hasUpdater(node.getCullCallback());
                bool conditional = dynamic_cast<osg::Billboard*>(&node) != nullptr
                    || dynamic_cast<NifOsg::AutoTransform*>(&node) != nullptr
                    || dynamic_cast<osg::Switch*>(&node) != nullptr || dynamic_cast<osg::LOD*>(&node) != nullptr
                    || dynamic_cast<osg::Sequence*>(&node) != nullptr
                    || (node.getNodeMask() != ~0u && node.getNodeMask() != Mask_GpuProxy);
                for (const osg::Callback* callback = node.getCullCallback(); callback != nullptr;
                     callback = callback->getNestedCallback())
                    conditional = conditional || dynamic_cast<const SceneUtil::LightListCallback*>(callback) == nullptr;
                mUpdaterDepth += updater;
                mConditionalDepth += conditional;
                traverse(node);
                mConditionalDepth -= conditional;
                mUpdaterDepth -= updater;
                mStates.resize(depth);
            }

            void apply(osg::Drawable& drawable) override
            {
                const std::size_t depth = mStates.size();
                if (drawable.getStateSet() != nullptr)
                    mStates.push_back(drawable.getStateSet());
                const bool updater = hasUpdater(drawable.getUpdateCallback()) || hasUpdater(drawable.getCullCallback());
                if (auto* rig = dynamic_cast<SceneUtil::RigGeometry*>(&drawable))
                    mRigs.push_back(CapturedRig{ rig, mStates, mUpdaterDepth != 0 || updater });
                else if (auto* morph = dynamic_cast<SceneUtil::MorphGeometry*>(&drawable))
                    mMorphs.push_back(morph);
                else if (osg::Geometry* geometry = drawable.asGeometry())
                    mGeometries.push_back(geometry);
                if (mActor && !mUpdaterDepth && !updater && !mConditionalDepth
                    && (drawable.getNodeMask() == ~0u || drawable.getNodeMask() == Mask_GpuProxy)
                    && drawable.getDrawCallback() == nullptr && foreignCullCallback(drawable) == nullptr)
                {
                    Part part{ &drawable, nullptr, mStates, {} };
                    for (osg::Node* node : getNodePath())
                    {
                        if (auto* group = node->asGroup())
                            part.mParent = group;
                        if (auto* transform = node->asTransform())
                            part.mTransforms.emplace_back(transform);
                    }
                    mParts.push_back(std::move(part));
                }
                mStates.resize(depth);
            }

            std::vector<Part> mParts;
            std::vector<osg::Geometry*> mGeometries;
            std::vector<CapturedRig> mRigs;
            std::vector<SceneUtil::MorphGeometry*> mMorphs;

        private:
            osg::Node* mSkip;
            bool mActor;
            std::vector<const osg::StateSet*> mStates;
            unsigned int mUpdaterDepth = 0;
            unsigned int mConditionalDepth = 0;
        };
    }
}

#endif
