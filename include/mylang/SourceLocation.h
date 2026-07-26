#ifndef MYLANG_SOURCE_LOCATION_H
#define MYLANG_SOURCE_LOCATION_H

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace mylang {

/// Represents a position in a source file.
struct SourcePosition {
    uint32_t line = 0;
    uint32_t column = 0;
};

/// Represents a range in a source file.
struct SourceRange {
    uint32_t begin_offset = 0;
    uint32_t end_offset = 0;
    SourcePosition begin;
    SourcePosition end;
};

/// Manages source file content and provides location tracking.
class SourceManager {
public:
    SourceManager() = default;

    /// Load a source file. Returns false if file cannot be opened.
    bool loadFile(const std::string& filename);

    /// Load from a string (for LSP).
    bool loadString(const std::string& content, const std::string& filename = "<unknown>");

    /// Get the filename of the loaded source.
    const std::string& filename() const { return filename_; }

    /// Get the full source text.
    const std::string& source() const { return source_; }

    /// Convert offset to line:column position.
    SourcePosition positionFromOffset(uint32_t offset) const;

    /// Get a line of source text (1-indexed).
    std::string_view lineText(uint32_t line) const;

    /// Number of lines in the source.
    uint32_t lineCount() const { return line_count_; }

private:
    std::string filename_;
    std::string source_;
    uint32_t line_count_ = 0;
    // Cache for fast offset→position lookup
    mutable bool positions_built_ = false;
    mutable std::vector<uint32_t> line_offsets_;
};

} // namespace mylang

#endif // MYLANG_SOURCE_LOCATION_H
