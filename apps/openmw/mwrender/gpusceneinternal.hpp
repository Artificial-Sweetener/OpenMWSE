#ifndef OPENMW_MWRENDER_GPUSCENEINTERNAL_H
#define OPENMW_MWRENDER_GPUSCENEINTERNAL_H

#include "gpuscene.hpp"
#include "renderbin.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <limits>
#include <numeric>
#include <optional>
#include <string_view>
#include <tuple>

#include <osg/AlphaFunc>
#include <osg/Billboard>
#include <osg/BufferObject>
#include <osg/Callback>
#include <osg/ColorMask>
#include <osg/Drawable>
#include <osg/FrameBufferObject>
#include <osg/FrontFace>
#include <osg/GLDefines>
#include <osg/GLExtensions>
#include <osg/Geometry>
#include <osg/GraphicsContext>
#include <osg/GraphicsThread>
#include <osg/LOD>
#include <osg/Multisample>
#include <osg/PolygonMode>
#include <osg/PrimitiveSet>
#include <osg/Sampler>
#include <osg/Sequence>
#include <osg/State>
#include <osg/Stats>
#include <osg/Switch>
#include <osg/Texture2D>
#include <osg/Texture2DArray>
#include <osg/Texture>
#include <osg/VertexArrayState>
#include <osg/observer_ptr>
#include <osgParticle/ParticleProcessor>
#include <osgParticle/ParticleSystem>
#include <osgParticle/ParticleSystemUpdater>
#include <osgUtil/CullVisitor>
#include <osgUtil/RenderStage>
#include <osgUtil/StateGraph>

#include <components/debug/debuglog.hpp>
#include <components/misc/constants.hpp>
#include <components/nifosg/autotransform.hpp>
#include <components/nifosg/controller.hpp>
#include <components/sceneutil/geometrybatch.hpp>
#include <components/sceneutil/glextensions.hpp>
#include <components/sceneutil/lightmanager.hpp>
#include <components/sceneutil/material.hpp>
#include <components/sceneutil/morphgeometry.hpp>
#include <components/sceneutil/positionattitudetransform.hpp>
#include <components/sceneutil/riggeometry.hpp>
#include <components/sceneutil/riggeometryosgaextension.hpp>
#include <components/sceneutil/shadowsbin.hpp>
#include <components/sceneutil/statesetupdater.hpp>
#include <components/sceneutil/texturetype.hpp>
#include <components/sceneutil/util.hpp>
#include <components/sceneutil/workqueue.hpp>
#include <components/settings/values.hpp>
#include <components/shader/shadermanager.hpp>
#include <components/terrain/compositemaprenderer.hpp>
#include <components/terrain/terraindrawable.hpp>

#include "gpuscenekeys.hpp"
#include "postprocessor.hpp"
#include "vismask.hpp"

#ifndef GL_COPY_READ_BUFFER
#define GL_COPY_READ_BUFFER 0x8F36
#endif
#ifndef GL_COPY_WRITE_BUFFER
#define GL_COPY_WRITE_BUFFER 0x8F37
#endif
#ifndef GL_R32UI
#define GL_R32UI 0x8236
#endif
#ifndef GL_RED_INTEGER
#define GL_RED_INTEGER 0x8D94
#endif
#ifndef GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS
#define GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS 0x90DD
#endif
#ifndef GL_MAX_VERTEX_SHADER_STORAGE_BLOCKS
#define GL_MAX_VERTEX_SHADER_STORAGE_BLOCKS 0x90D6
#endif
#ifndef GL_MAX_FRAGMENT_SHADER_STORAGE_BLOCKS
#define GL_MAX_FRAGMENT_SHADER_STORAGE_BLOCKS 0x90DA
#endif
#ifndef GL_MAX_COMBINED_SHADER_STORAGE_BLOCKS
#define GL_MAX_COMBINED_SHADER_STORAGE_BLOCKS 0x90DC
#endif
#ifndef GL_TEXTURE_SAMPLES
#define GL_TEXTURE_SAMPLES 0x9106
#endif
#ifndef GL_ARRAY_BUFFER
#define GL_ARRAY_BUFFER 0x8892
#endif
#ifndef GL_ELEMENT_ARRAY_BUFFER
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#endif
#ifndef GL_VERTEX_ARRAY_BINDING
#define GL_VERTEX_ARRAY_BINDING 0x85B5
#endif
#ifndef GL_GPU_MEMORY_INFO_CURRENT_AVAILABLE_VIDMEM_NVX
#define GL_GPU_MEMORY_INFO_CURRENT_AVAILABLE_VIDMEM_NVX 0x9049
#endif
#ifndef GL_TEXTURE_FREE_MEMORY_ATI
#define GL_TEXTURE_FREE_MEMORY_ATI 0x87FC
#endif
#ifndef GL_MAX_VERTEX_SHADER_STORAGE_BLOCKS
#define GL_MAX_VERTEX_SHADER_STORAGE_BLOCKS 0x90D6
#endif
#ifndef GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS
#define GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS 0x90DD
#endif
#ifndef GL_PRIMITIVES_GENERATED
#define GL_PRIMITIVES_GENERATED 0x8C87
#endif
#ifndef GL_TIME_ELAPSED
#define GL_TIME_ELAPSED 0x88BF
#endif
#ifndef GL_QUERY_RESULT
#define GL_QUERY_RESULT 0x8866
#endif
#ifndef GL_QUERY_RESULT_AVAILABLE
#define GL_QUERY_RESULT_AVAILABLE 0x8867
#endif

// These implementation fragments define nested types in dependency order and are not standalone headers.
// clang-format off
#include "gpuscenecaptureinternal.hpp"
#include "gpuscenehelpersinternal.hpp"
#include "gpuscenenodesinternal.hpp"
#include "gpuscenerayinternal.hpp"
#include "gpuscenetextureinternal.hpp"
#include "gpusceneglinternal.hpp"
// clang-format on

#endif
