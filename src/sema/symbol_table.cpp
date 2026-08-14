#include "mylang/SymbolTable.h"

namespace mylang {

void SymbolTable::enterScope() {
    scopes_.emplace_back();
}

void SymbolTable::leaveScope() {
    if (!scopes_.empty()) {
        scopes_.pop_back();
    }
}

bool SymbolTable::declare(const std::string& name, const TypeInfo& type) {
    if (scopes_.empty()) {
        scopes_.emplace_back();
    }
    auto& current = scopes_.back();
    if (current.symbols.find(name) != current.symbols.end()) {
        return false; // already declared in this scope
    }
    current.symbols[name] = type;
    return true;
}

void SymbolTable::redeclare(const std::string& name, const TypeInfo& type) {
    if (scopes_.empty()) {
        scopes_.emplace_back();
    }
    scopes_.back().symbols[name] = type;
}

const TypeInfo* SymbolTable::lookup(const std::string& name) const {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
        auto found = it->symbols.find(name);
        if (found != it->symbols.end()) {
            return &found->second;
        }
    }
    return nullptr;
}

bool SymbolTable::isGlobalScope() const {
    return scopes_.size() <= 1;
}

} // namespace mylang
