// Writing zip files, shared by the .sb format and 3MF export. Private to sb_io.

#pragma once

#include <deque>
#include <filesystem>
#include <string>
#include <utility>

#include "sb/core/Diagnostic.h"
#include "sb/io/SketchBilletFile.h"

namespace sb::io::zip {

// One file in the zip: its name, then its bytes.
using Entries = std::deque<std::pair<std::string, std::string>>;

// Packs entries, in order, into a zip in memory. Every entry gets the same
// fixed date (1980-01-01), so the same entries always give the same bytes.
// `compress` false stores entries as they are. On failure, the problem's
// detail says what libzip said; `failure` gives its code and message.
Diagnostics pack(const Entries& entries, Bytes& out, const Diagnostic& failure, bool compress = true);

// Writes bytes next to `path`, then moves them into place, so a failed write
// never leaves half a file behind. `failure` is the problem to report, with
// the reason added as its detail.
Diagnostics writeAtomically(const Bytes& bytes, const std::filesystem::path& path, const Diagnostic& failure);

}  // namespace sb::io::zip
