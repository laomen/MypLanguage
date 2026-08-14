#ifndef MYLANG_SYMBOL_TABLE_H
#define MYLANG_SYMBOL_TABLE_H

#include "Type.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace mylang {

/// Scoped symbol table for semantic analysis.
class SymbolTable {
public:
    SymbolTable() = default;

    void enterScope();
    void leaveScope();

    bool declare(const std::string& name, const TypeInfo& type);
    // Overwrite the binding in the current scope (create if absent). Used by
    // tuple destructure declarations so a re-declared name shadows the previous
    // one (MYP last-wins), instead of silently keeping the stale first type.
    void redeclare(const std::string& name, const TypeInfo& type);
    const TypeInfo* lookup(const std::string& name) const;

    bool isGlobalScope() const;

private:
    struct Scope {
        std::unordered_map<std::string, TypeInfo> symbols;
    };

    std::vector<Scope> scopes_;
};

} // namespace mylang

#endif // MYLANG_SYMBOL_TABLE_H
