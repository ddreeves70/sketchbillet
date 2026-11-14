// Tests for the update check (Updates.h) and Help > Check for Updates: reading
// GitHub's answers, when daily checks run, and what the student sees. GitHub's
// answers come from files (file:// URLs), so nothing goes online.

#include <catch2/catch_test_macros.hpp>

#include <QAction>
#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QLabel>
#include <QSettings>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QUrl>

#include <memory>
#include <optional>

#include "AppSettings.h"
#include "AppUi.h"
#include "Shell.h"
#include "Updates.h"
#include "sb/kernel_occt/OcctKernel.h"
#include "sb/plugin/PluginHost.h"
#include "sb/plugins/Modeling.h"
#include "sb/sketch_planegcs/PlaneGcsSolver.h"

using namespace sb;
using app::UpdateChannel;
using app::UpdateChecker;
using Kind = app::UpdateResult::Kind;

namespace {

// A folder laid out like GitHub's API for SketchBillet, holding the answers.
struct FakeGitHub {
    QTemporaryDir dir;
    QUrl url() const { return QUrl::fromLocalFile(dir.path()); }
    void answer(const QString& path, const QByteArray& json) {
        const auto file = dir.filePath(path);
        QDir().mkpath(QFileInfo(file).path());
        QFile f(file);
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write(json);
    }
    void release(const QString& tag) {
        answer(
            QStringLiteral("releases/latest"),
            QStringLiteral(
                R"({"tag_name": "%1", "html_url": "https://github.com/ddreeves70/sketchbillet/releases/tag/%1"})")
                .arg(tag)
                .toUtf8());
    }
    void preview(const QString& sha) {
        answer(QStringLiteral("git/ref/tags/preview"),
               QStringLiteral(R"({"ref": "refs/tags/preview", "object": {"sha": "%1", "type": "commit"}})")
                   .arg(sha)
                   .toUtf8());
    }
};

std::optional<std::pair<app::UpdateResult, bool>> waitFor(UpdateChecker& checker, bool byHand) {
    std::optional<std::pair<app::UpdateResult, bool>> got;
    QObject::connect(&checker, &UpdateChecker::finished,
                     [&](const app::UpdateResult& r, bool hand) { got = std::pair{r, hand}; });
    checker.check(byHand);
    QElapsedTimer t;
    t.start();
    while (!got && t.elapsed() < 5000) QApplication::processEvents(QEventLoop::AllEvents, 20);
    return got;
}

}  // namespace

TEST_CASE("Versions compare number by number", "[app][updates]") {
    CHECK(app::compareVersions(QStringLiteral("0.1.0"), QStringLiteral("0.1.0")) == 0);
    CHECK(app::compareVersions(QStringLiteral("v0.1.0"), QStringLiteral("0.1.0")) == 0);
    CHECK(app::compareVersions(QStringLiteral("0.2.0"), QStringLiteral("0.1.9")) > 0);
    CHECK(app::compareVersions(QStringLiteral("0.10.0"), QStringLiteral("0.9.0")) > 0);  // not as text
    CHECK(app::compareVersions(QStringLiteral("1.0"), QStringLiteral("1.0.0")) == 0);
    CHECK(app::compareVersions(QStringLiteral("0.1.0-rc1"), QStringLiteral("0.1.0")) == 0);
    CHECK(app::compareVersions(QStringLiteral("0.0.1"), QStringLiteral("v1.0.0")) < 0);
}

TEST_CASE("GitHub's answer about the newest release is read and compared", "[app][updates]") {
    const QByteArray newer =
        R"({"tag_name": "v0.2.0", "html_url": "https://github.com/x/y/releases/tag/v0.2.0"})";
    auto r = app::readLatestRelease(newer, QStringLiteral("0.1.0"));
    CHECK(r.kind == Kind::Newer);
    CHECK(r.version == QStringLiteral("0.2.0"));
    CHECK(r.url == QUrl(QStringLiteral("https://github.com/x/y/releases/tag/v0.2.0")));
    CHECK(r.message.contains(QStringLiteral("SketchBillet 0.2.0 is out. You have 0.1.0.")));

    CHECK(app::readLatestRelease(newer, QStringLiteral("0.2.0")).kind == Kind::Newest);
    CHECK(app::readLatestRelease(newer, QStringLiteral("0.3.0")).kind == Kind::Newest);  // built ahead

    // Only https pages are opened; anything else goes to the download page.
    r = app::readLatestRelease(R"({"tag_name": "v9.0.0", "html_url": "file:///etc/passwd"})",
                               QStringLiteral("0.1.0"));
    CHECK(r.url == QUrl(QStringLiteral("https://sketchbillet.org/download/")));

    for (const QByteArray& bad :
         {QByteArray("not json"), QByteArray("{}"), QByteArray(R"({"tag_name": "v"})")}) {
        r = app::readLatestRelease(bad, QStringLiteral("0.1.0"));
        CHECK(r.kind == Kind::Failed);
        CHECK(r.message.contains(QStringLiteral("Check that this computer is online")));
    }
}

TEST_CASE("A preview build compares its commit with the preview tag's", "[app][updates]") {
    const QByteArray tag = R"({"object": {"sha": "1531bb5aed695c0d7be968ed5929bf9b98a2b673"}})";
    CHECK(app::readPreviewTag(tag, QStringLiteral("1531bb5aed695c0d7be968ed5929bf9b98a2b673")).kind ==
          Kind::Newest);
    CHECK(app::readPreviewTag(tag, QStringLiteral("1531bb5")).kind == Kind::Newest);
    const auto r = app::readPreviewTag(tag, QStringLiteral("693ce4fdaf0c7f7eead5bfa819c0ee4305855446"));
    CHECK(r.kind == Kind::Newer);
    CHECK(r.version.isEmpty());
    CHECK(r.url.toString().endsWith(QStringLiteral("/releases/tag/preview")));
    CHECK(app::readPreviewTag("{}", QStringLiteral("1531bb5")).kind == Kind::Failed);
}

TEST_CASE("Daily checks: asked once, at most once a day, and a school can turn them off", "[app][updates]") {
    QTemporaryDir dir;
    QSettings settings(dir.filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
    const auto now = QDateTime::currentDateTimeUtc();

    SECTION("a copy built by hand never asks or checks by itself") {
        UpdateChecker checker(settings, UpdateChannel::None, QStringLiteral("0.1.0"), QString());
        CHECK_FALSE(checker.shouldAsk());
        checker.setDaily(true);
        CHECK_FALSE(checker.daily());
        CHECK_FALSE(checker.due(now));
    }
    SECTION("a release asks until answered, then checks once a day") {
        UpdateChecker checker(settings, UpdateChannel::Release, QStringLiteral("0.1.0"), QString());
        CHECK(checker.shouldAsk());
        CHECK_FALSE(checker.due(now));  // no answer yet: no check
        checker.setDaily(false);
        CHECK_FALSE(checker.shouldAsk());
        CHECK_FALSE(checker.due(now));
        checker.setDaily(true);
        CHECK(checker.due(now));  // never checked
        settings.setValue(QStringLiteral("updates/lastCheck"), now.addSecs(-3600));
        CHECK_FALSE(checker.due(now));
        settings.setValue(QStringLiteral("updates/lastCheck"), now.addDays(-1));
        CHECK(checker.due(now));
        settings.setValue(QStringLiteral("updates/lastCheck"), now.addDays(3));  // clock was wrong
        CHECK(checker.due(now));
    }
    SECTION("updates/allowed = false: never asks, never checks") {
        settings.setValue(QStringLiteral("updates/allowed"), false);
        settings.setValue(QStringLiteral("updates/daily"), true);
        UpdateChecker checker(settings, UpdateChannel::Release, QStringLiteral("0.1.0"), QString());
        CHECK_FALSE(checker.allowed());
        CHECK_FALSE(checker.shouldAsk());
        CHECK_FALSE(checker.daily());
        CHECK_FALSE(checker.due(now));
    }
}

TEST_CASE("A check asks the server and says what it found", "[app][updates]") {
    QTemporaryDir dir;
    QSettings settings(dir.filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
    FakeGitHub github;

    SECTION("a newer release") {
        github.release(QStringLiteral("v0.2.0"));
        UpdateChecker checker(settings, UpdateChannel::Release, QStringLiteral("0.1.0"), QString());
        checker.setServer(github.url());
        const auto got = waitFor(checker, false);
        REQUIRE(got);
        CHECK(got->first.kind == Kind::Newer);
        CHECK(got->first.version == QStringLiteral("0.2.0"));
        CHECK_FALSE(got->second);
        // A daily check is remembered, so the next waits a day; one by hand isn't.
        CHECK(settings.value(QStringLiteral("updates/lastCheck")).toDateTime().isValid());
    }
    SECTION("no releases yet (GitHub says not found): this is the newest") {
        UpdateChecker checker(settings, UpdateChannel::Release, QStringLiteral("0.1.0"), QString());
        checker.setServer(github.url());
        const auto got = waitFor(checker, true);
        REQUIRE(got);
        CHECK(got->first.kind == Kind::Newest);
        CHECK(got->first.message == QStringLiteral("You have the newest SketchBillet (0.1.0)."));
        CHECK_FALSE(settings.contains(QStringLiteral("updates/lastCheck")));
    }
    SECTION("a preview build asks about the preview tag") {
        github.preview(QStringLiteral("aaaa"));
        UpdateChecker checker(settings, UpdateChannel::Preview, QStringLiteral("0.1.0"),
                              QStringLiteral("bbbb"));
        checker.setServer(github.url());
        const auto got = waitFor(checker, true);
        REQUIRE(got);
        CHECK(got->first.kind == Kind::Newer);
    }
    SECTION("the preview tag can't be found: the check failed") {
        UpdateChecker checker(settings, UpdateChannel::Preview, QStringLiteral("0.1.0"),
                              QStringLiteral("bbbb"));
        checker.setServer(github.url());
        const auto got = waitFor(checker, true);
        REQUIRE(got);
        CHECK(got->first.kind == Kind::Failed);
    }
}

TEST_CASE("Help > Check for Updates, and the note a daily check leaves", "[app][updates]") {
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
    std::vector<app::UpdateResult> boxes;
    shell.showUpdateBox = [&](const app::UpdateResult& r) { boxes.push_back(r); };
    FakeGitHub github;
    github.release(QStringLiteral("v0.2.0"));

    auto settle = [] {
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < 300) QApplication::processEvents(QEventLoop::AllEvents, 20);
    };

    SECTION("a release with daily checks on: the menu items, then a note in the status bar") {
        settings.setValue(QStringLiteral("updates/daily"), true);
        UpdateChecker checker(settings, UpdateChannel::Release, QStringLiteral("0.1.0"), QString());
        checker.setServer(github.url());
        shell.setUpdates(&checker);
        auto* daily = shell.findChild<QAction*>(QStringLiteral("sb.shell.dailyupdates"));
        REQUIRE(daily);
        CHECK(daily->isChecked());
        settle();
        CHECK(boxes.empty());  // a daily check doesn't open a box
        auto* note = shell.findChild<QLabel*>(QStringLiteral("sb.shell.updatenote"));
        REQUIRE(note);
        CHECK(note->text().contains(QStringLiteral("SketchBillet 0.2.0 is out: get it")));
        CHECK(note->text().contains(
            QStringLiteral("https://github.com/ddreeves70/sketchbillet/releases/tag/v0.2.0")));

        daily->setChecked(false);
        CHECK(settings.value(QStringLiteral("updates/daily")).toBool() == false);

        auto* now = shell.findChild<QAction*>(QStringLiteral("sb.shell.checkupdates"));
        REQUIRE(now);
        now->trigger();
        settle();
        REQUIRE(boxes.size() == 1);
        CHECK(boxes[0].kind == Kind::Newer);
    }
    SECTION("the newest already: a daily check stays quiet, a check by hand says so") {
        settings.setValue(QStringLiteral("updates/daily"), true);
        UpdateChecker checker(settings, UpdateChannel::Release, QStringLiteral("0.2.0"), QString());
        checker.setServer(github.url());
        shell.setUpdates(&checker);
        settle();
        CHECK(shell.findChild<QLabel*>(QStringLiteral("sb.shell.updatenote")) == nullptr);
        shell.findChild<QAction*>(QStringLiteral("sb.shell.checkupdates"))->trigger();
        settle();
        REQUIRE(boxes.size() == 1);
        CHECK(boxes[0].message == QStringLiteral("You have the newest SketchBillet (0.2.0)."));
    }
    SECTION("built by hand: Check for Updates, but no daily checks") {
        UpdateChecker checker(settings, UpdateChannel::None, QStringLiteral("0.1.0"), QString());
        checker.setServer(github.url());
        shell.setUpdates(&checker);
        CHECK(shell.findChild<QAction*>(QStringLiteral("sb.shell.checkupdates")));
        CHECK(shell.findChild<QAction*>(QStringLiteral("sb.shell.dailyupdates")) == nullptr);
    }
    SECTION("turned off by the school: no menu items") {
        settings.setValue(QStringLiteral("updates/allowed"), false);
        UpdateChecker checker(settings, UpdateChannel::Release, QStringLiteral("0.1.0"), QString());
        shell.setUpdates(&checker);
        CHECK(shell.findChild<QAction*>(QStringLiteral("sb.shell.checkupdates")) == nullptr);
        CHECK(shell.findChild<QAction*>(QStringLiteral("sb.shell.dailyupdates")) == nullptr);
    }
}

TEST_CASE("GitHub's error answers are explained in plain words", "[app][updates]") {
    // A web server that answers every request with one HTTP status, like GitHub
    // does when a network has asked too often (403).
    for (const int status : {403, 500}) {
        QTcpServer server;
        REQUIRE(server.listen(QHostAddress::LocalHost));
        QObject::connect(&server, &QTcpServer::newConnection, [&server, status] {
            auto* socket = server.nextPendingConnection();
            QObject::connect(socket, &QTcpSocket::readyRead, [socket, status] {
                socket->readAll();
                socket->write(
                    QStringLiteral("HTTP/1.1 %1 Nope\r\nContent-Length: 0\r\nConnection: close\r\n\r\n")
                        .arg(status)
                        .toLatin1());
                socket->disconnectFromHost();
            });
        });
        QTemporaryDir dir;
        QSettings settings(dir.filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
        UpdateChecker checker(settings, UpdateChannel::Preview, QStringLiteral("0.1.0"),
                              QStringLiteral("bbbb"));
        checker.setServer(QUrl(QStringLiteral("http://127.0.0.1:%1/repos/x/y").arg(server.serverPort())));
        const auto got = waitFor(checker, true);
        REQUIRE(got);
        CHECK(got->first.kind == Kind::Failed);
        if (status == 403)
            CHECK(got->first.message.contains(
                QStringLiteral("too many questions from this network for now (error "
                               "403). Try Help > Check for Updates again in an hour.")));
        else
            CHECK(got->first.message.contains(QStringLiteral("GitHub answered with error 500. Check that")));
    }
}
