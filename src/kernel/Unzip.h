// Reading files out of a zip archive, for 3MF files: stored or deflated
// entries, with SketchBillet's own inflate (RFC 1951), so sb_kernel needs
// nothing but the standard library. Private to sb_kernel.

#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sb::kernel::unzip {

/// One file in an archive.
struct Entry {
    std::string name;            // its path in the archive, like "3D/3dmodel.model"
    int method = 0;              // 0 stored, 8 deflated
    std::size_t packedSize = 0;  // bytes in the archive
    std::size_t size = 0;        // bytes when unpacked
    std::size_t dataOffset = 0;  // where its packed bytes start
};

/// Lists an archive's files. Empty, with `why` set, if it isn't a zip archive
/// SketchBillet can read (zip64, or damaged).
std::vector<Entry> list(std::string_view zip, std::string& why);

/// Unpacks one file. Nothing, with `why` set, if it's damaged, packed in a
/// way other than stored or deflated, or would unpack to more than `limit` bytes.
std::optional<std::string> read(std::string_view zip, const Entry& entry, std::size_t limit,
                                std::string& why);

/// Inflates raw deflate data (RFC 1951). Nothing, with `why` set, if it's
/// damaged or would be more than `limit` bytes.
std::optional<std::string> inflate(std::string_view data, std::size_t limit, std::string& why);

}  // namespace sb::kernel::unzip
