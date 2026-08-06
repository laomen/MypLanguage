#include "mylang/Type.h"

namespace mylang {

bool TypeInfo::operator==(const TypeInfo& other) const {
    if (kind != other.kind) return false;

    switch (kind) {
        case TypeKind::Class:
            if (class_name != other.class_name) return false;
            if (type_args.size() != other.type_args.size()) return false;
            for (size_t i = 0; i < type_args.size(); i++)
                if (type_args[i] != other.type_args[i]) return false;
            return true;
        case TypeKind::Struct:
        case TypeKind::Enum:
        case TypeKind::Interface:
            return class_name == other.class_name && kind == other.kind;
        case TypeKind::Array:
            return *element_type == *other.element_type;
        case TypeKind::Function:
            return *return_type == *other.return_type &&
                   param_types == other.param_types;
        case TypeKind::Tuple:
            return tuple_types == other.tuple_types;
        default:
            return true; // basic types match by kind alone
    }
}

} // namespace mylang
