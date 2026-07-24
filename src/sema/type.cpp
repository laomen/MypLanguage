#include "mylang/Type.h"

namespace mylang {

bool TypeInfo::operator==(const TypeInfo& other) const {
    if (kind != other.kind) return false;

    switch (kind) {
        case TypeKind::Class:
            return class_name == other.class_name;
        case TypeKind::Array:
            return *element_type == *other.element_type;
        case TypeKind::Function:
            return *return_type == *other.return_type &&
                   param_types == other.param_types;
        default:
            return true; // basic types match by kind alone
    }
}

} // namespace mylang
