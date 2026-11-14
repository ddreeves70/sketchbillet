#include "Updates.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSettings>
#include <QSslSocket>
#include <QStringList>

#include <algorithm>
#include <chrono>
#include <utility>

namespace sb::app {

namespace {

const QString kDownloads = QStringLiteral("https://sketchbillet.org/download/");
const QString kPreviewPage =
    QStringLiteral("https://github.com/ddreeves70/sketchbillet/releases/tag/preview");

QStringList parts(QString v) {
    v = v.trimmed();
    if (v.startsWith(QLatin1Char('v')) || v.startsWith(QLatin1Char('V'))) v.remove(0, 1);
    if (const auto dash = v.indexOf(QLatin1Char('-')); dash >= 0) v.truncate(dash);
    return v.split(QLatin1Char('.'));
}

UpdateResult failed(const QString& why) {
    UpdateResult r;
    r.kind = UpdateResult::Kind::Failed;
    r.message = QObject::tr(
                    "SketchBillet couldn't check for a newer version: %1. Check that this computer "
                    "is online, then try Help > Check for Updates again.")
                    .arg(why);
    return r;
}

UpdateResult newest(const QString& ours) {
    UpdateResult r;
    r.kind = UpdateResult::Kind::Newest;
    r.message = QObject::tr("You have the newest SketchBillet (%1).").arg(ours);
    return r;
}

}  // namespace

int compareVersions(const QString& a, const QString& b) {
    const auto pa = parts(a);
    const auto pb = parts(b);
    for (qsizetype i = 0; i < std::max(pa.size(), pb.size()); ++i) {
        const int x = i < pa.size() ? pa[i].toInt() : 0;
        const int y = i < pb.size() ? pb[i].toInt() : 0;
        if (x != y) return x < y ? -1 : 1;
    }
    return 0;
}

UpdateResult readLatestRelease(const QByteArray& json, const QString& ours) {
    const auto doc = QJsonDocument::fromJson(json);
    const auto tag = doc.object().value(QStringLiteral("tag_name")).toString();
    if (!doc.isObject() || tag.isEmpty() || parts(tag).first().isEmpty())
        return failed(QObject::tr("GitHub's answer didn't say which version is newest"));
    if (compareVersions(tag, ours) <= 0) return newest(ours);
    UpdateResult r;
    r.kind = UpdateResult::Kind::Newer;
    r.version = parts(tag).join(QLatin1Char('.'));
    const auto page = doc.object().value(QStringLiteral("html_url")).toString();
    r.url = QUrl(page.startsWith(QStringLiteral("https://")) ? page : kDownloads);
    r.message = QObject::tr(
                    "SketchBillet %1 is out. You have %2. Download it from the release page, then "
                    "install it over this one: your designs and settings stay.")
                    .arg(r.version, ours);
    return r;
}

UpdateResult readPreviewTag(const QByteArray& json, const QString& ourCommit) {
    const auto doc = QJsonDocument::fromJson(json);
    const auto sha =
        doc.object().value(QStringLiteral("object")).toObject().value(QStringLiteral("sha")).toString();
    if (!doc.isObject() || sha.isEmpty())
        return failed(QObject::tr("GitHub's answer didn't say which preview build is newest"));
    if (!ourCommit.isEmpty() && sha.startsWith(ourCommit, Qt::CaseInsensitive)) {
        UpdateResult r;
        r.kind = UpdateResult::Kind::Newest;
        r.message = QObject::tr("You have the newest preview build of SketchBillet.");
        return r;
    }
    UpdateResult r;
    r.kind = UpdateResult::Kind::Newer;
    r.url = QUrl(kPreviewPage);
    r.message = QObject::tr(
        "A newer preview build of SketchBillet is out, with the newest changes. Download "
        "it from the preview page, then install it over this one: your designs and "
        "settings stay.");
    return r;
}

UpdateChecker::UpdateChecker(QSettings& settings, UpdateChannel channel, QString version, QString commit,
                             QObject* parent)
    : QObject(parent),
      settings_(settings),
      channel_(channel),
      version_(std::move(version)),
      commit_(std::move(commit)),
      server_(QStringLiteral("https://api.github.com/repos/ddreeves70/sketchbillet")),
      network_(new QNetworkAccessManager(this)) {}

UpdateChecker::~UpdateChecker() = default;

UpdateChannel UpdateChecker::builtChannel() {
#ifdef SB_UPDATE_CHANNEL
    const QString name = QStringLiteral(SB_UPDATE_CHANNEL);
    if (name == QStringLiteral("release")) return UpdateChannel::Release;
    if (name == QStringLiteral("preview")) return UpdateChannel::Preview;
#endif
    return UpdateChannel::None;
}

bool UpdateChecker::allowed() const {
    return settings_.value(QStringLiteral("updates/allowed"), true).toBool();
}

bool UpdateChecker::shouldAsk() const {
    return channel_ != UpdateChannel::None && allowed() &&
           !settings_.contains(QStringLiteral("updates/daily"));
}

bool UpdateChecker::daily() const {
    return channel_ != UpdateChannel::None && allowed() &&
           settings_.value(QStringLiteral("updates/daily"), false).toBool();
}

void UpdateChecker::setDaily(bool on) {
    settings_.setValue(QStringLiteral("updates/daily"), on);
}

bool UpdateChecker::due(const QDateTime& now) const {
    if (!daily()) return false;
    const auto last = settings_.value(QStringLiteral("updates/lastCheck")).toDateTime();
    return !last.isValid() || last.secsTo(now) >= 24 * 60 * 60 || last > now;
}

void UpdateChecker::check(bool byHand) {
    if (!byHand) settings_.setValue(QStringLiteral("updates/lastCheck"), QDateTime::currentDateTimeUtc());
    const bool preview = channel_ == UpdateChannel::Preview;
    QUrl url = server_;
    url.setPath(url.path() +
                (preview ? QStringLiteral("/git/ref/tags/preview") : QStringLiteral("/releases/latest")));
    if (url.scheme() == QStringLiteral("https") && !QSslSocket::supportsSsl()) {
        UpdateResult r;
        r.kind = UpdateResult::Kind::NoSecureConnection;
        r.message =
            tr("This copy of SketchBillet can't make secure connections, so it can't check for a "
               "newer version. Install SketchBillet again from https://sketchbillet.org to fix it.");
        emit finished(r, byHand);
        return;
    }
    QNetworkRequest request(url);
    // GitHub's API wants to know who's asking; nothing else is sent.
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("SketchBillet/%1").arg(version_));
    request.setRawHeader("Accept", "application/vnd.github+json");
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
    request.setTransferTimeout(std::chrono::seconds(15));
#else
    request.setTransferTimeout(15000);
#endif
    auto* reply = network_->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, preview, byHand] {
        reply->deleteLater();
        const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        UpdateResult r;
        if (!preview && (status == 404 || reply->error() == QNetworkReply::ContentNotFoundError)) {
            r = newest(version_);  // no releases yet
        } else if (status == 403 || status == 429) {
            // GitHub answers each network a limited number of times an hour.
            r.kind = UpdateResult::Kind::Failed;
            r.message = tr("SketchBillet couldn't check for a newer version: GitHub has had too many "
                           "questions from this network for now (error %1). Try Help > Check for Updates "
                           "again in an hour.")
                            .arg(status);
        } else if (status >= 400) {
            r = failed(tr("GitHub answered with error %1").arg(status));
        } else if (reply->error() != QNetworkReply::NoError) {
            r = failed(reply->errorString());
        } else if (preview) {
            r = readPreviewTag(reply->readAll(), commit_);
        } else {
            r = readLatestRelease(reply->readAll(), version_);
        }
        emit finished(r, byHand);
    });
}

}  // namespace sb::app
