#pragma once

/// @file
/// Saving and opening .sb design files.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include "sb/core/Diagnostic.h"
#include "sb/model/Document.h"

/// Reading and writing files: the native .sb format.
namespace sb::io {

/// The bytes of a whole file in memory.
using Bytes = std::vector<std::uint8_t>;

/// What openFile() and readBytes() return.
struct OpenResult {
    std::unique_ptr<model::Document> document;  ///< The design; null if it could not be opened.
    Diagnostics issues;                         ///< Why it could not be opened, in plain words.
    /// Things to know about how it opened, in plain words, when it did: for an
    /// assembly, linked parts updated from their files, found somewhere else, or
    /// not found (so their saved copies are used). See updateLinkedParts().
    Diagnostics notes;

    /// Checks whether the design was opened.
    /// @return True if @ref document is set.
    bool ok() const { return document != nullptr; }
};

/// Saves a design as a .sb file and marks the document as saved.
///
/// The file is written to a temporary file first and then moved into place, so
/// a failed save never damages an existing file.
///
/// @code
/// if (auto issues = io::saveFile(doc, "bracket.sb"); issues.empty()) {
///     auto opened = io::openFile("bracket.sb");  // same ids, same steps
/// }
/// @endcode
/// @param doc The design to save. On success, doc.isModified() becomes false.
/// @param path Where to save. An existing file is replaced.
/// @return No diagnostics on success; otherwise why the file could not be saved.
Diagnostics saveFile(model::Document& doc, const std::filesystem::path& path);

/// Saves a design as a .sb file without marking it saved, for callers that
/// only have read access to the document (the desktop app saves through the
/// DocumentList, then calls its markSaved()). Same safe write as saveFile().
/// @param doc The design to save.
/// @param path Where to save. An existing file is replaced.
/// @return No diagnostics on success; otherwise why the file could not be saved.
Diagnostics writeFile(const model::Document& doc, const std::filesystem::path& path);

/// Opens a .sb file.
///
/// The opened design has no undo history and is not marked as modified. An
/// assembly's linked parts are then brought up to date with their files
/// (updateLinkedParts(), looking in the assembly's folder): that's one undo step,
/// and leaves the assembly modified, and OpenResult::notes says what changed.
/// @param path The file to open.
/// @return The design, or diagnostics saying why it could not be opened.
OpenResult openFile(const std::filesystem::path& path);

/// Writes a design in the .sb format to memory, for example for a browser
/// download or a test. Does not mark the document as saved.
/// @param doc The design to write.
/// @param out Receives the file's bytes.
/// @param folder For an assembly, the folder the file goes in: linked parts' paths
///        are written relative to it. Empty to write them as they are.
/// @return No diagnostics on success; otherwise what went wrong.
Diagnostics writeBytes(const model::Document& doc, Bytes& out, const std::filesystem::path& folder = {});

/// Reads a design from the bytes of a .sb file.
/// @param data The file's bytes.
/// @param displayName The file name to use in messages, e.g. "bracket.sb".
/// @param folder For an assembly, the folder the file was in: linked parts' paths
///        are relative to it, and their files are looked for there (findLinkedFile()).
///        Empty to keep the paths as they are, and not look.
/// @return The design, or diagnostics saying why it could not be read. Linked
///         parts aren't updated: openFile() does that.
OpenResult readBytes(const Bytes& data, std::string_view displayName,
                     const std::filesystem::path& folder = {});

/// Reads the design id of a .sb file, without reading the rest of the design.
/// @param data The file's bytes.
/// @return The id in its manifest, or nullopt if it isn't a SketchBillet file.
std::optional<DocId> readDesignId(const Bytes& data);

/// Reads a whole file into memory.
/// @param path The file.
/// @param out Receives its bytes.
/// @return True if it could be read.
bool readFileBytes(const std::filesystem::path& path, Bytes& out);

}  // namespace sb::io
