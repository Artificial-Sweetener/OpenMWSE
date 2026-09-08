#ifndef COMPONENTS_STD140_UBO_H
#define COMPONENTS_STD140_UBO_H

#include <osg/Matrixf>
#include <osg/Vec2f>
#include <osg/Vec4f>

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <tuple>

namespace Std140
{
    /// Align a column-major matrix like its vec4 columns, including after arrays and scalar fields.
    struct Mat4
    {
        using Value = osg::Matrixf;
        Value mValue;
        static constexpr size_t sAlign = sizeof(osg::Vec4f);
        static constexpr std::string_view sTypeName = "mat4";
    };

    struct Vec4
    {
        using Value = osg::Vec4f;
        Value mValue;
        static constexpr size_t sAlign = sizeof(Value);
        static constexpr std::string_view sTypeName = "vec4";
    };

    struct Vec3
    {
        using Value = osg::Vec3f;
        Value mValue;
        static constexpr std::size_t sAlign = 4 * sizeof(osg::Vec3f::value_type);
        static constexpr std::string_view sTypeName = "vec3";
    };

    struct Vec2
    {
        using Value = osg::Vec2f;
        Value mValue;
        static constexpr std::size_t sAlign = sizeof(Value);
        static constexpr std::string_view sTypeName = "vec2";
    };

    struct Float
    {
        using Value = float;
        Value mValue;
        static constexpr std::size_t sAlign = sizeof(Value);
        static constexpr std::string_view sTypeName = "float";
    };

    struct Int
    {
        using Value = std::int32_t;
        Value mValue;
        static constexpr std::size_t sAlign = sizeof(Value);
        static constexpr std::string_view sTypeName = "int";
    };

    struct UInt
    {
        using Value = std::uint32_t;
        Value mValue;
        static constexpr std::size_t sAlign = sizeof(Value);
        static constexpr std::string_view sTypeName = "uint";
    };

    struct Bool
    {
        using Value = std::int32_t;
        Value mValue;
        static constexpr std::size_t sAlign = sizeof(Value);
        static constexpr std::string_view sTypeName = "bool";
    };

    /// Identify arrays whose elements already occupy complete std140 sixteen-byte slots.
    struct ArrayTag
    {
    };

    /// Store fixed-length vec4 arrays with the same element stride on the CPU and in GLSL.
    template <std::size_t Size>
    struct Vec4Array : ArrayTag
    {
        using Value = std::array<osg::Vec4f, Size>;
        Value mValue;
        static constexpr std::size_t sAlign = sizeof(osg::Vec4f);
        static constexpr std::size_t sArraySize = Size;
        static constexpr std::string_view sTypeName = "vec4";
    };

    template <class... CArgs>
    class UBO
    {
    private:
        template <typename T, typename... Args>
        struct contains : std::bool_constant<(std::is_base_of_v<Args, T> || ...)>
        {
        };

        static_assert((contains<CArgs, Mat4, Vec4, Vec3, Vec2, Float, Int, UInt, Bool, ArrayTag>() && ...));

        /// Include the GLSL array extent while keeping the public member name available for uniform lookup.
        template <class T>
        static std::string getMemberDeclaration()
        {
            std::string declaration = "    " + std::string(T::sTypeName) + " " + std::string(T::sName);
            if constexpr (std::is_base_of_v<ArrayTag, T>)
                declaration += "[" + std::to_string(T::sArraySize) + "]";
            return declaration + ";\n";
        }

        static constexpr size_t roundUpRemainder(size_t x, size_t multiple)
        {
            size_t remainder = x % multiple;
            if (remainder == 0)
                return 0;
            return multiple - remainder;
        }

        template <class T>
        static constexpr std::size_t getOffset()
        {
            bool found = false;
            std::size_t size = 0;
            ((found = found || std::is_same_v<T, CArgs>,
                 size += (found ? 0 : sizeof(typename CArgs::Value) + roundUpRemainder(size, CArgs::sAlign))),
                ...);
            return size + roundUpRemainder(size, T::sAlign);
        }

    public:
        static constexpr size_t getGPUSize()
        {
            std::size_t size = 0;
            ((size += (sizeof(typename CArgs::Value) + roundUpRemainder(size, CArgs::sAlign))), ...);
            return size;
        }

        static std::string getDefinition(const std::string& name)
        {
            std::string structDefinition = "struct " + name + " {\n";
            ((structDefinition += getMemberDeclaration<CArgs>()), ...);
            return structDefinition + "};";
        }

        using BufferType = std::array<char, getGPUSize()>;
        using TupleType = std::tuple<CArgs...>;

        /// Read one typed value from an already published std140 buffer without aliasing packed storage.
        template <class T>
        static typename T::Value read(const BufferType& buffer)
        {
            static_assert((std::is_same_v<T, CArgs> || ...));
            typename T::Value value;
            std::memcpy(&value, buffer.data() + getOffset<T>(), sizeof(value));
            return value;
        }

        template <class T>
        typename T::Value& get()
        {
            return std::get<T>(mData).mValue;
        }

        template <class T>
        const typename T::Value& get() const
        {
            return std::get<T>(mData).mValue;
        }

        void copyTo(BufferType& buffer) const
        {
            const auto copy = [&](const auto& v) {
                static_assert(std::is_standard_layout_v<std::decay_t<decltype(v.mValue)>>);
                constexpr std::size_t offset = getOffset<std::decay_t<decltype(v)>>();
                std::memcpy(buffer.data() + offset, &v.mValue, sizeof(v.mValue));
            };

            std::apply([&](const auto&... v) { (copy(v), ...); }, mData);
        }

        const auto& getData() const { return mData; }

    private:
        std::tuple<CArgs...> mData;
    };
}

#endif
