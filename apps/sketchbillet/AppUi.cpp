#include "AppUi.h"

#include "Icons.h"

#include <QApplication>
#include <QFileDialog>
#include <QFileInfo>
#include <QRegularExpression>

namespace sb::app {

void AppUi::showProblems(const Diagnostics& problems) {
    if (problemSink_) {
        problemSink_(problems);
    } else {
        heldProblems_.insert(heldProblems_.end(), problems.begin(), problems.end());
    }
}

QIcon AppUi::icon(std::string_view svg) const {
    return svgIcon(svg);
}

QString AppUi::askOpenFile(const QString& title, const QString& filter) {
    if (answerOpenFile) return answerOpenFile(title, filter);
    return QFileDialog::getOpenFileName(QApplication::activeWindow(), title, QString(), filter);
}

QString AppUi::askSaveFile(const QString& title, const QString& filter, const QString& suggested) {
    QString path = answerSaveFile
                       ? answerSaveFile(title, filter, suggested)
                       : QFileDialog::getSaveFileName(QApplication::activeWindow(), title, suggested, filter);
    if (path.isEmpty() || !QFileInfo(path).suffix().isEmpty()) return path;
    // No ending typed: add the first one the filter names, like ".csv" from "(*.csv)".
    static const QRegularExpression firstEnding(QStringLiteral(R"(\*(\.\w+))"));
    if (const auto m = firstEnding.match(filter); m.hasMatch()) path += m.captured(1);
    return path;
}

void AppUi::showMessage(const QString& message) {
    if (messageSink_) messageSink_(message);
}

void AppUi::setSinks(std::function<void(const Diagnostics&)> problems,
                     std::function<void(const QString&)> message) {
    problemSink_ = std::move(problems);
    messageSink_ = std::move(message);
    if (problemSink_ && !heldProblems_.empty()) {
        problemSink_(heldProblems_);
        heldProblems_.clear();
    }
}

}  // namespace sb::app
