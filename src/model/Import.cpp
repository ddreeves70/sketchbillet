#include "sb/model/Import.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>

#include "sb/model/Naming.h"

namespace sb::model {

namespace {

Diagnostic problem(std::string code, std::string message, std::string detail = {}) {
    return {std::move(code), Severity::Error, std::move(message), std::move(detail)};
}

// A path from UTF-8 text, and back, the same on every computer.
std::filesystem::path pathFrom(const std::string& utf8) {
    return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}
std::string utf8Of(const std::filesystem::path& path) {
    const auto u = path.u8string();
    return std::string(u.begin(), u.end());
}

}  // namespace

ImportFeature::ImportFeature(std::string name, ImportFormat format,
                             std::shared_ptr<const std::string> content, std::string source)
    : ImportFeature(FeatureId::generate(), std::move(name), format, std::move(content), std::move(source)) {}

ImportFeature::ImportFeature(FeatureId id, std::string name, ImportFormat format,
                             std::shared_ptr<const std::string> content, std::string source)
    : Feature(id, std::move(name)),
      format_(format),
      content_(std::move(content)),
      source_(std::move(source)) {}

const std::string& ImportFeature::content() const {
    static const std::string none;
    return content_ ? *content_ : none;
}

std::string ImportFeature::fileName() const {
    return utf8Of(pathFrom(source_).filename());
}

void ImportFeature::setContent(ImportFormat format, std::shared_ptr<const std::string> content,
                               std::string source) {
    format_ = format;
    content_ = std::move(content);
    source_ = std::move(source);
}

Built ImportFeature::build(kernel::IKernel& kernel, const ResultLookup&) const {
    Built out;
    if (content().empty()) {
        out.op.issues.push_back(
            problem("import.empty", "This step has no part in it: the design may be damaged. Import " +
                                        fileName() + " again with feature.reload_import."));
        return out;
    }
    out.op = kernel.importStep(content());
    if (!out.op.ok()) return out;
    // Every face is "<step>/import"; nameFromFaces() tells them apart by where they are.
    auto elements = kernel.elements(out.op.shape);
    const std::vector<std::string> faces(elements.faces.size(), shortId(id().toString()) + "/import");
    out.names = std::make_shared<const ShapeNames>(nameFromFaces(std::move(elements), faces));
    return out;
}

std::string_view formatName(ImportFormat format) {
    switch (format) {
        case ImportFormat::Step:
            return "step";
    }
    return "step";
}

ImportFile readImportFile(const std::string& file) {
    ImportFile out;
    const auto path = pathFrom(file);
    std::error_code ec;
    const auto whole = std::filesystem::absolute(path, ec);
    out.source = utf8Of(ec ? path : whole);
    out.stem = utf8Of(path.stem());
    const std::string name = utf8Of(path.filename());
    std::string ending = utf8Of(path.extension());
    for (auto& c : ending) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (ending == ".step" || ending == ".stp") {
        out.format = ImportFormat::Step;
    } else {
        out.issues.push_back(problem(
            "import.format", "SketchBillet can import STEP files (.step or .stp), so it can't import " +
                                 name + ". Export the part from the other program as STEP."));
        return out;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in || std::filesystem::is_directory(path, ec)) {
        out.issues.push_back(problem("import.no_file", "SketchBillet can't open " + name +
                                                           " (looked for it at " + out.source +
                                                           "). Check the file is there, and try again."));
        return out;
    }
    const auto size = std::filesystem::file_size(path, ec);
    if (!ec && size > kMaxImportBytes) {
        out.issues.push_back(
            problem("import.too_big", name + " is " + std::to_string(size / (1024 * 1024)) +
                                          " MB, and SketchBillet imports files up to 64 MB. Export a simpler "
                                          "version of the part, or just the piece you need."));
        return out;
    }
    std::ostringstream text;
    text << in.rdbuf();
    if (text.str().empty()) {
        out.issues.push_back(
            problem("import.empty", name + " is empty. Export the part again from the other program."));
        return out;
    }
    out.content = std::make_shared<const std::string>(text.str());
    return out;
}

}  // namespace sb::model
