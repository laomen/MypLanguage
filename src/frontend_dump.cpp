// frontend_dump.cpp — deterministic front-end dump (self-hosting oracle).
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 MYP Language authors
// See LICENSE for the full MIT license text.
//
// Implements the frozen format contract (see include/mylang/FrontendDump.h).
// Wired into mypc via `--frontend-dump <tokens|ast|sema>` (see src/main.cpp).
//
// Determinism: all unordered containers are sorted before emission; doubles use
// %.17g; strings are escaped; int64 prints as signed decimal.

#include "mylang/FrontendDump.h"

#include <algorithm>
#include <cstdio>
#include <iomanip>
#include <ostream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace mylang {

// ---------------------------------------------------------------------------
// Escaping & formatting helpers
// ---------------------------------------------------------------------------

std::string escapeDumpString(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            case '\r': out += "\\r"; break;
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\0': out += "\\0"; break;
            default:   out += c; break;
        }
    }
    return out;
}

static std::string dumpDouble(double v) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%.17g", v);
    return buf;
}

static std::string dumpInt(int64_t v) {
    return std::to_string(v);
}

std::string canonicalTokenKindName(TokenKind kind) {
    switch (kind) {
        case TokenKind::IntegerLiteral:   return "integer";
        case TokenKind::LongLiteral:      return "long";
        case TokenKind::UIntLiteral:      return "uint";
        case TokenKind::FloatLiteral:     return "float";
        case TokenKind::FloatLiteral32:   return "float32";
        case TokenKind::StringLiteral:    return "string";
        case TokenKind::CharLiteral:      return "char";
        case TokenKind::BoolLiteral:      return "bool";
        case TokenKind::NullLiteral:      return "null";
        case TokenKind::Identifier:       return "identifier";
        case TokenKind::EndOfFile:        return "eof";
        case TokenKind::Unknown:          return "unknown";
        default: {
            const char* kw = Token::keywordString(kind);
            if (kw) return std::string(kw);
            return "unknown";
        }
    }
}

// ---------------------------------------------------------------------------
// Type string (readable, deterministic)
// ---------------------------------------------------------------------------

static std::string typeNodeToString(const TypeNode& t) {
    if (t.element_type) {
        std::string s = typeNodeToString(*t.element_type) + "[";
        if (t.array_size > 0) s += std::to_string(t.array_size);
        s += "]";
        return s;
    }
    if (!t.class_name.empty()) {
        std::string s = t.class_name;
        if (!t.type_args.empty()) {
            s += "<";
            for (size_t i = 0; i < t.type_args.size(); i++) {
                if (i) s += ",";
                s += typeNodeToString(t.type_args[i]);
            }
            s += ">";
        }
        return s;
    }
    if (t.is_generic_param) return "$T";
    if (t.func_return_type) {
        std::string s = "(";
        for (size_t i = 0; i < t.func_param_types.size(); i++) {
            if (i) s += ",";
            s += typeNodeToString(t.func_param_types[i]);
        }
        s += ")->" + typeNodeToString(*t.func_return_type);
        return s;
    }
    if (t.is_tuple) {
        std::string s = "(";
        for (size_t i = 0; i < t.func_param_types.size(); i++) {
            if (i) s += ",";
            s += typeNodeToString(t.func_param_types[i]);
        }
        s += ")";
        return s;
    }
    switch (t.basic_type) {
        case BuiltinType::Byte:      return "byte";
        case BuiltinType::Short:     return "short";
        case BuiltinType::Int:       return "int";
        case BuiltinType::Long:      return "long";
        case BuiltinType::UByte:     return "ubyte";
        case BuiltinType::UShort:    return "ushort";
        case BuiltinType::UInt:      return "uint";
        case BuiltinType::ULong:     return "ulong";
        case BuiltinType::Char:      return "char";
        case BuiltinType::Float:     return "float";
        case BuiltinType::Double:    return "double";
        case BuiltinType::Bool:      return "bool";
        case BuiltinType::String:    return "string";
        case BuiltinType::Void:      return "void";
        case BuiltinType::Float4:    return "float4";
        case BuiltinType::Double2:   return "double2";
        case BuiltinType::Int4:      return "int4";
        case BuiltinType::Bit:       return "bit";
        case BuiltinType::BitVector: return "bitvector" + std::to_string(t.bitvector_width);
    }
    return "?";
}

static std::string typeKindToString(TypeKind k) {
    switch (k) {
        case TypeKind::Byte: return "byte";
        case TypeKind::Short: return "short";
        case TypeKind::Int: return "int";
        case TypeKind::Long: return "long";
        case TypeKind::UByte: return "ubyte";
        case TypeKind::UShort: return "ushort";
        case TypeKind::UInt: return "uint";
        case TypeKind::ULong: return "ulong";
        case TypeKind::Char: return "char";
        case TypeKind::Float: return "float";
        case TypeKind::Double: return "double";
        case TypeKind::Float4: return "float4";
        case TypeKind::Double2: return "double2";
        case TypeKind::Int4: return "int4";
        case TypeKind::Bool: return "bool";
        case TypeKind::Bit: return "bit";
        case TypeKind::BitVector: return "bitvector";
        case TypeKind::Bitfield: return "bitfield";
        case TypeKind::String: return "string";
        case TypeKind::Void: return "void";
        case TypeKind::Null: return "null";
        case TypeKind::Class: return "class";
        case TypeKind::Struct: return "struct";
        case TypeKind::Enum: return "enum";
        case TypeKind::Interface: return "interface";
        case TypeKind::Array: return "array";
        case TypeKind::Slice: return "slice";
        case TypeKind::Function: return "function";
        case TypeKind::Tuple: return "tuple";
        case TypeKind::Assoc: return "assoc";
    }
    return "?";
}

// ---------------------------------------------------------------------------
// AST dumper
// ---------------------------------------------------------------------------

namespace {

class Dumper {
public:
    Dumper(std::ostream& os, bool with_types) : os_(os), with_types_(with_types) {}

    // ---- Declarations ----
    void tu(const TranslationUnit& t) {
        line("(TranslationUnit)");
        indent_ += 2;
        for (auto& i : t.imports)       line("(Import name=\"" + escapeDumpString(i.module_name)
                                            + "\" path=\"" + escapeDumpString(i.file_path)
                                            + "\" isPath=" + std::to_string(i.is_path) + ")");
        for (auto& s : t.structs)       structDecl(s);
        for (auto& b : t.bitfields)     bitfieldDecl(b);
        for (auto& c : t.classes)       classDecl(c);
        for (auto& i : t.interfaces)    interfaceDecl(i);
        for (auto& m : t.mappings)      mappingDecl(m);
        for (auto& f : t.functions)     funcDecl(f);
        for (auto& e : t.enums)         enumDecl(e);
        for (auto& f : t.ffis)          ffiDecl(f);
        for (auto& m : t.macros)        macroDecl(m);
        for (auto& a : t.type_aliases)  line("(TypeAlias name=\"" + escapeDumpString(a.name)
                                            + "\" type=" + typeNodeToString(a.alias_type) + ")");
        indent_ -= 2;
    }

    void typeNode(const std::string& tag, const TypeNode& t) {
        line("(" + tag + " type=" + typeNodeToString(t) + ")");
    }

    void param(const ParamDecl& p) {
        std::string s = "(Param name=\"" + escapeDumpString(p.name) + "\" type="
                      + typeNodeToString(p.type) + " ref=" + std::to_string(p.is_ref);
        if (p.default_expr) s += " hasDefault=1";
        s += ")";
        line(s);
        if (p.default_expr) expr(*p.default_expr);
    }

    void varDecl(const VarDecl& d) {
        std::string s = "(Var name=\"" + escapeDumpString(d.name) + "\" type="
                      + typeNodeToString(d.type) + " const=" + std::to_string(d.is_const)
                      + " thread=" + std::to_string(d.has_thread_annotation)
                      + " threadpool=" + std::to_string(d.has_threadpool_annotation) + ")";
        line(s);
        if (d.init_expr) expr(*d.init_expr);
    }

    void property(const PropertyDecl& p) {
        std::string s = "(Property name=\"" + escapeDumpString(p.name) + "\" type="
                      + typeNodeToString(p.type) + " const=" + std::to_string(p.is_const)
                      + " weak=" + std::to_string(p.weak) + ")";
        line(s);
        if (p.init_expr) expr(*p.init_expr);
    }

    void action(const ActionDecl& a) {
        std::string s = "(Action name=\"" + escapeDumpString(a.name) + "\" ret="
                      + typeNodeToString(a.return_type)
                      + " startup=" + std::to_string(a.has_startup)
                      + " ctor=" + std::to_string(a.has_constructor)
                      + " test=" + std::to_string(a.has_test)
                      + " coro=" + std::to_string(a.has_coro)
                      + " async=" + std::to_string(a.has_async)
                      + " region=" + std::to_string(a.has_region)
                      + " stack=" + std::to_string(a.coro_stack_kb) + ")";
        line(s);
        indent_ += 2;
        for (auto& tp : a.type_params) line("(TypeParam name=\"" + escapeDumpString(tp) + "\")");
        for (auto& tc : sortedPairs(a.type_param_constraints))
            line("(Constraint name=\"" + escapeDumpString(tc.first)
                 + "\" iface=\"" + escapeDumpString(tc.second) + "\")");
        for (auto& p : a.params) param(p);
        if (a.body) stmt(*a.body);
        indent_ -= 2;
    }

    void event(const EventDecl& e) {
        std::string s = "(Event name=\"" + escapeDumpString(e.name) + "\")";
        line(s);
        indent_ += 2;
        for (auto& p : e.params) param(p);
        indent_ -= 2;
    }

    void structDecl(const StructDecl& s) {
        std::string h = "(Struct name=\"" + escapeDumpString(s.name)
                      + "\" parent=\"" + escapeDumpString(s.parent_class) + "\")";
        line(h);
        indent_ += 2;
        for (auto& p : s.properties) property(p);
        for (auto& f : s.functions) funcDecl(f);
        indent_ -= 2;
    }

    void bitfieldDecl(const BitfieldDecl& b) {
        line("(Bitfield name=\"" + escapeDumpString(b.name)
             + "\" totalBits=" + std::to_string(b.total_bits) + ")");
        indent_ += 2;
        for (auto& f : b.fields)
            line("(Field name=\"" + escapeDumpString(f.name)
                 + "\" width=" + std::to_string(f.bit_width)
                 + " offset=" + std::to_string(f.offset) + ")");
        indent_ -= 2;
    }

    void classDecl(const ClassDecl& c) {
        std::string s = "(Class name=\"" + escapeDumpString(c.name)
                      + "\" isStatic=" + std::to_string(c.is_static)
                      + " isGenericInst=" + std::to_string(c.is_generic_inst)
                      + " lambda=" + escapeDumpString(c.lambda_name)
                      + " iface=\"" + escapeDumpString(c.interface_class_name) + "\")";
        line(s);
        indent_ += 2;
        for (auto& tp : c.type_params) line("(TypeParam name=\"" + escapeDumpString(tp) + "\")");
        for (auto& tc : sortedPairs(c.type_param_constraints))
            line("(Constraint name=\"" + escapeDumpString(tc.first)
                 + "\" iface=\"" + escapeDumpString(tc.second) + "\")");
        for (auto& ta : c.inst_type_args) typeNode("InstTypeArg", ta);
        for (auto& ab : sortedPairs(c.associated_type_bindings))
            line("(AssocType name=\"" + escapeDumpString(ab.first)
                 + "\" type=" + typeNodeToString(ab.second) + ")");
        for (auto& ns : c.nonlocal_slots)
            line("(NonlocalSlot slot=\"" + escapeDumpString(ns.slot)
                 + "\" var=\"" + escapeDumpString(ns.var)
                 + "\" cell=\"" + escapeDumpString(ns.cell_class) + "\")");
        for (auto& a : c.actions) action(a);
        for (auto& a : c.static_actions) { line("(Static)") ; indent_ += 2; action(a); indent_ -= 2; }
        for (auto& e : c.events) event(e);
        for (auto& p : c.properties) property(p);
        for (auto& f : c.functions) funcDecl(f);
        for (auto& s : c.structs) structDecl(s);
        indent_ -= 2;
    }

    void interfaceDecl(const InterfaceDecl& i) {
        line("(Interface name=\"" + escapeDumpString(i.name) + "\")");
        indent_ += 2;
        for (auto& at : i.associated_types) line("(AssocTypeDecl name=\"" + escapeDumpString(at) + "\")");
        for (auto& a : i.actions) action(a);
        for (auto& e : i.events) event(e);
        indent_ -= 2;
    }

    void funcDecl(const FuncDecl& f) {
        std::string s = "(Function name=\"" + escapeDumpString(f.name) + "\" ret="
                      + typeNodeToString(f.return_type)
                      + " genericInst=" + std::to_string(f.is_generic_inst)
                      + " test=" + std::to_string(f.has_test)
                      + " autoMain=" + std::to_string(f.is_auto_main)
                      + " region=" + std::to_string(f.has_region)
                      + " coro=" + std::to_string(f.has_coro)
                      + " async=" + std::to_string(f.has_async)
                      + " stack=" + std::to_string(f.coro_stack_kb)
                      + " op=\"" + escapeDumpString(f.op_symbol) + "\""
                      + " eval=" + std::to_string(f.has_eval)
                      + " macro=" + std::to_string(f.has_proc_macro)
                      + " ctor=" + std::to_string(f.has_constructor)
                      + " constDecl=" + std::to_string(f.is_const_decl) + ")";
        line(s);
        indent_ += 2;
        for (auto& tp : f.type_params) line("(TypeParam name=\"" + escapeDumpString(tp) + "\")");
        for (auto& tc : sortedPairs(f.type_param_constraints))
            line("(Constraint name=\"" + escapeDumpString(tc.first)
                 + "\" iface=\"" + escapeDumpString(tc.second) + "\")");
        for (auto& ta : f.inst_type_args) typeNode("InstTypeArg", ta);
        for (auto& p : f.params) param(p);
        if (f.body) stmt(*f.body);
        indent_ -= 2;
    }

    void enumDecl(const EnumDecl& e) {
        line("(Enum name=\"" + escapeDumpString(e.name) + "\")");
        indent_ += 2;
        for (auto& v : e.variants) {
            line("(Variant name=\"" + escapeDumpString(v.name) + "\")");
            indent_ += 2;
            for (auto& p : v.params) param(p);
            indent_ -= 2;
        }
        indent_ -= 2;
    }

    void ffiDecl(const FFIDecl& f) {
        line("(FFI name=\"" + escapeDumpString(f.name) + "\" ret="
             + typeNodeToString(f.return_type) + ")");
        indent_ += 2;
        for (auto& p : f.params) param(p);
        indent_ -= 2;
    }

    void mappingDecl(const MappingDecl& m) {
        line("(Mapping scope=" + std::to_string(m.has_scope) + ")");
        indent_ += 2;
        for (auto& ch : m.chains) {
            line("(Chain)");
            indent_ += 2;
            for (auto& n : ch.nodes) {
                std::string s = "(Node src=\"" + escapeDumpString(n.source_name)
                              + "\" member=\"" + escapeDumpString(n.member_name)
                              + "\" isFunction=" + std::to_string(n.is_function)
                              + " isLambda=" + std::to_string(n.is_lambda)
                              + " isTransformer=" + std::to_string(n.is_transformer)
                              + " trKind=" + std::to_string(n.transformer_kind)
                              + " trParam=" + std::to_string(n.transformer_param) + ")";
                line(s);
                if (n.lambda) expr(*n.lambda);
            }
            if (ch.where_expr) { line("(Where)"); indent_ += 2; expr(*ch.where_expr); indent_ -= 2; }
            indent_ -= 2;
        }
        indent_ -= 2;
    }

    void macroDecl(const MacroDecl& m) {
        line("(Macro name=\"" + escapeDumpString(m.name) + "\")");
        indent_ += 2;
        for (auto& p : m.params) line("(Param name=\"" + escapeDumpString(p) + "\")");
        if (m.body) stmt(*m.body);
        indent_ -= 2;
    }

    // ---- Statements ----
    void stmt(const Stmt& s) {
        switch (s.kind) {
        case StmtKind::Block: {
            line("(Block)");
            indent_ += 2;
            for (auto& st : static_cast<const BlockStmt&>(s).statements)
                if (st) stmt(*st);
            indent_ -= 2;
            break;
        }
        case StmtKind::VarDeclStmt: {
            auto& v = static_cast<const VarDeclStmt&>(s);
            line("(VarDecl)");
            indent_ += 2;
            for (auto& d : v.decls) varDecl(d);
            indent_ -= 2;
            break;
        }
        case StmtKind::DestructureStmt: {
            auto& v = static_cast<const DestructureStmt&>(s);
            line("(Destructure isDecl=" + std::to_string(v.is_decl) + ")");
            indent_ += 2;
            destructureTarget(v.target);
            if (v.value) expr(*v.value);
            indent_ -= 2;
            break;
        }
        case StmtKind::ExprStmt: {
            line("(ExprStmt)");
            indent_ += 2;
            if (static_cast<const ExprStmt&>(s).expression)
                expr(*static_cast<const ExprStmt&>(s).expression);
            indent_ -= 2;
            break;
        }
        case StmtKind::IfStmt: {
            auto& v = static_cast<const IfStmt&>(s);
            line("(If)");
            indent_ += 2;
            if (v.condition) expr(*v.condition);
            if (v.then_block) stmt(*v.then_block);
            if (v.else_block) stmt(*v.else_block);
            indent_ -= 2;
            break;
        }
        case StmtKind::WhileStmt: {
            auto& v = static_cast<const WhileStmt&>(s);
            line("(While)");
            indent_ += 2;
            if (v.condition) expr(*v.condition);
            if (v.body) stmt(*v.body);
            indent_ -= 2;
            break;
        }
        case StmtKind::ForStmt: {
            auto& v = static_cast<const ForStmt&>(s);
            line("(For parallel=" + std::to_string(v.parallel)
                 + " gpu=" + std::to_string(v.gpu)
                 + " stride=" + std::to_string(v.stride)
                 + " block=" + dumpInt(v.block_val) + ")");
            indent_ += 2;
            for (auto& r : v.resident)
                line("(Resident arr=\"" + escapeDumpString(r.first)
                     + "\" dev=\"" + escapeDumpString(r.second) + "\")");
            if (v.init) stmt(*v.init);
            if (v.condition) expr(*v.condition);
            if (v.step) expr(*v.step);
            if (v.stream_expr) expr(*v.stream_expr);
            if (v.body) stmt(*v.body);
            indent_ -= 2;
            break;
        }
        case StmtKind::ForInStmt: {
            auto& v = static_cast<const ForInStmt&>(s);
            line("(ForIn name=\"" + escapeDumpString(v.var_name)
                 + "\" type=" + typeNodeToString(v.var_type)
                 + " hasType=" + std::to_string(v.has_type)
                 + " iterKind=" + std::to_string(v.iter_kind)
                 + " class=\"" + escapeDumpString(v.class_name)
                 + "\" sizeFn=\"" + escapeDumpString(v.size_fn)
                 + "\" getFn=\"" + escapeDumpString(v.get_fn)
                 + "\" arrSize=" + std::to_string(v.array_size) + ")");
            indent_ += 2;
            if (v.iterable) expr(*v.iterable);
            if (v.body) stmt(*v.body);
            indent_ -= 2;
            break;
        }
        case StmtKind::GpuTileStmt: {
            auto& v = static_cast<const GpuTileStmt&>(s);
            line("(GpuTile name=\"" + escapeDumpString(v.name)
                 + "\" hasGrid=" + std::to_string(v.has_grid)
                 + " grid=" + dumpInt(v.grid_val)
                 + " block=" + dumpInt(v.block_val) + ")");
            indent_ += 2;
            typeNode("SharedType", v.shared_type);
            if (v.grid_expr) expr(*v.grid_expr);
            for (auto& r : v.resident)
                line("(Resident arr=\"" + escapeDumpString(r.first)
                     + "\" dev=\"" + escapeDumpString(r.second) + "\")");
            if (v.stream_expr) expr(*v.stream_expr);
            if (v.body) stmt(*v.body);
            indent_ -= 2;
            break;
        }
        case StmtKind::GpuReduceStmt: {
            auto& v = static_cast<const GpuReduceStmt&>(s);
            line("(GpuReduce acc=\"" + escapeDumpString(v.op_acc)
                 + "\" x=\"" + escapeDumpString(v.op_x)
                 + "\" arr=\"" + escapeDumpString(v.array_name)
                 + "\" out=\"" + escapeDumpString(v.out_name)
                 + "\" block=" + dumpInt(v.block_val) + ")");
            indent_ += 2;
            if (v.op_body) stmt(*v.op_body);
            if (v.op_expr) expr(*v.op_expr);
            if (v.init_expr) expr(*v.init_expr);
            if (v.begin_expr) expr(*v.begin_expr);
            if (v.end_expr) expr(*v.end_expr);
            indent_ -= 2;
            break;
        }
        case StmtKind::GpuScanStmt: {
            auto& v = static_cast<const GpuScanStmt&>(s);
            line("(GpuScan acc=\"" + escapeDumpString(v.op_acc)
                 + "\" x=\"" + escapeDumpString(v.op_x)
                 + "\" in=\"" + escapeDumpString(v.in_name)
                 + "\" out=\"" + escapeDumpString(v.out_name)
                 + "\" exclusive=" + std::to_string(v.exclusive)
                 + " block=" + dumpInt(v.block_val) + ")");
            indent_ += 2;
            if (v.op_body) stmt(*v.op_body);
            if (v.op_expr) expr(*v.op_expr);
            if (v.init_expr) expr(*v.init_expr);
            if (v.begin_expr) expr(*v.begin_expr);
            if (v.end_expr) expr(*v.end_expr);
            indent_ -= 2;
            break;
        }
        case StmtKind::GpuScatterStmt: {
            auto& v = static_cast<const GpuScatterStmt&>(s);
            line("(GpuScatter a=\"" + escapeDumpString(v.a_name)
                 + "\" b=\"" + escapeDumpString(v.b_name)
                 + "\" idx=\"" + escapeDumpString(v.idx_name)
                 + "\" mode=" + std::to_string(v.mode)
                 + " block=" + dumpInt(v.block_val) + ")");
            indent_ += 2;
            if (v.a_begin) expr(*v.a_begin);
            if (v.a_end) expr(*v.a_end);
            if (v.idx_begin) expr(*v.idx_begin);
            if (v.idx_end) expr(*v.idx_end);
            indent_ -= 2;
            break;
        }
        case StmtKind::ReturnStmt: {
            line("(Return)");
            indent_ += 2;
            if (static_cast<const ReturnStmt&>(s).value)
                expr(*static_cast<const ReturnStmt&>(s).value);
            indent_ -= 2;
            break;
        }
        case StmtKind::BreakStmt:    line("(Break)"); break;
        case StmtKind::ContinueStmt: line("(Continue)"); break;
        case StmtKind::AwaitStmt: {
            auto& v = static_cast<const AwaitStmt&>(s);
            line("(Await)");
            indent_ += 2;
            if (v.expr) expr(*v.expr);
            if (v.timeout) expr(*v.timeout);
            indent_ -= 2;
            break;
        }
        case StmtKind::MappingStmt: {
            line("(MappingStmt)");
            indent_ += 2;
            mappingDecl(static_cast<const MappingStmt&>(s).decl);
            indent_ -= 2;
            break;
        }
        case StmtKind::MatchStmt: {
            auto& v = static_cast<const MatchStmt&>(s);
            line("(Match)");
            indent_ += 2;
            if (v.subject) expr(*v.subject);
            for (auto& a : v.arms) {
                line("(Arm enum=\"" + escapeDumpString(a.enum_name)
                     + "\" variant=\"" + escapeDumpString(a.variant_name)
                     + "\" index=" + std::to_string(a.variant_index)
                     + " bindings=[" + joinStrings(a.bindings) + "])");
                indent_ += 2;
                if (a.body) stmt(*a.body);
                indent_ -= 2;
            }
            indent_ -= 2;
            break;
        }
        case StmtKind::TryStmt: {
            auto& v = static_cast<const TryStmt&>(s);
            line("(Try)");
            indent_ += 2;
            if (v.try_block) stmt(*v.try_block);
            for (auto& c : v.catches) {
                line("(Catch var=\"" + escapeDumpString(c.var_name)
                     + "\" type=\"" + escapeDumpString(c.var_type) + "\")");
                indent_ += 2;
                if (c.block) stmt(*c.block);
                indent_ -= 2;
            }
            if (v.finally_block) stmt(*v.finally_block);
            indent_ -= 2;
            break;
        }
        case StmtKind::ThrowStmt: {
            line("(Throw)");
            indent_ += 2;
            if (static_cast<const ThrowStmt&>(s).expr)
                expr(*static_cast<const ThrowStmt&>(s).expr);
            indent_ -= 2;
            break;
        }
        case StmtKind::NonlocalStmt: {
            auto& v = static_cast<const NonlocalStmt&>(s);
            line("(Nonlocal names=[" + joinStrings(v.names) + "])");
            break;
        }
        }
    }

    void destructureTarget(const DestructureTarget& t) {
        if (!t.elements.empty()) {
            line("(DestructureTarget)");
            indent_ += 2;
            for (auto& e : t.elements) destructureTarget(e);
            indent_ -= 2;
        } else {
            line("(DestructureLeaf name=\"" + escapeDumpString(t.name)
                 + "\" type=" + typeNodeToString(t.type)
                 + " hasType=" + std::to_string(t.has_type) + ")");
        }
    }

    // ---- Expressions ----
    void expr(const Expr& e) {
        std::string type_suffix;
        if (with_types_) {
            if (e.type) type_suffix = " : " + typeNodeToString(*e.type);
            else type_suffix = " : " + typeKindToString(e.resolved_kind);
        }
        switch (e.kind) {
        case ExprKind::IntegerLiteral: {
            auto& v = static_cast<const IntegerLiteralExpr&>(e);
            line("(Integer value=" + dumpInt(v.value)
                 + " long=" + std::to_string(v.is_long)
                 + " unsigned=" + std::to_string(v.is_unsigned)
                 + " char=" + std::to_string(v.is_char) + ")" + type_suffix);
            break;
        }
        case ExprKind::FloatLiteral: {
            auto& v = static_cast<const FloatLiteralExpr&>(e);
            line("(Float value=" + dumpDouble(v.value)
                 + " f32=" + std::to_string(v.is_f32) + ")" + type_suffix);
            break;
        }
        case ExprKind::BoolLiteral: {
            auto& v = static_cast<const BoolLiteralExpr&>(e);
            line("(Bool value=" + std::to_string(v.value) + ")" + type_suffix);
            break;
        }
        case ExprKind::StringLiteral: {
            auto& v = static_cast<const StringLiteralExpr&>(e);
            line("(String value=\"" + escapeDumpString(v.value) + "\")" + type_suffix);
            break;
        }
        case ExprKind::NullLiteral:
            line("(Null)" + type_suffix);
            break;
        case ExprKind::Identifier: {
            auto& v = static_cast<const IdentifierExpr&>(e);
            line("(Identifier name=\"" + escapeDumpString(v.name) + "\")" + type_suffix);
            break;
        }
        case ExprKind::BinaryOp: {
            auto& v = static_cast<const BinaryOpExpr&>(e);
            line("(Binary op=" + binaryOpName(v.op)
                 + " lhsUnsigned=" + std::to_string(v.lhs_unsigned)
                 + " rhsUnsigned=" + std::to_string(v.rhs_unsigned)
                 + " resultUnsigned=" + std::to_string(v.result_unsigned)
                 + " opCall=" + (v.op_call ? "1" : "0") + ")" + type_suffix);
            indent_ += 2;
            if (v.lhs) expr(*v.lhs);
            if (v.rhs) expr(*v.rhs);
            indent_ -= 2;
            break;
        }
        case ExprKind::UnaryOp: {
            auto& v = static_cast<const UnaryOpExpr&>(e);
            line("(Unary op=" + unaryOpName(v.op) + ")" + type_suffix);
            indent_ += 2;
            if (v.operand) expr(*v.operand);
            indent_ -= 2;
            break;
        }
        case ExprKind::Convert: {
            auto& v = static_cast<const ConvertExpr&>(e);
            line("(Convert to=" + typeKindToString(v.to_kind)
                 + " bw=" + std::to_string(v.to_bitvector_width) + ")" + type_suffix);
            indent_ += 2;
            if (v.operand) expr(*v.operand);
            indent_ -= 2;
            break;
        }
        case ExprKind::Call: {
            auto& v = static_cast<const CallExpr&>(e);
            std::string ta = "[";
            for (size_t i = 0; i < v.call_type_args.size(); i++) {
                if (i) ta += ",";
                ta += typeNodeToString(v.call_type_args[i]);
            }
            ta += "]";
            line("(Call typeArgs=" + ta
                 + " resolved=\"" + escapeDumpString(v.resolved_call_name)
                 + "\" structType=\"" + escapeDumpString(v.resolved_struct_type)
                 + "\" structCtor=\"" + escapeDumpString(v.resolved_struct_ctor) + "\")" + type_suffix);
            indent_ += 2;
            if (v.callee) expr(*v.callee);
            for (auto& a : v.args) if (a) expr(*a);
            indent_ -= 2;
            break;
        }
        case ExprKind::MemberAccess: {
            auto& v = static_cast<const MemberAccessExpr&>(e);
            line("(Member name=\"" + escapeDumpString(v.member_name)
                 + "\" resolvedClass=\"" + escapeDumpString(v.resolved_object_class)
                 + "\")" + type_suffix);
            indent_ += 2;
            if (v.object) expr(*v.object);
            indent_ -= 2;
            break;
        }
        case ExprKind::Subscript: {
            auto& v = static_cast<const SubscriptExpr&>(e);
            line("(Subscript)" + type_suffix);
            indent_ += 2;
            if (v.array) expr(*v.array);
            if (v.index) expr(*v.index);
            indent_ -= 2;
            break;
        }
        case ExprKind::NewExpr: {
            auto& v = static_cast<const NewExpr&>(e);
            std::string ta = "[";
            for (size_t i = 0; i < v.type_args.size(); i++) {
                if (i) ta += ",";
                ta += typeNodeToString(v.type_args[i]);
            }
            ta += "]";
            line("(New class=\"" + escapeDumpString(v.class_name)
                 + "\" typeArgs=" + ta
                 + " ctor=\"" + escapeDumpString(v.resolved_ctor) + "\")" + type_suffix);
            indent_ += 2;
            for (auto& a : v.args) if (a) expr(*a);
            indent_ -= 2;
            break;
        }
        case ExprKind::NewArrayExpr: {
            auto& v = static_cast<const NewArrayExpr&>(e);
            line("(NewArray elem=" + typeNodeToString(v.element_type) + ")" + type_suffix);
            indent_ += 2;
            for (auto& d : v.dimensions) if (d) expr(*d);
            indent_ -= 2;
            break;
        }
        case ExprKind::ThisExpr:
            line("(This)" + type_suffix);
            break;
        case ExprKind::Assignment: {
            auto& v = static_cast<const AssignmentExpr&>(e);
            line("(Assign)" + type_suffix);
            indent_ += 2;
            if (v.target) expr(*v.target);
            if (v.value) expr(*v.value);
            indent_ -= 2;
            break;
        }
        case ExprKind::Ternary: {
            auto& v = static_cast<const TernaryExpr&>(e);
            line("(Ternary)" + type_suffix);
            indent_ += 2;
            if (v.condition) expr(*v.condition);
            if (v.true_expr) expr(*v.true_expr);
            if (v.false_expr) expr(*v.false_expr);
            indent_ -= 2;
            break;
        }
        case ExprKind::Range: {
            auto& v = static_cast<const RangeExpr&>(e);
            line("(Range)" + type_suffix);
            indent_ += 2;
            if (v.start) expr(*v.start);
            if (v.end) expr(*v.end);
            indent_ -= 2;
            break;
        }
        case ExprKind::EnumVariant: {
            auto& v = static_cast<const EnumVariantExpr&>(e);
            line("(EnumVariant enum=\"" + escapeDumpString(v.enum_name)
                 + "\" index=" + std::to_string(v.variant_index) + ")" + type_suffix);
            indent_ += 2;
            for (auto& a : v.args) if (a) expr(*a);
            indent_ -= 2;
            break;
        }
        case ExprKind::Lambda: {
            auto& v = static_cast<const LambdaExpr&>(e);
            line("(Lambda name=\"" + escapeDumpString(v.name)
                 + "\" hiddenClass=\"" + escapeDumpString(v.hidden_class_name)
                 + "\" captures=[" + joinStrings(v.capture_names)
                 + "] slots=[" + joinStrings(v.capture_slots)
                 + "] nonlocal=[" + joinStrings(v.nonlocal_cells) + "])" + type_suffix);
            indent_ += 2;
            for (auto& p : v.params) param(p);
            if (v.body) stmt(*v.body);
            indent_ -= 2;
            break;
        }
        case ExprKind::Pipe: {
            auto& v = static_cast<const PipeExpr&>(e);
            line("(Pipe target=\"" + escapeDumpString(v.target_kind)
                 + "\" class=\"" + escapeDumpString(v.class_name)
                 + "\" method=\"" + escapeDumpString(v.method) + "\")" + type_suffix);
            indent_ += 2;
            if (v.lhs) expr(*v.lhs);
            if (v.rhs) expr(*v.rhs);
            indent_ -= 2;
            break;
        }
        case ExprKind::Try: {
            auto& v = static_cast<const TryExpr&>(e);
            line("(TryExpr catchVar=\"" + escapeDumpString(v.catch_var_name) + "\")" + type_suffix);
            indent_ += 2;
            if (v.try_expr) expr(*v.try_expr);
            if (v.catch_expr) expr(*v.catch_expr);
            indent_ -= 2;
            break;
        }
        case ExprKind::Await: {
            auto& v = static_cast<const AwaitExpr&>(e);
            line("(Await)" + type_suffix);
            indent_ += 2;
            if (v.operand) expr(*v.operand);
            if (v.timeout) expr(*v.timeout);
            indent_ -= 2;
            break;
        }
        case ExprKind::TupleExpr: {
            auto& v = static_cast<const TupleExpr&>(e);
            line("(Tuple)" + type_suffix);
            indent_ += 2;
            for (auto& el : v.elements) if (el) expr(*el);
            indent_ -= 2;
            break;
        }
        case ExprKind::NamedArg: {
            auto& v = static_cast<const NamedArgExpr&>(e);
            line("(NamedArg name=\"" + escapeDumpString(v.name) + "\")" + type_suffix);
            indent_ += 2;
            if (v.value) expr(*v.value);
            indent_ -= 2;
            break;
        }
        case ExprKind::MacroParam: {
            auto& v = static_cast<const MacroParamExpr&>(e);
            line("(MacroParam name=\"" + escapeDumpString(v.name) + "\")" + type_suffix);
            break;
        }
        case ExprKind::Quote: {
            auto& v = static_cast<const QuoteExpr&>(e);
            line("(Quote)" + type_suffix);
            indent_ += 2;
            if (v.body) stmt(*v.body);
            indent_ -= 2;
            break;
        }
        case ExprKind::GpuReduceExpr: {
            auto& v = static_cast<const GpuReduceExpr&>(e);
            line("(GpuReduceExpr result=" + typeKindToString(v.result_kind) + ")" + type_suffix);
            indent_ += 2;
            if (v.stmt) stmt(*v.stmt);
            indent_ -= 2;
            break;
        }
        }
    }

private:
    template <typename K, typename V>
    std::vector<std::pair<K, V>> sortedPairs(const std::unordered_map<K, V>& m) {
        std::vector<std::pair<K, V>> v(m.begin(), m.end());
        std::sort(v.begin(), v.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
        return v;
    }

    static std::string joinStrings(const std::vector<std::string>& v) {
        std::string s;
        for (size_t i = 0; i < v.size(); i++) {
            if (i) s += ",";
            s += escapeDumpString(v[i]);
        }
        return s;
    }

    static std::string binaryOpName(BinaryOpKind op) {
        switch (op) {
            case BinaryOpKind::Add: return "+";
            case BinaryOpKind::Sub: return "-";
            case BinaryOpKind::Mul: return "*";
            case BinaryOpKind::Div: return "/";
            case BinaryOpKind::Mod: return "%";
            case BinaryOpKind::Eq:  return "==";
            case BinaryOpKind::Ne:  return "!=";
            case BinaryOpKind::Lt:  return "<";
            case BinaryOpKind::Gt:  return ">";
            case BinaryOpKind::Le:  return "<=";
            case BinaryOpKind::Ge:  return ">=";
            case BinaryOpKind::And: return "&&";
            case BinaryOpKind::Or:  return "||";
            case BinaryOpKind::BitAnd: return "&";
            case BinaryOpKind::BitOr:  return "|";
            case BinaryOpKind::BitXor: return "^";
            case BinaryOpKind::Shl: return "<<";
            case BinaryOpKind::Shr: return ">>";
        }
        return "?";
    }

    static std::string unaryOpName(UnaryOpKind op) {
        switch (op) {
            case UnaryOpKind::Negate: return "-";
            case UnaryOpKind::Not:    return "!";
            case UnaryOpKind::BitNot: return "~";
        }
        return "?";
    }

    void line(const std::string& s) {
        os_ << std::string(indent_, ' ') << s << "\n";
    }

    std::ostream& os_;
    int indent_ = 0;
    bool with_types_ = false;
};

} // namespace

// ---------------------------------------------------------------------------
// Public entry points
// ---------------------------------------------------------------------------

void dumpTokens(std::ostream& os, const std::vector<Token>& tokens) {
    for (auto& t : tokens) {
        os << "token " << t.range.begin_offset << ":" << t.range.end_offset
           << " " << canonicalTokenKindName(t.kind)
           << " \"" << escapeDumpString(t.value) << "\"\n";
    }
}

void dumpAST(std::ostream& os, const TranslationUnit& tu, bool with_types) {
    Dumper d(os, with_types);
    d.tu(tu);
}

void dumpSema(std::ostream& os, const TranslationUnit& tu) {
    Dumper d(os, /*with_types=*/true);
    d.tu(tu);
}

} // namespace mylang
