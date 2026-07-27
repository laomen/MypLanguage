#ifndef MYLANG_FMT_H
#define MYLANG_FMT_H

#include "DiagnosticEngine.h"
#include "SourceLocation.h"
#include "Token.h"

#include <string>
#include <vector>

namespace mylang {

/// MYP source code formatter.
class Formatter {
public:
    Formatter(SourceManager& source_mgr, DiagnosticEngine& diag);

    /// Format the source file. Returns the formatted source text.
    std::string format();

private:
    /// Formatting style
    static constexpr int indent_size_ = 4;

    /// Write indentation.
    std::string indentStr(int level) const;

    /// Decide what spacing should precede a token based on context.
    /// Returns: "\n{indent}", " ", or "".
    std::string spacingBefore(const Token& tok, int indent_level,
                              const Token* prev_tok, bool prev_was_newline);

    /// Skip whitespace and comments in source at a given offset, returning the
    /// text (comments only, whitespace discarded).
    std::string extractComments(const std::string& src, uint32_t& offset,
                                uint32_t end_offset);

    SourceManager& source_mgr_;
    DiagnosticEngine& diag_;
    const std::string& source_;
};

} // namespace mylang

#endif // MYLANG_FMT_H
