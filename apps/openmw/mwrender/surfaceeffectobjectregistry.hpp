#ifndef OPENMW_MWRENDER_SURFACEEFFECTOBJECTREGISTRY_H
#define OPENMW_MWRENDER_SURFACEEFFECTOBJECTREGISTRY_H

#include <unordered_map>
#include <vector>

#include <osg/Node>
#include <osg/observer_ptr>
#include <osg/ref_ptr>

namespace MWRender
{
    /// Track loaded surface-effect roots without extending their scene lifetime.
    class SurfaceEffectObjectRegistry
    {
    public:
        /// Hold the live roots and identify expired roots whose renderer bindings require cleanup.
        struct Snapshot
        {
            std::vector<osg::ref_ptr<osg::Node>> mLive;
            std::vector<const osg::Node*> mExpired;
        };

        /// Remember a loaded root through an observer that becomes invalid when the scene releases it.
        void remember(osg::Node& root) { mObjects.insert_or_assign(&root, &root); }

        /// Forget a root that is leaving the scene through the ordinary removal lifecycle.
        void forget(const osg::Node* root) { mObjects.erase(root); }

        /// Return whether a root is already known to the registry.
        bool contains(const osg::Node* root) const { return mObjects.contains(root); }

        /// Lock every live root for a rebuild and prune roots already released by the scene.
        Snapshot lockLive()
        {
            Snapshot result;
            result.mLive.reserve(mObjects.size());
            for (auto it = mObjects.begin(); it != mObjects.end();)
            {
                osg::ref_ptr<osg::Node> root;
                if (it->second.lock(root))
                {
                    result.mLive.push_back(std::move(root));
                    ++it;
                }
                else
                {
                    result.mExpired.push_back(it->first);
                    it = mObjects.erase(it);
                }
            }
            return result;
        }

    private:
        std::unordered_map<const osg::Node*, osg::observer_ptr<osg::Node>> mObjects;
    };
}

#endif
