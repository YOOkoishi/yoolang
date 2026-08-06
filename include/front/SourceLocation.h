#pragma once

#include <cstdint>

namespace front {

using SourceFileId = std::uint32_t;

// Source locations use one-based line/column coordinates and a zero-based byte
// offset. Source file id zero is reserved for an invalid/unknown location.
struct SourceLocation {
    SourceFileId file_id = 0;
    std::uint64_t byte_offset = 0;
    std::uint32_t line = 0;
    std::uint32_t column = 0;

    constexpr SourceLocation() = default;
    constexpr SourceLocation(SourceFileId file, std::uint64_t offset, std::uint32_t line_number,
                             std::uint32_t column_number)
        : file_id(file), byte_offset(offset), line(line_number), column(column_number) {
    }

    constexpr bool valid() const {
        return file_id != 0 && line != 0 && column != 0;
    }
};

constexpr bool operator==(const SourceLocation &lhs, const SourceLocation &rhs) {
    return lhs.file_id == rhs.file_id && lhs.byte_offset == rhs.byte_offset &&
           lhs.line == rhs.line && lhs.column == rhs.column;
}

constexpr bool operator!=(const SourceLocation &lhs, const SourceLocation &rhs) {
    return !(lhs == rhs);
}

// Ranges are half-open: begin is included and end is excluded. A zero-width
// range is valid and is useful for diagnostics at an insertion point.
struct SourceRange {
    SourceLocation begin;
    SourceLocation end;

    constexpr SourceRange() = default;
    constexpr SourceRange(SourceLocation range_begin, SourceLocation range_end)
        : begin(range_begin), end(range_end) {
    }

    static constexpr SourceRange point(SourceLocation location) {
        return SourceRange(location, location);
    }

    constexpr bool valid() const {
        return begin.valid() && end.valid() && begin.file_id == end.file_id &&
               begin.byte_offset <= end.byte_offset;
    }
};

constexpr bool operator==(const SourceRange &lhs, const SourceRange &rhs) {
    return lhs.begin == rhs.begin && lhs.end == rhs.end;
}

constexpr bool operator!=(const SourceRange &lhs, const SourceRange &rhs) {
    return !(lhs == rhs);
}

} // namespace front
