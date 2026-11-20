#include "sb/io/PartCommands.h"

#include <algorithm>
#include <memory>
#include <system_error>

#include "sb/io/LinkedParts.h"
#include "sb/io/SketchBilletFile.h"
#include "sb/model/AssemblyCommands.h"
#include "sb/model/Commands.h"

namespace sb::io {

namespace {

namespace fs = std::filesystem;

std::unique_ptr<model::Command> problem(std::string code, std::string message) {
    return std::make_unique<model::ProblemCommand>(
        Diagnostic{std::move(code), Severity::Error, std::move(message), {}});
}

fs::path fromUtf8(const std::string& text) {
    return fs::path(std::u8string(text.begin(), text.end()));
}

std::string toUtf8(const fs::path& p) {
    const auto text = p.generic_u8string();
    return std::string(text.begin(), text.end());
}

// The file's full path, tidied, as a link keeps it.
fs::path fullPath(const fs::path& file) {
    std::error_code ec;
    const auto full = fs::absolute(file, ec);
    return (ec ? file : full).lexically_normal();
}

// A part's file, read: the design, and the link to it (full path, fingerprint, now).
struct ReadPart {
    std::unique_ptr<model::Document> design;
    model::PartLink link;
    std::unique_ptr<model::Command> problem;  // why it couldn't be read
};

ReadPart readPart(const std::string& file) {
    ReadPart out;
    const auto path = fullPath(fromUtf8(file));
    Bytes data;
    if (!readFileBytes(path, data)) {
        out.problem =
            problem("file.cant_read", "SketchBillet couldn't open " + toUtf8(path.filename()) +
                                          ". Check that the file is there and that you're allowed to "
                                          "open it.");
        return out;
    }
    auto opened = readBytes(data, toUtf8(path.filename()), path.parent_path());
    if (!opened.ok()) {
        out.problem = std::make_unique<model::ProblemCommand>(opened.issues.front());
        return out;
    }
    out.design = std::move(opened.document);
    out.link = {toUtf8(path), sha256Hex(data), utcNow(), true};
    return out;
}

const model::Document* partArg(const model::Document& doc, const model::Args& a,
                               std::unique_ptr<model::Command>& why) {
    Diagnostic d;
    const auto* part = model::partNamed(doc, model::argText(a, "part"), d);
    if (!part) why = std::make_unique<model::ProblemCommand>(d);
    return part;
}

}  // namespace

void addPartCommands(model::CommandRegistry& r) {
    using model::ArgType;
    r.add(
        {"component.insert",
         "Insert part from file",
         "Puts a part from a .sb file into the assembly, with one or more components of it in a row. The "
         "part "
         "stays linked to its file: when the file changes, the assembly uses the new part the next time it "
         "opens (or with part.update). The assembly keeps a copy too, so it opens even without the file. Use "
         "link=false for an inside part, which doesn't follow the file.",
         {{"file", ArgType::Text, "The part's file, like wheel.sb.", true, {}},
          {"count", ArgType::Integer, "How many, from 1 to 100. Leave it out for 1.", false, {}},
          {"at", ArgType::Vec3, "Where the first one's origin goes (mm). Leave it out for 0,0,0.", false, {}},
          {"step", ArgType::Vec3, "How far each one is from the one before (mm), like 30,0,0.", false, {}},
          {"link",
           ArgType::Bool,
           "true to follow the file (a linked part), false to only keep a copy (an inside part). Leave it "
           "out for "
           "true.",
           false,
           {}}}},
        [](const model::Document& doc, const model::Args& a) -> std::unique_ptr<model::Command> {
            if (doc.kind() != model::DocKind::Assembly) {
                return problem(
                    "assembly.not_assembly",
                    doc.name() +
                        " is a part, not an assembly, so parts can't go in it. Start an assembly in a "
                        "new design with assembly.new.");
            }
            auto read = readPart(model::argText(a, "file"));
            if (read.problem) return std::move(read.problem);
            int count = 1;
            if (const auto it = a.find("count"); it != a.end()) {
                if (const auto* n = std::get_if<std::int64_t>(&it->second))
                    count = static_cast<int>(std::clamp<std::int64_t>(*n, -1, 1000));
            }
            kernel::Placement at;
            at.origin = model::argVec3(a, "at");
            std::optional<model::PartLink> link;
            if (model::argBool(a, "link", true)) link = std::move(read.link);
            return std::make_unique<model::InsertPartCommand>(std::move(read.design), count, at,
                                                              model::argVec3(a, "step"), std::move(link));
        });

    r.add({"part.update",
           "Update linked parts",
           "Checks the files of the assembly's linked parts, and uses the new part from each file that "
           "changed. "
           "Opening the assembly does this too; use it when a file changed while the assembly was open.",
           {{"part",
             ArgType::Text,
             "Just this part, like Wheel. Leave it out for every linked part.",
             false,
             {}}}},
          [](const model::Document& doc, const model::Args& a) -> std::unique_ptr<model::Command> {
              std::optional<DocId> only;
              if (a.count("part")) {
                  std::unique_ptr<model::Command> why;
                  const auto* part = partArg(doc, a, why);
                  if (!part) return why;
                  if (!doc.partLink(part->id())) {
                      return problem("part.not_linked",
                                     part->name() + " is an inside part, so it has no file to update from.");
                  }
                  only = part->id();
              } else if (doc.kind() != model::DocKind::Assembly) {
                  return problem("assembly.not_assembly",
                                 doc.name() + " is a part, not an assembly, so it has no linked parts.");
              }
              auto updates = updateLinkedParts(doc, {}, only);
              if (updates.command) return std::move(updates.command);
              // Nothing changed: say why (a file that can't be found), or that it's up to date.
              for (const auto& note : updates.notes) {
                  if (note.severity == Severity::Warning) {
                      return std::make_unique<model::ProblemCommand>(
                          Diagnostic{note.code, Severity::Error, note.message, note.detail});
                  }
              }
              const auto parts = doc.parts();
              const bool any = std::any_of(parts.begin(), parts.end(), [&](const model::Document* p) {
                  return doc.partLink(p->id()) != nullptr;
              });
              if (!any) {
                  return problem("part.none_linked",
                                 doc.name() + " has no linked parts. Put one in with component.insert.");
              }
              return problem("part.up_to_date",
                             only
                                 ? doc.findPart(*only)->name() +
                                       " is the same as its file already, so there's "
                                       "nothing to update."
                                 : "Every linked part is the same as its file already, so there's nothing to "
                                   "update.");
          });

    r.add({"part.relink",
           "Find linked file",
           "Links a part to its file where it is now, when the assembly can't find it (it was moved, or "
           "renamed "
           "somewhere else). The file must hold the same design: the one the part was made from.",
           {{"part", ArgType::Text, "The part, like Wheel.", true, {}},
            {"file", ArgType::Text, "Its file where it is now, like parts/wheel.sb.", true, {}}}},
          [](const model::Document& doc, const model::Args& a) -> std::unique_ptr<model::Command> {
              std::unique_ptr<model::Command> why;
              const auto* part = partArg(doc, a, why);
              if (!part) return why;
              auto read = readPart(model::argText(a, "file"));
              if (read.problem) return std::move(read.problem);
              const std::string file = toUtf8(fromUtf8(read.link.path).filename());
              if (read.design->id() != part->id()) {
                  return problem("part.other_design", file + " holds " + read.design->name() +
                                                          ", a different design from " + part->name() +
                                                          ", so it can't take its place. Pick the file " +
                                                          part->name() + " was saved in.");
              }
              std::vector<model::PartUpdate> changes;
              changes.push_back({part->id(), std::move(read.design), std::move(read.link)});
              return std::make_unique<model::UpdatePartsCommand>(std::move(changes),
                                                                 "Link " + part->name() + " to " + file);
          });

    r.add({"part.save_as",
           "Save part as file",
           "Saves one of the assembly's parts to its own .sb file, and links the part to it, so it can be "
           "opened "
           "and changed on its own, or used in other assemblies.",
           {{"part", ArgType::Text, "The part, like Wheel.", true, {}},
            {"file", ArgType::Text, "The new file, like wheel.sb.", true, {}}}},
          [](const model::Document& doc, const model::Args& a) -> std::unique_ptr<model::Command> {
              std::unique_ptr<model::Command> why;
              const auto* part = partArg(doc, a, why);
              if (!part) return why;
              const auto path = fullPath(fromUtf8(model::argText(a, "file")));
              const std::string file = toUtf8(path.filename());
              std::error_code ec;
              if (fs::exists(path, ec)) {
                  return problem("part.file_there", file + " is there already. Pick another name, or link " +
                                                        part->name() + " to it with part.relink.");
              }
              if (auto issues = writeFile(*part, path); !issues.empty()) {
                  return std::make_unique<model::ProblemCommand>(issues.front());
              }
              Bytes data;
              if (!readFileBytes(path, data)) {
                  return problem("file.cant_read",
                                 "SketchBillet saved " + file + " but couldn't read it back.");
              }
              std::vector<model::PartUpdate> changes;
              changes.push_back(
                  {part->id(), nullptr, model::PartLink{toUtf8(path), sha256Hex(data), utcNow(), true}});
              return std::make_unique<model::UpdatePartsCommand>(std::move(changes),
                                                                 "Save " + part->name() + " as " + file);
          });
}

}  // namespace sb::io
