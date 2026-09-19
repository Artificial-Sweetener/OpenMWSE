#ifndef OPENMW_COMPONENTS_RESOURCE_SURFACEEFFECTREGISTRY_H
#define OPENMW_COMPONENTS_RESOURCE_SURFACEEFFECTREGISTRY_H

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <components/sceneutil/surfaceeffect.hpp>
#include <components/vfs/pathutil.hpp>

namespace VFS
{
    class Manager;
}

namespace Resource
{
    /// Resolves mod-extensible material signals to renderer-owned surface-effect profiles.
    class SurfaceEffectRegistry
    {
    public:
        /// Load every VFS rule document and validate profile references deterministically.
        explicit SurfaceEffectRegistry(const VFS::Manager& vfs);

        /// Resolve the last matching rule without retaining any source texture as visual material input.
        std::optional<SceneUtil::SurfaceEffectProfile> match(VFS::Path::NormalizedView mesh, std::string_view material,
            const std::vector<VFS::Path::Normalized>& textures) const;

        /// Return the number of valid profiles exposed by the loaded rule set.
        std::size_t profileCount() const { return mProfiles.size(); }

        /// Return the number of validated classification rules in deterministic precedence order.
        std::size_t ruleCount() const { return mRules.size(); }

    private:
        struct Rule
        {
            std::string mProfile;
            std::string mMeshPattern;
            std::string mMaterialPattern;
            std::vector<std::string> mTexturePatterns;
        };

        std::unordered_map<std::string, SceneUtil::SurfaceEffectProfile> mProfiles;
        std::vector<Rule> mRules;
    };
}

#endif
