#pragma once

/// @file
/// Linked parts: an assembly's parts that come from their own `.sb` files (see
/// docs/design/assemblies.md, "Where parts live"). Finding the files, telling
/// when they've changed, and updating the assembly from them.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "sb/core/Diagnostic.h"
#include "sb/core/Id.h"
#include "sb/model/Command.h"
#include "sb/model/Document.h"

namespace sb::io {

/// Gives the SHA-256 fingerprint of some bytes, as linked parts keep it.
/// @param data The bytes, like a whole `.sb` file.
/// @return 64 lowercase hex digits.
std::string sha256Hex(const std::vector<std::uint8_t>& data);

/// Gives the time now, as a linked part's PartLink::updated keeps it.
/// @return The time in UTC, to the second, like "2026-11-13T23:40:00Z".
std::string utcNow();

/// Writes a link's date the way messages show it.
/// @param updated A time like "2026-11-13T23:40:00Z".
/// @return Like "Nov 13, 2026", or @p updated as it is if it isn't a time like that.
std::string linkDate(const std::string& updated);

/// Looks for a linked part's file: at its path; then by its file name in
/// @p folder; then by the part's design id among the `.sb` files in @p folder
/// and the folders in it, three deep (so a renamed file is found too). A file
/// only counts if it holds the same design.
/// @param link The part's link (PartLink::path is a full path).
/// @param part The part's design id.
/// @param folder Where else to look, usually the assembly's folder; empty to
///        look only in the folder the file was in.
/// @return The file, or nullopt if it can't be found.
std::optional<std::filesystem::path> findLinkedFile(const model::PartLink& link, const DocId& part,
                                                    const std::filesystem::path& folder);

/// What updating an assembly's linked parts found.
struct LinkUpdates {
    /// The change that brings the assembly up to date: parts updated from files
    /// that changed, and links to files found somewhere else. One undo step; null
    /// if there's nothing to change.
    std::unique_ptr<model::Command> command;
    /// What happened to each linked part that wasn't simply up to date, in plain
    /// words: notes (Severity::Info) for parts updated or found somewhere else,
    /// warnings for files that can't be found or opened, so the saved copy is used.
    Diagnostics notes;
};

/// Looks for an assembly's linked files (findLinkedFile()) and makes the change
/// that brings the assembly up to date with them, as opening it does and
/// `part.update` does.
///
/// @code
/// auto updates = sb::io::updateLinkedParts(car, "/class/car");
/// if (updates.command) car.execute(std::move(updates.command));
/// for (const auto& note : updates.notes) show(note.message);
/// @endcode
/// @param assembly The assembly.
/// @param folder The assembly's folder, to look for moved files in; empty to
///        look only next to where each file was.
/// @param only Just this part (its design id); nullopt for every linked part.
/// @return The change and the notes.
LinkUpdates updateLinkedParts(const model::Document& assembly, const std::filesystem::path& folder,
                              const std::optional<DocId>& only = std::nullopt);

}  // namespace sb::io
