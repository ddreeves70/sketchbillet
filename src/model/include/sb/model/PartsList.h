/// @file
/// An assembly's parts list (bill of materials): each part once, how many
/// components use it, what it's made of and what it weighs.

#pragma once

#include <string>
#include <vector>

#include "sb/core/Id.h"
#include "sb/core/Units.h"
#include "sb/kernel/IKernel.h"

namespace sb::model {

class Document;
class RebuildReport;

/// One row of a parts list: a part, and how many of it the assembly uses.
struct PartsListRow {
    int item = 0;           ///< Its number in the list, from 1.
    DocId part{};           ///< The part.
    std::string name;       ///< Its name, like "Wheel".
    int count = 0;          ///< How many components use it: how many to print or make.
    std::string material;   ///< What its bodies are made of, like "PLA", or "PLA, TPU" for more than one.
    int bodies = 0;         ///< How many bodies it has (0 when it has none, or didn't build).
    double volumeEach = 0;  ///< The volume of one (mm^3).
    double gramsEach = 0;   ///< The mass of one (g).
    double gramsAll = 0;    ///< The mass of all of them: gramsEach × count (g).
    double costEach = 0;    ///< The material cost of one, for materials with a price (0 if none has).
    bool linked = false;    ///< True if it's linked to its own `.sb` file; false if it's kept inside.
    std::string file;       ///< The linked file's name, like "Wheel.sb", or empty for an inside part.
    bool built = false;     ///< True if it built with no errors.
};

/// Makes an assembly's parts list: each part once, in the assembly's order, with
/// how many components use it, its materials, and its mass each and in all.
/// @param assembly The assembly.
/// @param report Its last rebuild.
/// @param kernel The kernel that built it.
/// @return The rows; empty for a part, or an assembly with no parts.
///
/// @code
/// for (const auto& row : model::partsList(car, report, kernel))
///     std::printf("%d x %s\n", row.count, row.name.c_str());  // "4 x Wheel"
/// @endcode
std::vector<PartsListRow> partsList(const Document& assembly, const RebuildReport& report,
                                    const kernel::IKernel& kernel);

/// Writes a parts list as CSV (comma-separated values), for a spreadsheet: a
/// heading row, then a row for each part. Text with a comma or a quote is quoted.
/// Masses are in grams and volumes in the unit given, cubed.
/// @param rows The parts list (see partsList()).
/// @param unit The unit for the volumes.
/// @return The CSV text, with "\n" after each row.
std::string partsListCsv(const std::vector<PartsListRow>& rows, LengthUnit unit);

}  // namespace sb::model
