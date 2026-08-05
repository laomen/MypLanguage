#ifndef MYLANG_MACRO_H
#define MYLANG_MACRO_H

#include "AST.h"

namespace mylang {

class DiagnosticEngine;

/// M3 — declarative macro expansion.
///
/// Runs after parsing, before sema. Collects top-level `macro` declarations,
/// then iteratively expands every macro call (a CallExpr whose callee is a
/// macro name, in statement position) into the macro template body with
/// `$param` placeholders replaced by the captured argument ASTs. Expansion is
/// pure AST transformation (no execution); nested macros are expanded up to a
/// depth limit. Returns false if a diagnostic was reported.
bool expandMacros(TranslationUnit& tu, DiagnosticEngine& diag);

/// Debug helper for `--macro-expand`: print a compact AST dump of every
/// function / method body after expansion to stdout.
void dumpMacroExpandedAST(TranslationUnit& tu);

} // namespace mylang

#endif // MYLANG_MACRO_H
