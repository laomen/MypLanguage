#ifndef MYLANG_DIAGNOSTIC_ENGINE_H
#define MYLANG_DIAGNOSTIC_ENGINE_H

#include "SourceLocation.h"

#include <cstdint>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

namespace mylang {

struct Diagnostic {
    SourceRange range;
    std::string message;
    int severity; // 1=error, 2=warning, 3=info
};

enum class Severity : uint8_t {
    Note,
    Warning,
    Error,
    Fatal,
};

class DiagnosticEngine {
public:
    DiagnosticEngine(SourceManager& source_mgr)
        : source_mgr_(source_mgr) {}

    /// Report a diagnostic at a specific source location.
    void report(SourceRange range, Severity severity, const std::string& message) {
        printDiagnostic(range, severity, message);
        if (severity == Severity::Error || severity == Severity::Fatal) {
            ++error_count_;
        }
    }

    /// Convenience: report an error.
    void error(SourceRange range, const std::string& message) {
        report(range, Severity::Error, message);
    }

    /// Convenience: report a warning.
    void warn(SourceRange range, const std::string& message) {
        report(range, Severity::Warning, message);
    }

    /// Convenience: report a note.
    void note(SourceRange range, const std::string& message) {
        report(range, Severity::Note, message);
    }

    /// Return true if any errors (or fatals) were reported.
    bool hasErrors() const { return error_count_ > 0; }

    /// Number of errors reported.
    uint32_t errorCount() const { return error_count_; }

    /// Reset error count.
    void reset() { error_count_ = 0; diagnostics_.clear(); }

    /// Get all diagnostics (for LSP).
    const std::vector<Diagnostic>& getDiagnostics() const { return diagnostics_; }

    /// Get all errors (legacy).
    const std::vector<Diagnostic>& getErrors() const { return diagnostics_; }

private:
    void printDiagnostic(SourceRange range, Severity severity, const std::string& message) {
        diagnostics_.push_back({range, message, severity == Severity::Error ? 1 : severity == Severity::Warning ? 2 : 3});

        const auto& filename = source_mgr_.filename();
        auto line = range.begin.line;
        auto col = range.begin.column;

        // Severity label
        std::string severity_label;
        switch (severity) {
            case Severity::Note:    severity_label = "note";    break;
            case Severity::Warning: severity_label = "warning"; break;
            case Severity::Error:   severity_label = "error";   break;
            case Severity::Fatal:   severity_label = "fatal";   break;
        }

        // Primary message: file:line:col: severity: message
        std::cerr << filename << ":" << line << ":" << col << ": "
                  << severity_label << ": " << message << "\n";

        // Source line with caret
        if (line > 0 && line <= source_mgr_.lineCount()) {
            auto line_text = source_mgr_.lineText(line);
            std::cerr << "  " << line_text << "\n";
            std::cerr << "  " << std::string(col > 0 ? col - 1 : 0, ' ') << "^\n";
        }
    }

    SourceManager& source_mgr_;
    uint32_t error_count_ = 0;
    std::vector<Diagnostic> diagnostics_;
};

} // namespace mylang

#endif // MYLANG_DIAGNOSTIC_ENGINE_H
