#pragma once

#include <QDateTime>
#include <QObject>
#include <QString>
#include <QUrl>

class QNetworkAccessManager;
class QSettings;

namespace sb::app {

// Which builds of SketchBillet this copy compares itself with. Set when it's
// built (SB_UPDATE_CHANNEL in CMake): CI builds releases from version tags and
// preview builds from main; a copy someone builds by hand has no channel.
enum class UpdateChannel {
    None,     // built by hand: never checks by itself
    Release,  // a release, like 0.1.0: compares with the newest release
    Preview,  // a preview build from main: compares with the newest preview build
};

// What one check found.
struct UpdateResult {
    enum class Kind {
        Newest,              // this copy is the newest
        Newer,               // a newer one is out: `version` (empty for a preview) at `url`
        Failed,              // couldn't reach GitHub, or it answered something unexpected
        NoSecureConnection,  // this copy can't make https connections (a missing TLS plugin)
    };
    Kind kind = Kind::Failed;
    QString version;  // the newer release's version, like "0.2.0"
    QUrl url;         // the page to download it from
    QString message;  // for the student: what was found, and what to do
};

// Compares two versions like "0.2.0" and "v0.10.1", number by number; a leading
// "v" and anything after a "-" are ignored. Returns <0, 0 or >0.
int compareVersions(const QString& a, const QString& b);

// Reads GitHub's answer for the newest release (GET /releases/latest) and
// compares it with `ours`, like "0.1.0".
UpdateResult readLatestRelease(const QByteArray& json, const QString& ours);

// Reads GitHub's answer for the preview tag (GET /git/ref/tags/preview) and
// compares the commit it points at with `ourCommit`.
UpdateResult readPreviewTag(const QByteArray& json, const QString& ourCommit);

// Checks whether a newer SketchBillet is out, by asking GitHub's API, at most
// once a day if the student said yes (the first time the app starts), or when
// they click Help > Check for Updates. It never downloads or installs anything,
// and sends nothing but the request itself.
//
// Settings (QSettings, so a school can set them for every computer):
//   updates/daily     true or false: check once a day. Not set: ask.
//   updates/allowed   false: never check, never ask, and no menu items.
//   updates/lastCheck when the last daily check ran.
class UpdateChecker : public QObject {
    Q_OBJECT
public:
    // `version` is this copy's version, like "0.1.0"; `commit` the commit it was
    // built from (for preview builds).
    UpdateChecker(QSettings& settings, UpdateChannel channel, QString version, QString commit,
                  QObject* parent = nullptr);
    ~UpdateChecker() override;

    // The channel this copy was built for (SB_UPDATE_CHANNEL), from its name.
    static UpdateChannel builtChannel();

    UpdateChannel channel() const { return channel_; }
    // Where to ask: GitHub's API for SketchBillet's repository. Tests point it at
    // a folder of answers (file:// URLs).
    void setServer(const QUrl& base) { server_ = base; }
    QUrl server() const { return server_; }

    // False when a school turned checking off (updates/allowed = false).
    bool allowed() const;
    // Whether to ask the student about daily checks: this copy can check, it's
    // allowed, and they haven't answered yet.
    bool shouldAsk() const;
    bool daily() const;
    void setDaily(bool on);
    // Whether a daily check is due at `now`: daily checks are on and the last was
    // a day or more ago.
    bool due(const QDateTime& now) const;

    // Starts a check; `finished` says what it found. A check started by the
    // student (`byHand`) works even without a channel: it compares with releases.
    void check(bool byHand);

signals:
    void finished(const sb::app::UpdateResult& result, bool byHand);

private:
    QSettings& settings_;
    UpdateChannel channel_;
    QString version_;
    QString commit_;
    QUrl server_;
    QNetworkAccessManager* network_ = nullptr;
};

}  // namespace sb::app
