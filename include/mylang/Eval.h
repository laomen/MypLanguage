#ifndef MYLANG_EVAL_H
#define MYLANG_EVAL_H

#include "AST.h"

namespace mylang {

class DiagnosticEngine;

/// M1 — @eval compile-time evaluation.
///
/// Runs after Sema, before CodeGen. For every top-level `const T name = <expr>`
/// whose initializer is a compile-time foldable expression (literals, scalar
/// arithmetic, calls to @eval functions, references to earlier consts), it
/// evaluates the expression and rewrites the const function body to
/// `return <literal>;` so codegen emits a constant instead of a runtime call.
///
/// Also validates that every @eval function is pure (no `new`, no non-@eval
/// calls, no I/O) by evaluating its body when called.
///
/// Returns false if a hard evaluation error was reported (e.g. an @eval
/// function uses a forbidden construct). Non-foldable const initializers are
/// left untouched (existing runtime behavior).
bool evaluateCompileTimeConstants(TranslationUnit& tu, DiagnosticEngine& diag);

/// M4 — execute a `@macro` proc-macro function at compile time.
/// Binds the call arguments, evaluates the macro body via the interpreter,
/// and appends the generated statements to \p out. Returns false if a
/// diagnostic was reported (e.g. body not producing an AST value).
bool evalProcMacro(TranslationUnit& tu, DiagnosticEngine& diag,
                   FuncDecl& fn,
                   const std::vector<std::unique_ptr<Expr>>& args,
                   std::vector<std::unique_ptr<Stmt>>& out);

} // namespace mylang

#endif // MYLANG_EVAL_H
