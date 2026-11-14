// Tests for crash reports (CrashReport.h) and the offer to send one the next time
// SketchBillet starts. Real crashes are tested by app.crash_report.* (CTest),
// which crash the app on purpose.

#include <catch2/catch_test_macros.hpp>

#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QFile>
#include <QSettings>
#include <QTemporaryDir>
#include <QUrlQuery>

#include <memory>

#include "AppSettings.h"
#include "AppUi.h"
#include "CrashReport.h"
#include "Shell.h"
#include "sb/kernel_occt/OcctKernel.h"
#include "sb/plugin/PluginHost.h"
#include "sb/plugins/Modeling.h"
#include "sb/sketch_planegcs/PlaneGcsSolver.h"

using namespace sb;
namespace crash = app::crash;

namespace {

const QByteArray kLinuxReport =
    "SketchBillet crash report\n"
    "version=0.1.0\n"
    "channel=release\n"
    "commit=1531bb5\n"
    "system=Ubuntu 24.04.5 LTS (x86_64)\n"
    "qt=6.8.3\n"
    "time=1794631911\n"
    "what=segfault\n"
    "last=sb.modeling.fillet\n"
    "stack:\n"
    "/home/alex/Apps/sketchbillet(+0x71139)[0x55786f594139]\n"
    "/tmp/.mount_SketchAbc/usr/lib/libTKernel.so.7.8(_ZN5Foo3barEv+0x1c)[0x7f1d41845330]\n"
    "libc.so.6(+0x2a1ca)[0x7f1d4182a1ca]\n";

void write(const QString& file, const QByteArray& text) {
    QFile f(file);
    REQUIRE(f.open(QIODevice::WriteOnly));
    f.write(text);
}

}  // namespace

TEST_CASE("A crash report is read, without the student's folders", "[app][crash]") {
    const auto r = crash::parse(kLinuxReport, QStringLiteral("crash-1.txt"));
    CHECK(r.version == QStringLiteral("0.1.0"));
    CHECK(r.system == QStringLiteral("Ubuntu 24.04.5 LTS (x86_64)"));
    CHECK(r.when == QDateTime::fromSecsSinceEpoch(1794631911).toUTC());
    CHECK(r.what == QStringLiteral("SketchBillet used memory it shouldn't have (a segmentation fault)."));
    CHECK(r.last == QStringLiteral("sb.modeling.fillet"));
    REQUIRE(r.stack.size() == 3);
    // Linux writes whole paths, which can have a name in them: only file names stay.
    CHECK(r.stack[0] == QStringLiteral("sketchbillet(+0x71139)[0x55786f594139]"));
    CHECK(r.stack[1] == QStringLiteral("libTKernel.so.7.8(_ZN5Foo3barEv+0x1c)[0x7f1d41845330]"));
    CHECK(r.stack[2] == QStringLiteral("libc.so.6(+0x2a1ca)[0x7f1d4182a1ca]"));
    CHECK_FALSE(r.raw.contains("alex"));
    CHECK_FALSE(r.raw.contains(".mount"));
    CHECK(r.raw.contains("what=segfault\nlast=sb.modeling.fillet\nstack:\nsketchbillet(+0x71139)"));
}

TEST_CASE("Each kind of crash is said in plain words", "[app][crash]") {
    auto what = [](const QByteArray& lines) {
        return crash::parse("SketchBillet crash report\n" + lines + "\nstack:\n", QString()).what;
    };
    CHECK(what("what=abort").startsWith(QStringLiteral("SketchBillet stopped itself")));
    CHECK(what("what=exception\nexception=Standard_ConstructionError") ==
          QStringLiteral("An error nobody caught: Standard_ConstructionError"));
    CHECK(what("what=arithmetic").startsWith(QStringLiteral("A math error")));
    CHECK(what("what=exception-code 0xc0000005") ==
          QStringLiteral("Windows stopped SketchBillet (exception 0xc0000005)."));
}

TEST_CASE("The GitHub issue: a place for the student, the facts, and a link that isn't too long",
          "[app][crash]") {
    const auto r = crash::parse(kLinuxReport, QString());
    const auto body = crash::issueBody(r);
    CHECK(body.startsWith(QStringLiteral("**What were you doing when SketchBillet closed?**")));
    CHECK(body.contains(QStringLiteral("- **Version:** 0.1.0")));
    CHECK(body.contains(QStringLiteral("- **When:** 2026-11-14 04:51 UTC")));
    CHECK(body.contains(QStringLiteral("- **Last thing clicked:** `sb.modeling.fillet`")));
    CHECK(body.contains(QStringLiteral("```\nSketchBillet crash report\nversion=0.1.0")));

    const auto url = crash::issueUrl(r);
    CHECK(url.host() == QStringLiteral("github.com"));
    CHECK(url.path() == QStringLiteral("/ddreeves70/sketchbillet/issues/new"));
    const QUrlQuery query(url);
    CHECK(query.queryItemValue(QStringLiteral("title"), QUrl::FullyDecoded) ==
          QStringLiteral("Crash in SketchBillet 0.1.0 after sb.modeling.fillet"));
    CHECK(query.queryItemValue(QStringLiteral("body"), QUrl::FullyDecoded) == body);

    // A very deep stack is shortened from the outermost end.
    QByteArray deep = kLinuxReport;
    for (int i = 0; i < 400; ++i)
        deep += "libQt6Widgets.so.6(_ZN7QWidget5eventEP6QEvent+0x" + QByteArray::number(i) + ")[0x7f]\n";
    const auto long_ = crash::issueUrl(crash::parse(deep, QString()));
    CHECK(long_.toEncoded().size() < 8000);
    const auto shortened = QUrlQuery(long_).queryItemValue(QStringLiteral("body"), QUrl::FullyDecoded);
    CHECK(shortened.contains(QStringLiteral("sketchbillet(+0x71139)")));  // the innermost lines stay
    CHECK_FALSE(shortened.contains(QStringLiteral("+0x399)")));
}

TEST_CASE("Reports waiting in the crash folder, newest first, and removed once decided", "[app][crash]") {
    QTemporaryDir dir;
    QByteArray older = kLinuxReport;
    older.replace("time=1794631911", "time=1794600000");
    write(dir.filePath(QStringLiteral("crash-1-10.txt")), older);
    write(dir.filePath(QStringLiteral("crash-2-11.txt")), kLinuxReport);
    write(dir.filePath(QStringLiteral("crash-3-12.txt")),
          "SketchBillet crash report\nversion=0.1.0\n");  // cut short
    write(dir.filePath(QStringLiteral("notes.txt")), "not a report");
    const auto reports = crash::pending(dir.path());
    REQUIRE(reports.size() == 2);
    CHECK(reports[0].when > reports[1].when);
    CHECK(reports[0].file.endsWith(QStringLiteral("crash-2-11.txt")));
    crash::forget(dir.path());
    CHECK(crash::pending(dir.path()).empty());
    CHECK(QFile::exists(dir.filePath(QStringLiteral("notes.txt"))));
}

TEST_CASE("The next start offers the report: report it, copy it, or not", "[app][crash]") {
    QTemporaryDir dir;
    QSettings settings(dir.filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
    auto kernel = kernel::makeOcctKernel();
    auto solver = sketch::makePlaneGcsSolver();
    app::AppSettings pluginSettings(settings);
    app::AppUi ui;
    plugin::PluginHost host(kernel.get(), solver.get());
    host.provide<ui::UiRegistry>(ui);
    host.addBuiltin(plugins::modelingPlugin());
    host.setSettings(pluginSettings);
    host.loadAll();
    app::Shell shell(host, ui, settings);
    const auto folder = dir.filePath(QStringLiteral("crashes"));
    QDir().mkpath(folder);
    std::vector<QUrl> opened;
    shell.openLink = [&](const QUrl& url) { opened.push_back(url); };
    QString askedBody;
    int askedCount = 0;
    auto answer = [&](app::Shell::CrashChoice choice) {
        shell.askCrashReport = [&, choice](const QString&, const QString& body, int count) {
            askedBody = body;
            askedCount = count;
            return choice;
        };
    };

    SECTION("no reports: nothing is asked") {
        answer(app::Shell::CrashChoice::Report);
        CHECK_FALSE(shell.offerCrashReport(folder));
        CHECK(askedCount == 0);
    }
    SECTION("Report It opens the issue, once") {
        write(QDir(folder).filePath(QStringLiteral("crash-1-1.txt")), kLinuxReport);
        write(QDir(folder).filePath(QStringLiteral("crash-2-2.txt")), kLinuxReport);
        answer(app::Shell::CrashChoice::Report);
        CHECK(shell.offerCrashReport(folder) == app::Shell::CrashChoice::Report);
        CHECK(askedCount == 2);
        CHECK(askedBody.contains(QStringLiteral("`sb.modeling.fillet`")));
        REQUIRE(opened.size() == 1);
        CHECK(opened[0].path() == QStringLiteral("/ddreeves70/sketchbillet/issues/new"));
        CHECK_FALSE(shell.offerCrashReport(folder));  // asked once
    }
    SECTION("Copy Report puts it on the clipboard") {
        write(QDir(folder).filePath(QStringLiteral("crash-1-1.txt")), kLinuxReport);
        answer(app::Shell::CrashChoice::Copy);
        CHECK(shell.offerCrashReport(folder) == app::Shell::CrashChoice::Copy);
        CHECK(opened.empty());
        CHECK(QGuiApplication::clipboard()->text() == askedBody);
        CHECK(crash::pending(folder).empty());
    }
    SECTION("Don't Report sends nothing and forgets it") {
        write(QDir(folder).filePath(QStringLiteral("crash-1-1.txt")), kLinuxReport);
        answer(app::Shell::CrashChoice::Skip);
        CHECK(shell.offerCrashReport(folder) == app::Shell::CrashChoice::Skip);
        CHECK(opened.empty());
        CHECK(crash::pending(folder).empty());
    }
}
