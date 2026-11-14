#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <string_view>
#include <vector>

// Crash reports. When SketchBillet crashes, it writes a small text file in its
// crash folder: its version, the computer's system, what went wrong, the last
// menu item or button clicked, and where in SketchBillet's code it was (the
// stack). Never a design, a file name or anything typed. The next time it
// starts, it offers to open a GitHub issue with the report filled in, which
// the student reads (and adds to) before sending; nothing is sent by itself.
namespace sb::app::crash {

// What's known about this copy, written into every report.
struct BuildInfo {
    QString version;  // like "0.1.0"
    QString channel;  // "release", "preview", or empty when built by hand
    QString commit;   // the commit it was built from, if known
};

// Catches crashes from now on (bad memory reads, abort(), unhandled C++
// exceptions, and on Windows unhandled structured exceptions), writing a report
// into `folder` (made if missing). Call once, early in main().
void install(const QString& folder, const BuildInfo& build);

// Remembers the last thing the student did, like "sb.modeling.fillet", for the
// next report. Only names of menu items and buttons: never what's in a design.
void note(std::string_view what);

// One report found in the crash folder.
struct Report {
    QString file;       // the report's file
    QDateTime when;     // when the crash happened (UTC)
    QString what;       // what went wrong, in plain words
    QString last;       // the last menu item or button clicked, if any
    QString version;    // SketchBillet's version
    QString system;     // the computer's system, like "Windows 11 Version 24H2"
    QStringList stack;  // where in the code, innermost first
    QByteArray raw;     // the file as written
};

// Reads a report file's text. `file` is only remembered.
Report parse(const QByteArray& raw, const QString& file);

// The reports waiting in `folder`, newest first.
std::vector<Report> pending(const QString& folder);

// Removes every report in `folder`, once the student has decided.
void forget(const QString& folder);

// The report as the body of a GitHub issue (Markdown), with a place at the top
// for the student to say what they were doing.
QString issueBody(const Report& report);

// A link that opens a new GitHub issue with the report filled in. Long stacks are
// shortened so the link stays under what browsers accept.
QUrl issueUrl(const Report& report);

// Crashes on purpose, for testing the reports: "segfault" (a bad memory read),
// "abort" (abort()) or "exception" (an unhandled C++ exception).
[[noreturn]] void crashForTest(const QString& how);

}  // namespace sb::app::crash
