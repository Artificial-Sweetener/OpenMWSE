#include "objects.hpp"

#include <osg/Group>
#include <osg/UserDataContainer>

#include <components/misc/resourcehelpers.hpp>
#include <components/misc/strings/algorithm.hpp>
#include <components/sceneutil/positionattitudetransform.hpp>
#include <components/sceneutil/unrefqueue.hpp>

#include "../mwworld/class.hpp"
#include "../mwworld/ptr.hpp"

#include "animation.hpp"
#include "creatureanimation.hpp"
#include "esm4npcanimation.hpp"
#include "gpuscene.hpp"
#include "npcanimation.hpp"
#include "vismask.hpp"
#include "water.hpp"

namespace MWRender
{

    Objects::Objects(Resource::ResourceSystem* resourceSystem, const osg::ref_ptr<osg::Group>& rootNode,
        GpuScene* gpuScene, SceneUtil::UnrefQueue& unrefQueue)
        : mRootNode(rootNode)
        , mResourceSystem(resourceSystem)
        , mGpuScene(gpuScene)
        , mUnrefQueue(unrefQueue)
    {
    }

    Objects::~Objects()
    {
        mObjects.clear();

        for (CellMap::iterator iter = mCellSceneNodes.begin(); iter != mCellSceneNodes.end(); ++iter)
            iter->second->getParent(0)->removeChild(iter->second);
        mCellSceneNodes.clear();
    }

    osg::Group* Objects::createCellNode(const MWWorld::CellStore* store)
    {
        osg::ref_ptr<osg::Group> cellnode = new osg::Group;
        cellnode->setName("Cell Root");
        // GPU-resident objects park under one masked child.
        osg::ref_ptr<osg::Group> proxy = new osg::Group;
        proxy->setNodeMask(Mask_GpuProxy);
        cellnode->addChild(proxy);
        mRootNode->addChild(cellnode);
        mCellSceneNodes[store] = cellnode;
        return cellnode;
    }

    void Objects::insertBegin(const MWWorld::Ptr& ptr)
    {
        assert(mObjects.find(ptr.mRef) == mObjects.end());

        osg::ref_ptr<osg::Group> cellnode;

        CellMap::iterator found = mCellSceneNodes.find(ptr.getCell());
        if (found == mCellSceneNodes.end())
            cellnode = createCellNode(ptr.getCell());
        else
            cellnode = found->second;

        osg::ref_ptr<SceneUtil::PositionAttitudeTransform> insert(new SceneUtil::PositionAttitudeTransform);
        cellnode->addChild(insert);

        insert->getOrCreateUserDataContainer()->addUserObject(new PtrHolder(ptr));

        const float* f = ptr.getRefData().getPosition().pos;

        insert->setPosition(osg::Vec3(f[0], f[1], f[2]));

        const float scale = ptr.getCellRef().getScale();
        osg::Vec3f scaleVec(scale, scale, scale);
        ptr.getClass().adjustScale(ptr, scaleVec, true);
        insert->setScale(scaleVec);

        ptr.getRefData().setBaseNode(std::move(insert));
    }

    void Objects::insertModel(const MWWorld::Ptr& ptr, const std::string& mesh, bool allowLight)
    {
        insertBegin(ptr);
        ptr.getRefData().getBaseNode()->setNodeMask(Mask_Object);
        bool animated = ptr.getClass().useAnim();
        std::string animationMesh = mesh;
        if (animated && !mesh.empty())
        {
            animationMesh = Misc::ResourceHelpers::correctActorModelPath(
                VFS::Path::toNormalized(mesh), mResourceSystem->getVFS());
            if (animationMesh == mesh && Misc::StringUtils::ciEndsWith(animationMesh, ".nif"))
                animated = false;
        }

        osg::ref_ptr<ObjectAnimation> anim(
            new ObjectAnimation(ptr, animationMesh, mResourceSystem, animated, allowLight, mGpuRefreshPending));

        if (mWater)
        {
            bool hasSurfaceEffects = false;
            ptr.getRefData().getBaseNode()->getUserValue("surfaceEffects", hasSurfaceEffects);
            if (!hasSurfaceEffects && anim->getObjectRoot())
                anim->getObjectRoot()->getUserValue("surfaceEffects", hasSurfaceEffects);
            if (hasSurfaceEffects)
                ptr.getRefData().getBaseNode()->setUserValue("surfaceEffects", true);
        }
        anim->setGpuScene(mGpuScene);
        mObjects.emplace(ptr.mRef, std::move(anim));
    }

    void Objects::insertCreature(const MWWorld::Ptr& ptr, const std::string& mesh, bool weaponsShields)
    {
        insertBegin(ptr);
        ptr.getRefData().getBaseNode()->setNodeMask(Mask_Actor);

        bool animated = true;
        std::string animationMesh
            = Misc::ResourceHelpers::correctActorModelPath(VFS::Path::toNormalized(mesh), mResourceSystem->getVFS());
        if (animationMesh == mesh && Misc::StringUtils::ciEndsWith(animationMesh, ".nif"))
            animated = false;

        // CreatureAnimation
        osg::ref_ptr<Animation> anim;

        if (weaponsShields)
            anim = new CreatureWeaponAnimation(ptr, animationMesh, mResourceSystem, animated);
        else
            anim = new CreatureAnimation(ptr, animationMesh, mResourceSystem, animated);

        anim->setGpuScene(mGpuScene);
        if (mObjects.emplace(ptr.mRef, anim).second)
            ptr.getClass().getContainerStore(ptr).setContListener(static_cast<ActorAnimation*>(anim.get()));
    }

    void Objects::setGpuScene(GpuScene* scene)
    {
        mGpuScene = scene;
        for (const auto& [ref, anim] : mObjects)
            anim->setGpuScene(scene);
    }

    void Objects::insertNPC(const MWWorld::Ptr& ptr)
    {
        insertBegin(ptr);
        ptr.getRefData().getBaseNode()->setNodeMask(Mask_Actor);

        if (ptr.getType() == ESM::REC_NPC_4)
        {
            osg::ref_ptr<ESM4NpcAnimation> anim(
                new ESM4NpcAnimation(ptr, osg::ref_ptr<osg::Group>(ptr.getRefData().getBaseNode()), mResourceSystem));
            anim->setGpuScene(mGpuScene);
            mObjects.emplace(ptr.mRef, anim);
        }
        else
        {
            osg::ref_ptr<NpcAnimation> anim(
                new NpcAnimation(ptr, osg::ref_ptr<osg::Group>(ptr.getRefData().getBaseNode()), mResourceSystem));
            anim->setGpuScene(mGpuScene);

            if (mObjects.emplace(ptr.mRef, anim).second)
            {
                ptr.getClass().getInventoryStore(ptr).setInvListener(anim.get());
                ptr.getClass().getInventoryStore(ptr).setContListener(anim.get());
            }
        }
    }

    bool Objects::removeObject(const MWWorld::Ptr& ptr)
    {
        if (!ptr.getRefData().getBaseNode())
            return true;

        const auto iter = mObjects.find(ptr.mRef);
        if (iter != mObjects.end())
        {
            if (mWater)
                mWater->unregisterSurfaceEffects(ptr.getRefData().getBaseNode());
            iter->second->removeFromScene();
            mUnrefQueue.push(std::move(iter->second));
            mObjects.erase(iter);

            if (ptr.getClass().isActor())
            {
                if (ptr.getClass().hasInventoryStore(ptr))
                    ptr.getClass().getInventoryStore(ptr).setInvListener(nullptr);

                ptr.getClass().getContainerStore(ptr).setContListener(nullptr);
            }

            ptr.getRefData().getBaseNode()->getParent(0)->removeChild(ptr.getRefData().getBaseNode());

            ptr.getRefData().setBaseNode(nullptr);
            return true;
        }
        return false;
    }

    void Objects::removeCell(const MWWorld::CellStore* store)
    {
        for (PtrAnimationMap::iterator iter = mObjects.begin(); iter != mObjects.end();)
        {
            MWWorld::Ptr ptr = iter->second->getPtr();
            if (ptr.getCell() == store)
            {
                if (mWater)
                    mWater->unregisterSurfaceEffects(ptr.getRefData().getBaseNode());
                if (ptr.getClass().isActor() && ptr.getRefData().getCustomData())
                {
                    if (ptr.getClass().hasInventoryStore(ptr))
                        ptr.getClass().getInventoryStore(ptr).setInvListener(nullptr);
                    ptr.getClass().getContainerStore(ptr).setContListener(nullptr);
                }

                iter->second->removeFromScene();
                mUnrefQueue.push(std::move(iter->second));
                iter = mObjects.erase(iter);
            }
            else
                ++iter;
        }

        CellMap::iterator cell = mCellSceneNodes.find(store);
        if (cell != mCellSceneNodes.end())
        {
            cell->second->getParent(0)->removeChild(cell->second);
            mCellSceneNodes.erase(cell);
        }
    }

    void Objects::flushGpuRefresh()
    {
        if (mGpuScene == nullptr || !mGpuRefreshPending)
            return;
        mGpuRefreshPending = false;
        for (auto& [_, animation] : mObjects)
            animation->flushGpuRefresh();
    }

    void Objects::refreshGpuInstances()
    {
        for (auto& [_, animation] : mObjects)
            animation->refreshGpuInstances();
        if (mGpuScene != nullptr)
            mGpuScene->capture(*mRootNode);
    }

    void Objects::updatePtr(const MWWorld::Ptr& old, const MWWorld::Ptr& cur)
    {
        osg::ref_ptr<osg::Node> objectNode = cur.getRefData().getBaseNode();
        if (!objectNode)
            return;

        MWWorld::CellStore* newCell = cur.getCell();

        osg::Group* cellnode;
        if (mCellSceneNodes.find(newCell) == mCellSceneNodes.end())
            cellnode = createCellNode(newCell);
        else
            cellnode = mCellSceneNodes[newCell];

        osg::UserDataContainer* userDataContainer = objectNode->getUserDataContainer();
        if (userDataContainer)
            for (unsigned int i = 0; i < userDataContainer->getNumUserObjects(); ++i)
            {
                if (dynamic_cast<PtrHolder*>(userDataContainer->getUserObject(i)))
                    userDataContainer->setUserObject(i, new PtrHolder(cur));
            }

        if (objectNode->getNumParents())
            objectNode->getParent(0)->removeChild(objectNode);
        cellnode->addChild(objectNode);

        PtrAnimationMap::iterator iter = mObjects.find(old.mRef);
        if (iter != mObjects.end())
            iter->second->updatePtr(cur);
    }

    Animation* Objects::getAnimation(const MWWorld::Ptr& ptr)
    {
        PtrAnimationMap::const_iterator iter = mObjects.find(ptr.mRef);
        if (iter != mObjects.end())
            return iter->second;

        return nullptr;
    }

    const Animation* Objects::getAnimation(const MWWorld::ConstPtr& ptr) const
    {
        PtrAnimationMap::const_iterator iter = mObjects.find(ptr.mRef);
        if (iter != mObjects.end())
            return iter->second;

        return nullptr;
    }

}
