#include "mylang/SourceLocation.h"

#include <fstream>
#include <sstream>

namespace mylang {

bool SourceManager::loadFile(const std::string& filename) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        return false;
    }

    std::stringstream ss;
    ss << file.rdbuf();
    source_ = ss.str();
    filename_ = filename;

    // Count lines
    line_count_ = 1;
    for (auto c : source_) {
        if (c == '\n') ++line_count_;
    }

    return true;
}

bool SourceManager::loadString(const std::string& content, const std::string& filename) {
    source_ = content;
    filename_ = filename;
    line_count_ = 1;
    for (auto c : source_) {
        if (c == '\n') ++line_count_;
    }
    positions_built_ = false;
    line_offsets_.clear();
    return true;
}

SourcePosition SourceManager::positionFromOffset(uint32_t offset) const {
    if (!positions_built_) {
        // Build line offset cache
        line_offsets_.clear();
        line_offsets_.push_back(0);
        for (size_t i = 0; i < source_.size(); ++i) {
            if (source_[i] == '\n') {
                line_offsets_.push_back(static_cast<uint32_t>(i + 1));
            }
        }
        positions_built_ = true;
    }

    SourcePosition pos;
    pos.line = 1;
    for (size_t i = 0; i < line_offsets_.size(); ++i) {
        if (line_offsets_[i] > offset) break;
        pos.line = static_cast<uint32_t>(i + 1);
    }

    if (pos.line > 0 && pos.line <= line_offsets_.size()) {
        pos.column = offset - line_offsets_[pos.line - 1] + 1;
    }

    return pos;
}

std::string_view SourceManager::lineText(uint32_t line) const {
    if (line == 0 || line > line_count_) return {};

    if (!positions_built_) {
        positionFromOffset(0); // trigger cache build
    }

    size_t start = (line - 1) < line_offsets_.size()
        ? line_offsets_[line - 1] : 0;
    size_t end = source_.find('\n', start);
    if (end == std::string::npos) {
        end = source_.size();
    }

    return std::string_view(source_).substr(start, end - start);
}

} // namespace mylang
