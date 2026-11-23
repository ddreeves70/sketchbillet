#pragma once

// Plays a demo: a script of clicks, typing and captions, done on the app the way
// a student does it, with the mouse pointer gliding to each thing it clicks. For
// making the tutorial videos (tools/videos/), and for checking their scripts
// still work (a test plays each one, fast).
//
//   sketchbillet --demo script.json [--demo-log log.jsonl] [--demo-speed 1]
//
// A script is a JSON object with "steps", a list of objects, each one of:
//
//   {"caption": "Click Box to add a box."}   what the video says from here on
//   {"chapter": "Make the plate"}             a chapter of the video, and its caption
//   {"click": "<target>"}                     glide there and click; "ctrl": true holds
//                                             Ctrl, "double": true double-clicks,
//                                             "right": true right-clicks
//   {"hover": "<target>"}                     glide there, and rest
//   {"type": "Plate", "into": "<target>"}     click a box, select what's in it, type,
//                                             and press Enter (unless "enter": false)
//   {"key": "Escape"}                         press keys, like "Ctrl+A" or "Return"
//   {"drag": "<target>", "by": [dx, dy]}      drag from a target by so many pixels
//                                             ("button": "left", "middle" or "right")
//   {"scroll": "<target>", "by": -2}          turn the mouse wheel so many notches
//                                             (up is positive)
//   {"wait": 1.5}                             rest, in seconds
//
// Targets, found when the step runs:
//
//   "action:sb.modeling.box"   a toolbar button for a menu item (by its id)
//   "#sb.ribbon.file"          the visible widget with this object name
//   "tree:Plate"               the feature tree's row for a step or component
//   "menu:Export..."           an item of the menu that's open, by its words
//   "list:PETG"                an item of the open dropdown list, by its words
//   "button:Save"              a visible button with these words (in a dialog, say)
//   "tab:Inspect"              a toolbar tab
//   "view" or "view:0.5,0.6"   the 3D view, at this fraction of its width and height
//
// The log is one JSON object per line: {"t": <ms since 1970>, "event": "start" |
// "caption" | "chapter" | "end" | "error", "text": ...}, for putting the captions
// on the video at the right times.

#include <QJsonArray>
#include <QObject>
#include <QPoint>
#include <QPointer>
#include <QString>
#include <functional>

class QFile;
class QWidget;

namespace sb::app {

class Shell;

class DemoPlayer : public QObject {
    Q_OBJECT
public:
    // `speed` 1 is a person's pace; 0 plays as fast as it can (for tests).
    DemoPlayer(Shell& shell, QJsonArray steps, double speed = 1.0, QString logFile = {});
    ~DemoPlayer() override;

    // Starts playing, after the window has had a moment to draw.
    void start();
    // Why it stopped early, or empty if it played to the end.
    QString problem() const { return problem_; }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

signals:
    // Played to the end (ok) or stopped at a step that couldn't be done.
    void finished(bool ok);

private:
    void next();
    void runStep(const QJsonObject& step);
    void fail(const QString& why);
    void log(const QString& event, const QString& text = {});
    // Finds a target's point on the screen; empty on failure (and says why).
    bool locate(const QString& target, QPoint& global, QWidget** widget = nullptr);
    // Glides the pointer to `to`, then calls `then`.
    void glide(QPoint to, std::function<void()> then);
    void press(QPoint global, Qt::MouseButton button, Qt::KeyboardModifiers mods, bool isDouble);
    void keys(const QString& sequence);
    void typeText(const QString& text, bool enter, std::function<void()> done);
    void ripple(QPoint global);
    int ms(double seconds) const;

    Shell& shell_;
    QJsonArray steps_;
    double speed_;
    int at_ = 0;
    int tries_ = 0;  // times the current step looked for its target
    QString problem_;
    QFile* log_ = nullptr;
    QPoint pointer_;
    QPointer<QWidget> ring_;
    bool dragging_ = false;
    QPointer<QWidget> typing_;  // the box being typed in, if a step clicked one
};

}  // namespace sb::app
