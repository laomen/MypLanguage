#ifndef MYLANG_FRONTEND_DUMP_H
#define MYLANG_FRONTEND_DUMP_H

// Deterministic, byte-comparable front-end dumps — the acceptance oracle for
// the MYP self-hosted compiler (see tools/selfhost/design.md §5.1, F0).
//
// Format contract (frozen):
//   Line 1: MYP_FRONTEND_DUMP v1
//   Line 2: (Mode <tokens|ast|sema>)
//   ...    : mode-specific records (see below)
//   (Diagnostics)         — always printed
//   (Diag line=.. col=.. sev=.. msg="...")   — sorted by line,col,msg (0..n)
//   (Result ok=0|1 errors=<n>)
//
// Determinism rules:
//   * every hash container (unordered_map/set) is SORTED before emission
//   * string values are escaped with \n \t \r \" \\  (and \0 as \0)
//   * double values use %.17g (exact round-trip)
//   * int64 values print as signed decimal

#include "AST.h"
#include "DiagnosticEngine.h"
#include "Token.h"

#include <iosfwd>
#include <string>
#include <vector>

namespace mylang {

/// Canonical, stable name for a token kind (the string the MYP side must
/// reproduce byte-for-byte). Keywords/types/operators → keywordString;
/// literals → "integer"/"long"/"uint"/"float"/"float32"/"string"/"char"/
/// "bool"/"null"; identifiers → "identifier"; EOF → "eof"; else "unknown".
std::string canonicalTokenKindName(TokenKind kind);

/// Escape a raw string deterministically (used for token values & messages).
std::string escapeDumpString(const std::string& s);

/// tokens mode: one `token <begin>:<end> <kind> "<value>"` per token.
void dumpTokens(std::ostream& os, const std::vector<Token>& tokens);

/// ast mode: indented tree of the (import-merged) translation unit.
void dumpAST(std::ostream& os, const TranslationUnit& tu, bool with_types = false);

/// sema mode: resolved AST (with ` : <type>` annotations). Diagnostics are
/// emitted by the caller (see runFrontendDump in src/main.cpp).
void dumpSema(std::ostream& os, const TranslationUnit& tu);

} // namespace mylang

#endif // MYLANG_FRONTEND_DUMP_H
