#include "geometrybatch.hpp"

#include <algorithm>
#include <cstring>

namespace SceneUtil
{
    namespace
    {
        bool appendTriangles(const osg::PrimitiveSet& primitives, std::vector<std::uint32_t>& indices)
        {
            if (dynamic_cast<const osg::DrawArrayLengths*>(&primitives) != nullptr)
                return false;
            const unsigned int count = primitives.getNumIndices();
            switch (primitives.getMode())
            {
                case GL_TRIANGLES:
                    for (unsigned int i = 0; i + 2 < count; i += 3)
                    {
                        indices.push_back(primitives.index(i));
                        indices.push_back(primitives.index(i + 1));
                        indices.push_back(primitives.index(i + 2));
                    }
                    return true;
                case GL_QUADS:
                    for (unsigned int i = 0; i + 3 < count; i += 4)
                    {
                        indices.push_back(primitives.index(i));
                        indices.push_back(primitives.index(i + 1));
                        indices.push_back(primitives.index(i + 2));
                        indices.push_back(primitives.index(i));
                        indices.push_back(primitives.index(i + 2));
                        indices.push_back(primitives.index(i + 3));
                    }
                    return true;
                case GL_TRIANGLE_STRIP:
                    for (unsigned int i = 2; i < count; ++i)
                    {
                        const unsigned int a = (i % 2 == 0) ? i - 2 : i - 1;
                        const unsigned int b = (i % 2 == 0) ? i - 1 : i - 2;
                        indices.push_back(primitives.index(a));
                        indices.push_back(primitives.index(b));
                        indices.push_back(primitives.index(i));
                    }
                    return true;
                case GL_TRIANGLE_FAN:
                    for (unsigned int i = 2; i < count; ++i)
                    {
                        indices.push_back(primitives.index(0));
                        indices.push_back(primitives.index(i - 1));
                        indices.push_back(primitives.index(i));
                    }
                    return true;
                default:
                    return false;
            }
        }

        template <class Array>
        const Array* perVertexArray(const osg::Array* array, std::size_t vertices)
        {
            const Array* typed = dynamic_cast<const Array*>(array);
            if (typed == nullptr || typed->getBinding() != osg::Array::BIND_PER_VERTEX || typed->size() != vertices)
                return nullptr;
            return typed;
        }

    }

    bool packGeometry(
        const osg::Geometry& geometry, std::vector<std::uint8_t>& vertexBytes, std::vector<std::uint32_t>& indices)
    {
        const osg::Vec3Array* positions = dynamic_cast<const osg::Vec3Array*>(geometry.getVertexArray());
        if (positions == nullptr || positions->empty())
            return false;
        const std::size_t count = positions->size();
        const osg::Vec3Array* normals = perVertexArray<osg::Vec3Array>(geometry.getNormalArray(), count);
        osg::Vec3f overallNormal(0.f, 0.f, 1.f);
        if (geometry.getNormalArray() != nullptr && normals == nullptr)
        {
            const auto* overall = dynamic_cast<const osg::Vec3Array*>(geometry.getNormalArray());
            if (overall == nullptr || overall->getBinding() != osg::Array::BIND_OVERALL || overall->size() != 1)
                return false;
            overallNormal = (*overall)[0];
        }

        const osg::Array* colorArray = geometry.getColorArray();
        const osg::Vec4Array* colors = nullptr;
        const osg::Vec4ubArray* colorsUb = nullptr;
        osg::Vec4f overallColor(1.f, 1.f, 1.f, 1.f);
        if (colorArray != nullptr)
        {
            if (colorArray->getBinding() == osg::Array::BIND_OVERALL && colorArray->getNumElements() == 1)
            {
                if (const auto* c = dynamic_cast<const osg::Vec4Array*>(colorArray))
                    overallColor = (*c)[0];
                else if (const auto* cub = dynamic_cast<const osg::Vec4ubArray*>(colorArray))
                    overallColor = osg::Vec4f(
                        (*cub)[0].r() / 255.f, (*cub)[0].g() / 255.f, (*cub)[0].b() / 255.f, (*cub)[0].a() / 255.f);
                else
                    return false;
            }
            else
            {
                colors = perVertexArray<osg::Vec4Array>(colorArray, count);
                colorsUb = perVertexArray<osg::Vec4ubArray>(colorArray, count);
                if (colors == nullptr && colorsUb == nullptr)
                    return false;
            }
        }

        const osg::Vec2Array* uv[8] = {};
        const osg::Vec4Array* tangents = nullptr;
        for (unsigned int unit = 0; unit < geometry.getNumTexCoordArrays(); ++unit)
        {
            const osg::Array* array = geometry.getTexCoordArray(unit);
            if (array == nullptr)
                continue;
            if (unit < 7)
            {
                uv[unit] = perVertexArray<osg::Vec2Array>(array, count);
                if (uv[unit] == nullptr)
                    return false;
            }
            else if (unit == 7)
            {
                tangents = perVertexArray<osg::Vec4Array>(array, count);
                if (tangents == nullptr)
                {
                    uv[unit] = perVertexArray<osg::Vec2Array>(array, count);
                    if (uv[unit] == nullptr)
                        return false;
                }
            }
            else
                return false;
        }
        const osg::UIntArray* materials = perVertexArray<osg::UIntArray>(geometry.getVertexAttribArray(7), count);
        const auto* skin = perVertexArray<osg::Vec2uiArray>(geometry.getVertexAttribArray(8), count);
        for (unsigned int i = 0; i < geometry.getNumVertexAttribArrays(); ++i)
            if (geometry.getVertexAttribArray(i) != nullptr
                && !((i == 7 && materials != nullptr) || (i == 8 && skin != nullptr)))
                return false;

        for (unsigned int i = 0; i < geometry.getNumPrimitiveSets(); ++i)
            if (!appendTriangles(*geometry.getPrimitiveSet(i), indices))
                return false;
        if (indices.empty())
            return false;

        vertexBytes.resize(count * sizeof(PackedVertex));
        PackedVertex* out = reinterpret_cast<PackedVertex*>(vertexBytes.data());
        for (std::size_t i = 0; i < count; ++i)
        {
            PackedVertex& v = out[i];
            std::memset(&v, 0, sizeof(PackedVertex));
            v.mMaterial = materials != nullptr ? (*materials)[i] : 0;
            if (skin != nullptr)
            {
                v.mPalette = (*skin)[i].x();
                v.mGroup = (*skin)[i].y();
            }
            const osg::Vec3f& p = (*positions)[i];
            const osg::Vec3f n = normals != nullptr ? (*normals)[i] : overallNormal;
            v.mPosition[0] = p.x();
            v.mPosition[1] = p.y();
            v.mPosition[2] = p.z();
            v.mNormal[0] = n.x();
            v.mNormal[1] = n.y();
            v.mNormal[2] = n.z();
            osg::Vec4f c = overallColor;
            if (colors != nullptr)
                c = (*colors)[i];
            else if (colorsUb != nullptr)
            {
                const osg::Vec4ub& cub = (*colorsUb)[i];
                c = osg::Vec4f(cub.r() / 255.f, cub.g() / 255.f, cub.b() / 255.f, cub.a() / 255.f);
            }
            for (int k = 0; k < 4; ++k)
                v.mColor[k] = static_cast<unsigned char>(std::clamp(c[k], 0.f, 1.f) * 255.f + 0.5f);
            for (int unit = 0; unit < 7; ++unit)
            {
                if (uv[unit] == nullptr)
                    continue;
                const osg::Vec2f& t = (*uv[unit])[i];
                v.mUv[unit][0] = t.x();
                v.mUv[unit][1] = t.y();
            }
            if (tangents != nullptr)
            {
                const osg::Vec4f& t = (*tangents)[i];
                for (int k = 0; k < 4; ++k)
                    v.mTexCoord7[k] = t[k];
            }
            else if (uv[7] != nullptr)
            {
                const osg::Vec2f& t = (*uv[7])[i];
                v.mTexCoord7[0] = t.x();
                v.mTexCoord7[1] = t.y();
            }
        }
        return true;
    }

    GeometryBatch::GeometryBatch()
    {
        mGeometry->setVertexArray(new osg::Vec3Array);
        mGeometry->setNormalArray(new osg::Vec3Array, osg::Array::BIND_PER_VERTEX);
        mGeometry->setColorArray(new osg::Vec4ubArray, osg::Array::BIND_PER_VERTEX);
        for (unsigned int unit = 0; unit < 7; ++unit)
            mGeometry->setTexCoordArray(unit, new osg::Vec2Array, osg::Array::BIND_PER_VERTEX);
        mGeometry->setTexCoordArray(7, new osg::Vec4Array, osg::Array::BIND_PER_VERTEX);
        mGeometry->setVertexAttribArray(7, new osg::UIntArray, osg::Array::BIND_PER_VERTEX);
        mGeometry->setVertexAttribArray(8, new osg::Vec2uiArray, osg::Array::BIND_PER_VERTEX);
        mGeometry->addPrimitiveSet(new osg::DrawElementsUInt(GL_TRIANGLES));
    }

    bool GeometryBatch::add(const osg::Geometry& geometry, std::uint32_t material, bool clockwise)
    {
        std::vector<std::uint8_t> bytes;
        std::vector<std::uint32_t> indices;
        if (!packGeometry(geometry, bytes, indices))
            return false;
        auto& positions = *static_cast<osg::Vec3Array*>(mGeometry->getVertexArray());
        auto& normals = *static_cast<osg::Vec3Array*>(mGeometry->getNormalArray());
        auto& colors = *static_cast<osg::Vec4ubArray*>(mGeometry->getColorArray());
        auto& materials = *static_cast<osg::UIntArray*>(mGeometry->getVertexAttribArray(7));
        auto& skin = *static_cast<osg::Vec2uiArray*>(mGeometry->getVertexAttribArray(8));
        auto& tangents = *static_cast<osg::Vec4Array*>(mGeometry->getTexCoordArray(7));
        const auto first = static_cast<std::uint32_t>(positions.size());
        const auto* vertices = reinterpret_cast<const PackedVertex*>(bytes.data());
        for (std::size_t i = 0; i < bytes.size() / sizeof(PackedVertex); ++i)
        {
            const PackedVertex& v = vertices[i];
            positions.push_back({ v.mPosition[0], v.mPosition[1], v.mPosition[2] });
            normals.push_back({ v.mNormal[0], v.mNormal[1], v.mNormal[2] });
            colors.push_back({ v.mColor[0], v.mColor[1], v.mColor[2], v.mColor[3] });
            materials.push_back(material);
            skin.push_back({ 0, 0 });
            tangents.push_back({ v.mTexCoord7[0], v.mTexCoord7[1], v.mTexCoord7[2], v.mTexCoord7[3] });
            for (unsigned int unit = 0; unit < 7; ++unit)
                static_cast<osg::Vec2Array*>(mGeometry->getTexCoordArray(unit))
                    ->push_back({ v.mUv[unit][0], v.mUv[unit][1] });
        }
        if (clockwise)
            for (std::size_t i = 0; i + 2 < indices.size(); i += 3)
                std::swap(indices[i + 1], indices[i + 2]);
        auto& output = *static_cast<osg::DrawElementsUInt*>(mGeometry->getPrimitiveSet(0));
        for (std::uint32_t index : indices)
            output.push_back(first + index);
        return true;
    }
}
