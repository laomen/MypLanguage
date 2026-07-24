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
    Array,
    Function,
};

struct TypeInfo {
    TypeKind kind;

    // For Class types
    std::string class_name;

    // For Array types (use shared_ptr for copyability)
    std::shared_ptr<TypeInfo> element_type;

    // For Function types
    std::shared_ptr<TypeInfo> return_type;
    std::vector<TypeInfo> param_types;

    TypeInfo() : kind(TypeKind::Void) {}
    TypeInfo(TypeKind k) : kind(k) {}

    bool isBasic() const {
        return kind != TypeKind::Class &&
               kind != TypeKind::Struct &&
               kind != TypeKind::Array &&
               kind != TypeKind::Function &&
               kind != TypeKind::Void;
    }

    bool operator==(const TypeInfo& other) const;
    bool operator!=(const TypeInfo& other) const { return !(*this == other); }
};

} // namespace mylang

#endif // MYLANG_TYPE_H
