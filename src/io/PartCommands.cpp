#include "sb/io/PartCommands.h"

#include <algorithm>
#include <memory>

#include "sb/io/SketchBilletFile.h"
#include "sb/model/AssemblyCommands.h"
#include "sb/model/Commands.h"

namespace sb::io {

void addPartCommands(model::CommandRegistry& r) {
    using model::ArgType;
    r.add(
        {"component.insert",
         "Insert copy of part",
         "Puts a part from a .sb file into the assembly, with one or more components of it in a row. The "
         "assembly keeps its own copy of the part, so it opens anywhere; later changes to the file don't "
         "change it.",
         {{"file", ArgType::Text, "The part's file, like wheel.sb.", true, {}},
          {"count", ArgType::Integer, "How many, from 1 to 100. Leave it out for 1.", false, {}},
          {"at", ArgType::Vec3, "Where the first one's origin goes (mm). Leave it out for 0,0,0.", false, {}},
          {"step", ArgType::Vec3, "How far each one is from the one before (mm), like 30,0,0.", false, {}}}},
        [](const model::Document& doc, const model::Args& a) -> std::unique_ptr<model::Command> {
            if (doc.kind() != model::DocKind::Assembly) {
                return std::make_unique<model::ProblemCommand>(Diagnostic{
                    "assembly.not_assembly",
                    Severity::Error,
                    doc.name() +
                        " is a part, not an assembly, so parts can't go in it. Start an assembly in a "
                        "new design with assembly.new.",
                    {}});
            }
            const std::string file = model::argText(a, "file");
            auto opened = openFile(std::filesystem::path(std::u8string(file.begin(), file.end())));
            if (!opened.ok()) return std::make_unique<model::ProblemCommand>(opened.issues.front());
            int count = 1;
            if (const auto it = a.find("count"); it != a.end()) {
                if (const auto* n = std::get_if<std::int64_t>(&it->second))
                    count = static_cast<int>(std::clamp<std::int64_t>(*n, -1, 1000));
            }
            kernel::Placement at;
            at.origin = model::argVec3(a, "at");
            return std::make_unique<model::InsertPartCommand>(std::move(opened.document), count, at,
                                                              model::argVec3(a, "step"));
        });
}

}  // namespace sb::io
