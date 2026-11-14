/// @file
/// The Import step: a part made in another program, read from a STEP, STL or
/// 3MF file and kept in the design.
#pragma once

#include <memory>
#include <string>

#include "sb/core/Diagnostic.h"
#include "sb/core/Units.h"
#include "sb/model/Feature.h"

namespace sb::model {

/// The kinds of file a part can be imported from.
enum class ImportFormat {
    Step,     ///< STEP (ISO 10303-21): .step or .stp. The part is exact, with its curved faces.
    Stl,      ///< STL: .stl, binary or text. Triangles only, and no unit (see ImportFeature::unit()).
    ThreeMf,  ///< 3MF: .3mf. Triangles, in the unit the file says.
};

/// An Import step: a part from another program, as a body. STEP files give the
/// exact part; STL and 3MF files give triangles, made into a solid whose flat
/// areas are one face each (see kernel::IKernel::solidFromMesh()). The file's whole
/// contents are kept in the step (and saved in the design), so the design opens
/// anywhere, even where the file isn't. It also remembers where the file came
/// from, so it can be read again after the file changes (see
/// feature.reload_import).
///
/// Its faces are named `<step>/import`, told apart by where they are (see
/// nameFromFaces()), so sketches and fillets on them keep their faces when
/// steps before it change. After reading the file again, faces are found by
/// where they are and how they look.
///
/// @code
/// auto read = readImportFile("bracket.step");
/// if (read.issues.empty())
///     doc.execute(std::make_unique<AddFeatureCommand>(std::make_unique<ImportFeature>(
///         "bracket", read.format, read.content, read.source)));
/// @endcode
class ImportFeature final : public Feature {
public:
    /// Makes an Import step with a new id.
    /// @param name The name shown in the feature tree.
    /// @param format What kind of file it is.
    /// @param content The file's whole contents.
    /// @param source Where the file was, as UTF-8 text: the whole path, for reading it again.
    ImportFeature(std::string name, ImportFormat format, std::shared_ptr<const std::string> content,
                  std::string source);

    /// Makes an Import step with a given id, for loading a saved design.
    /// @param id The id it was saved with.
    /// @param name The name shown in the feature tree.
    /// @param format What kind of file it is.
    /// @param content The file's whole contents.
    /// @param source Where the file was, as UTF-8 text.
    ImportFeature(FeatureId id, std::string name, ImportFormat format,
                  std::shared_ptr<const std::string> content, std::string source);

    /// Gives what kind of file it is.
    /// @return The format.
    ImportFormat format() const { return format_; }

    /// Gives the file's contents.
    /// @return The contents; empty if there are none (a damaged design).
    const std::string& content() const;

    /// Gives the file's contents, shared: copies of this step share them.
    /// @return The contents, or null.
    const std::shared_ptr<const std::string>& sharedContent() const { return content_; }

    /// Gives where the file was when it was imported or last read again.
    /// @return The whole path, as UTF-8 text.
    const std::string& source() const { return source_; }

    /// Gives the file's name, without its folder.
    /// @return For example "bracket.step".
    std::string fileName() const;

    /// Gives the unit an STL file's numbers are in (STL files don't say).
    /// @return Millimeter unless it was changed; not used for STEP and 3MF.
    LengthUnit unit() const { return unit_; }

    /// Changes the unit an STL file's numbers are in. Use on a copy, or from a Command.
    /// @param unit The unit, like LengthUnit::Inch for a part made in inches.
    void setUnit(LengthUnit unit) { unit_ = unit; }

    /// Puts in new contents, read again from a file. Use on a copy, or from a Command.
    /// @param format What kind of file it is.
    /// @param content The file's whole contents.
    /// @param source Where the file was, as UTF-8 text.
    void setContent(ImportFormat format, std::shared_ptr<const std::string> content, std::string source);

    FeatureType type() const override { return FeatureType::Import; }
    std::unique_ptr<Feature> clone() const override { return std::make_unique<ImportFeature>(*this); }
    Built build(kernel::IKernel& kernel, const ResultLookup& earlier) const override;

private:
    ImportFormat format_;
    std::shared_ptr<const std::string> content_;
    std::string source_;
    LengthUnit unit_ = LengthUnit::Millimeter;
};

/// The name of a format in files and messages.
/// @param format The format.
/// @return "step", "stl" or "3mf".
std::string_view formatName(ImportFormat format);

/// What readImportFile() read.
struct ImportFile {
    ImportFormat format = ImportFormat::Step;    ///< What kind of file it is, from its ending.
    std::shared_ptr<const std::string> content;  ///< Its whole contents; null if it couldn't be read.
    std::string source;                          ///< Its whole path, as UTF-8 text.
    std::string stem;                            ///< Its name without the ending, like "bracket".
    Diagnostics issues;                          ///< Why it couldn't be read.
};

/// Reads a file to import. Its kind comes from its ending: .step, .stp, .stl or .3mf. It
/// isn't checked to be a part here: that happens when the step builds.
/// @param file The file, as UTF-8 text: a whole path, or one from the current folder.
/// @return Its contents, or why not: import.no_file (it isn't there), import.format
///     (an ending SketchBillet can't import), import.too_big (over 64 MB), import.empty.
ImportFile readImportFile(const std::string& file);

/// The biggest file that can be imported (64 MB): bigger ones make designs too
/// big to save and open quickly.
inline constexpr std::size_t kMaxImportBytes = 64ull * 1024 * 1024;

}  // namespace sb::model
