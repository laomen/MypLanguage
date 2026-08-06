#ifndef MYLANG_TYPE_H
#define MYLANG_TYPE_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace mylang {

enum class TypeKind : uint8_t {
    Byte, Short, Int, Long,
    UByte, UShort, UInt, ULong,
    Char,
    Float, Double,
    Bool,
    String,
    Void,
    Null,
    Class,
    Struct,
    Enum,
    Interface,
    Array,
    Slice,
    Function,
    Tuple,
    Assoc,   // 抽象关联类型（接口 type Item;）——静态检查与任意类型兼容，运行时经实现类绑定
};

struct TypeInfo {
    TypeKind kind;

    // For Class types
    std::string class_name;
    std::vector<TypeInfo> type_args; // generic type arguments

    // For Array types (use shared_ptr for copyability)
    std::shared_ptr<TypeInfo> element_type;
    int array_size = 0; // >0 for fixed-size arrays like int[200]

    // For Function types
    std::shared_ptr<TypeInfo> return_type;
    std::vector<TypeInfo> param_types;
    std::vector<bool> param_is_ref;

    // For Tuple types
    std::vector<TypeInfo> tuple_types;

    TypeInfo() : kind(TypeKind::Void) {}
    TypeInfo(TypeKind k) : kind(k) {}

    bool isBasic() const {
        return kind != TypeKind::Class &&
               kind != TypeKind::Struct &&
               kind != TypeKind::Array &&
               kind != TypeKind::Function &&
               kind != TypeKind::Tuple &&
               kind != TypeKind::Void;
    }

    bool operator==(const TypeInfo& other) const;
    bool operator!=(const TypeInfo& other) const { return !(*this == other); }
};

} // namespace mylang

#endif // MYLANG_TYPE_H
