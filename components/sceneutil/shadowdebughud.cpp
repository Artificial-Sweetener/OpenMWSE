#include "shadowdebughud.hpp"

#include <cstddef>
#include <string>
#include <utility>

#include <osg/Callback>
#include <osg/Shader>
#include <osg/StateSet>
#include <osg/Version>

namespace
{
    const std::string sDebugVertexShader
        = "void main(void){gl_Position = gl_Vertex; gl_TexCoord[0]=gl_MultiTexCoord0;}";
    const std::string sDebugFragmentShader = R"(
        uniform sampler2D texture;

        void main(void)
        {
            float f = texture2D(texture, gl_TexCoord[0].xy).r;

            f = 256.0 * f;
            float fC = floor(f) / 256.0;

            f = 256.0 * fract(f);
            float fS = floor(f) / 256.0;

            f = 256.0 * fract(f);
            float fH = floor(f) / 256.0;

            fS *= 0.5;
            fH = (fH * 0.34 + 0.66) * (1.0 - fS);

            vec3 rgb = vec3((fC > 0.5 ? (1.0 - fC) : fC), abs(fC - 0.333333), abs(fC - 0.666667));
            rgb = min(vec3(1.0, 1.0, 1.0), 3.0 * rgb);

            float fMax = 1.0 / max(max(rgb.r, rgb.g), rgb.b);
            gl_FragColor = vec4(fS + fH * fMax * rgb, 1.0);
        }
    )";
    const std::string sDebugFrustumVertexShader
        = "varying float depth; uniform mat4 transform; void main(void){gl_Position = transform * gl_Vertex; depth = "
          "gl_Position.z / gl_Position.w;}";
    const std::string sDebugFrustumFragmentShader = R"(
        varying float depth;

        void main(void)
        {
            float f = depth;

            f = 256.0 * f;
            float fC = floor(f) / 256.0;

            f = 256.0 * fract(f);
            float fS = floor(f) / 256.0;

            f = 256.0 * fract(f);
            float fH = floor(f) / 256.0;

            fS *= 0.5;
            fH = (fH * 0.34 + 0.66) * (1.0 - fS);

            vec3 rgb = vec3((fC > 0.5 ? (1.0 - fC) : fC), abs(fC - 0.333333), abs(fC - 0.666667));
            rgb = min(vec3(1.0, 1.0, 1.0), 3.0 * rgb);

            float fMax = 1.0 / max(max(rgb.r, rgb.g), rgb.b);
            gl_FragColor = vec4(fS + fH * fMax * rgb, 1.0);
        }
    )";

    /// Select one of two frustum geometry buffers for the active traversal.
    class DoubleBufferCallback final : public osg::Callback
    {
    public:
        /// Retain the two traversal-owned children selected by the callback.
        explicit DoubleBufferCallback(osg::NodeList children)
            : mChildren(std::move(children))
        {
        }

        /// Traverse the child corresponding to the visitor traversal number.
        bool run(osg::Object*, osg::Object* visitor) override
        {
            osg::ref_ptr<osg::NodeVisitor> nodeVisitor = visitor->asNodeVisitor();
            mChildren[nodeVisitor->getTraversalNumber() % 2]->accept(*nodeVisitor);
            return true;
        }

    private:
        osg::NodeList mChildren;
    };
}

SceneUtil::ShadowDebugHud::ShadowDebugHud(int numberOfShadowMapsPerLight)
    : mDebugProgram(new osg::Program)
{
    mDebugProgram->addShader(new osg::Shader(osg::Shader::VERTEX, sDebugVertexShader));
    mDebugProgram->addShader(new osg::Shader(osg::Shader::FRAGMENT, sDebugFragmentShader));

    osg::ref_ptr<osg::Program> frustumProgram = new osg::Program;
    frustumProgram->addShader(new osg::Shader(osg::Shader::VERTEX, sDebugFrustumVertexShader));
    frustumProgram->addShader(new osg::Shader(osg::Shader::FRAGMENT, sDebugFrustumFragmentShader));

    for (auto& frustumGeometry : mFrustumGeometries)
    {
        frustumGeometry = new osg::Geometry;
        frustumGeometry->setCullingActive(false);
        frustumGeometry->getOrCreateStateSet()->setAttributeAndModes(frustumProgram, osg::StateAttribute::ON);
    }

    osg::ref_ptr<osg::DrawElementsUShort> strip = new osg::DrawElementsUShort(osg::PrimitiveSet::LINE_STRIP);
    for (auto& geometry : mFrustumGeometries)
        geometry->addPrimitiveSet(strip);
    for (const unsigned short index : { 0, 1, 2, 3, 0, 4, 5, 6, 7, 4 })
        strip->push_back(index);

    osg::ref_ptr<osg::DrawElementsUShort> lines = new osg::DrawElementsUShort(osg::PrimitiveSet::LINES);
    for (auto& geometry : mFrustumGeometries)
        geometry->addPrimitiveSet(lines);
    for (const unsigned short index : { 1, 5, 2, 6, 3, 7 })
        lines->push_back(index);

    for (int index = 0; index < numberOfShadowMapsPerLight; ++index)
        addShadowMap();
}

void SceneUtil::ShadowDebugHud::draw(osg::ref_ptr<osg::Texture2D> texture, unsigned int shadowMapNumber,
    const osg::Matrixd& matrix, osgUtil::CullVisitor& visitor)
{
    if (shadowMapNumber > mDebugCameras.size())
        addShadowMap();

    osg::ref_ptr<osg::StateSet> stateSet = new osg::StateSet;
    stateSet->setTextureAttribute(sDebugTextureUnit, texture, osg::StateAttribute::ON);

    mFrustumUniforms[visitor.getTraversalNumber() % 2][shadowMapNumber]->set(matrix);
    stateSet->addUniform(mFrustumUniforms[visitor.getTraversalNumber() % 2][shadowMapNumber]);

    const unsigned int traversalMask = visitor.getTraversalMask();
    visitor.setTraversalMask(mDebugGeometry[shadowMapNumber]->getNodeMask());
    visitor.pushStateSet(stateSet);
    mDebugCameras[shadowMapNumber]->accept(visitor);
    visitor.popStateSet();
    visitor.setTraversalMask(traversalMask);
}

void SceneUtil::ShadowDebugHud::releaseGLObjects(osg::State* state) const
{
    for (const auto& camera : mDebugCameras)
        camera->releaseGLObjects(state);
    mDebugProgram->releaseGLObjects(state);
    for (const auto& node : mDebugGeometry)
        node->releaseGLObjects(state);
    for (const auto& node : mFrustumTransforms)
        node->releaseGLObjects(state);
    for (const auto& node : mFrustumGeometries)
        node->releaseGLObjects(state);
}

void SceneUtil::ShadowDebugHud::setFrustumVertices(osg::ref_ptr<osg::Vec3Array> vertices, unsigned int traversalNumber)
{
    mFrustumGeometries[traversalNumber % 2]->setVertexArray(vertices);
}

void SceneUtil::ShadowDebugHud::addShadowMap()
{
    const std::size_t shadowMapNumber = mDebugCameras.size();

    mDebugCameras.push_back(new osg::Camera);
    mDebugCameras[shadowMapNumber]->setViewport(static_cast<int>(200 * shadowMapNumber), 0, 200, 200);
    mDebugCameras[shadowMapNumber]->setRenderOrder(osg::Camera::POST_RENDER);
    mDebugCameras[shadowMapNumber]->setClearColor(osg::Vec4(1.0, 1.0, 0.0, 1.0));
    mDebugCameras[shadowMapNumber]->getOrCreateStateSet()->setMode(GL_DEPTH_TEST, osg::StateAttribute::OFF);

    mDebugGeometry.emplace_back(
        osg::createTexturedQuadGeometry(osg::Vec3(-1, -1, 0), osg::Vec3(2, 0, 0), osg::Vec3(0, 2, 0)));
    mDebugGeometry[shadowMapNumber]->setCullingActive(false);
    mDebugCameras[shadowMapNumber]->addChild(mDebugGeometry[shadowMapNumber]);
    osg::ref_ptr<osg::StateSet> stateSet = mDebugGeometry[shadowMapNumber]->getOrCreateStateSet();
    stateSet->setAttributeAndModes(mDebugProgram, osg::StateAttribute::ON);
    stateSet->addUniform(new osg::Uniform("texture", sDebugTextureUnit));

    mFrustumTransforms.push_back(new osg::Group);
    mFrustumTransforms[shadowMapNumber]->setCullCallback(
        new DoubleBufferCallback(osg::NodeList(mFrustumGeometries.cbegin(), mFrustumGeometries.cend())));
    mFrustumTransforms[shadowMapNumber]->setCullingActive(false);
    mDebugCameras[shadowMapNumber]->addChild(mFrustumTransforms[shadowMapNumber]);

    for (auto& uniformVector : mFrustumUniforms)
        uniformVector.push_back(new osg::Uniform(osg::Uniform::FLOAT_MAT4, "transform"));
}
