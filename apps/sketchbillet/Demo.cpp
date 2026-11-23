#include "Demo.h"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QCursor>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QKeySequence>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QRegion>
#include <QTabBar>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QWheelEvent>
#include <QWindow>
#include <algorithm>
#include <cmath>
#include <memory>

#include "Shell.h"

namespace sb::app {

namespace {

// A ring that grows where the pointer clicks, so viewers see the click. It's a
// window shaped like a ring (no see-through needed, so it works without a
// compositor), and clicks pass through it.
class Ring : public QWidget {
public:
    Ring()
        : QWidget(nullptr, Qt::ToolTip | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint |
                               Qt::WindowDoesNotAcceptFocus | Qt::X11BypassWindowManagerHint) {
        setObjectName(QStringLiteral("sb.demo.ring"));
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_ShowWithoutActivating);
        resize(2 * kMax + 4, 2 * kMax + 4);
    }
    void play(QPoint at) {
        move(at - QPoint(width() / 2, height() / 2));
        radius_ = 6;
        shape();
        show();
        raise();
        timer_.start();
        auto* tick = new QTimer(this);
        connect(tick, &QTimer::timeout, this, [this, tick] {
            const double t = std::min(1.0, static_cast<double>(timer_.elapsed()) / 380.0);
            radius_ = 6 + t * (kMax - 6);
            if (t >= 1.0) {
                tick->stop();
                tick->deleteLater();
                hide();
                return;
            }
            shape();
            update();
        });
        tick->start(16);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.fillRect(rect(), QColor(255, 140, 0));  // only the ring shows: see shape()
    }

private:
    static constexpr int kMax = 26;
    void shape() {
        const QPoint c(width() / 2, height() / 2);
        const int r = static_cast<int>(radius_);
        const QRegion outer(QRect(c.x() - r, c.y() - r, 2 * r, 2 * r), QRegion::Ellipse);
        const QRegion inner(QRect(c.x() - r + 3, c.y() - r + 3, 2 * r - 6, 2 * r - 6), QRegion::Ellipse);
        setMask(outer.subtracted(inner));
    }
    double radius_ = 6;
    QElapsedTimer timer_;
};

QString plain(QString text) {
    return text.remove(QLatin1Char('&')).trimmed();
}

// The visible widgets called `name`: in the open popup or dialog first.
QWidget* findNamed(const QString& name) {
    QList<QWidget*> roots;
    if (auto* p = QApplication::activePopupWidget()) roots << p;
    if (auto* m = QApplication::activeModalWidget()) roots << m;
    roots << QApplication::topLevelWidgets();
    for (auto* root : roots) {
        if (!root->isVisible()) continue;
        if (root->objectName() == name) return root;
        for (auto* w : root->findChildren<QWidget*>(name)) {
            if (w->isVisible()) return w;
        }
    }
    return nullptr;
}

QPoint center(QWidget* w, QRect r) {
    return w->mapToGlobal(r.center());
}

}  // namespace

DemoPlayer::DemoPlayer(Shell& shell, QJsonArray steps, double speed, QString logFile)
    : QObject(&shell), shell_(shell), steps_(std::move(steps)), speed_(speed) {
    qApp->installEventFilter(this);
    if (!logFile.isEmpty()) {
        log_ = new QFile(logFile, this);
        if (!log_->open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
            delete log_;
            log_ = nullptr;
        }
    }
}

DemoPlayer::~DemoPlayer() {
    delete ring_;
}

bool DemoPlayer::eventFilter(QObject* watched, QEvent* event) {
    // Moving the pointer makes the screen report moves with no button held. While
    // the demo drags, those would end the drag, so they're dropped: the drag's own
    // moves (sent, not from the screen) say the button is down.
    // (They come to the window from the screen; what the window passes on to its
    // widgets is marked as coming from the screen too, so only windows are watched.)
    if (dragging_ && event->spontaneous() && watched->isWindowType()) {
        switch (event->type()) {
            case QEvent::MouseMove:
            case QEvent::MouseButtonPress:
            case QEvent::MouseButtonRelease:
                return true;
            default:
                break;
        }
    }
    return QObject::eventFilter(watched, event);
}

int DemoPlayer::ms(double seconds) const {
    // Played fast, a rest is still long enough for panels to catch up.
    return speed_ <= 0 ? 20 : static_cast<int>(seconds * 1000 / speed_);
}

void DemoPlayer::log(const QString& event, const QString& text) {
    if (!log_) return;
    QJsonObject o{{QStringLiteral("t"), QDateTime::currentMSecsSinceEpoch()},
                  {QStringLiteral("event"), event}};
    if (!text.isEmpty()) o[QStringLiteral("text")] = text;
    log_->write(QJsonDocument(o).toJson(QJsonDocument::Compact) + '\n');
    log_->flush();
}

void DemoPlayer::start() {
    pointer_ = QCursor::pos();
    shell_.activateWindow();  // with no window manager, nothing else gives it the keyboard
    QTimer::singleShot(ms(1.0), this, [this] {
        log(QStringLiteral("start"));
        next();
    });
}

void DemoPlayer::fail(const QString& why) {
    problem_ = QStringLiteral("Step %1: %2").arg(at_).arg(why);
    log(QStringLiteral("error"), problem_);
    // Close whatever is open, so the app can end.
    if (auto* p = QApplication::activePopupWidget()) p->close();
    if (auto* m = QApplication::activeModalWidget()) m->close();
    emit finished(false);
}

void DemoPlayer::next() {
    if (!problem_.isEmpty()) return;
    if (at_ >= steps_.size()) {
        QTimer::singleShot(ms(1.5), this, [this] {
            log(QStringLiteral("end"));
            emit finished(true);
        });
        return;
    }
    const auto step = steps_.at(at_++).toObject();
    tries_ = 0;
    typing_ = nullptr;
    runStep(step);
}

void DemoPlayer::runStep(const QJsonObject& step) {
    // The next step comes after a short rest. It's set going before this step
    // clicks: a click that opens a dialog or a menu doesn't return until it closes.
    auto then = [this](double rest) { QTimer::singleShot(ms(rest), this, [this] { next(); }); };

    if (step.contains(QStringLiteral("caption")) || step.contains(QStringLiteral("chapter"))) {
        const bool chapter = step.contains(QStringLiteral("chapter"));
        const QString text =
            step.value(chapter ? QStringLiteral("chapter") : QStringLiteral("caption")).toString();
        log(chapter ? QStringLiteral("chapter") : QStringLiteral("caption"), text);
        // Time to read it before anything happens: about 3 words a second.
        const auto words = text.split(QLatin1Char(' '), Qt::SkipEmptyParts).size();
        then(std::max(1.5, 0.3 * static_cast<double>(words)));
        return;
    }
    if (step.contains(QStringLiteral("wait"))) {
        then(step.value(QStringLiteral("wait")).toDouble());
        return;
    }
    if (step.contains(QStringLiteral("key"))) {
        then(0.6);
        keys(step.value(QStringLiteral("key")).toString());
        return;
    }
    if (step.contains(QStringLiteral("type"))) {
        const QString text = step.value(QStringLiteral("type")).toString();
        const bool enter = step.value(QStringLiteral("enter")).toBool(true);
        auto typeNow = [this, text, enter, then] {
            // Select what's there, so the typing replaces it.
            keys(QStringLiteral("Ctrl+A"));
            QTimer::singleShot(ms(0.15), this,
                               [this, text, enter, then] { typeText(text, enter, [then] { then(0.5); }); });
        };
        if (!step.contains(QStringLiteral("into"))) {
            typeNow();
            return;
        }
        QPoint at;
        QWidget* w = nullptr;
        if (!locate(step.value(QStringLiteral("into")).toString(), at, &w)) return;
        glide(at, [this, at, w, typeNow] {
            press(at, Qt::LeftButton, Qt::NoModifier, false);
            typing_ = w;
            if (w) {
                w->activateWindow();
                w->setFocus(Qt::MouseFocusReason);
            }
            QTimer::singleShot(ms(0.25), this, typeNow);
        });
        return;
    }
    if (step.contains(QStringLiteral("hover"))) {
        QPoint at;
        if (!locate(step.value(QStringLiteral("hover")).toString(), at)) return;
        glide(at, [then] { then(0.8); });
        return;
    }
    if (step.contains(QStringLiteral("click"))) {
        QPoint at;
        if (!locate(step.value(QStringLiteral("click")).toString(), at)) return;
        const auto mods = step.value(QStringLiteral("ctrl")).toBool() ? Qt::ControlModifier : Qt::NoModifier;
        const auto button = step.value(QStringLiteral("right")).toBool() ? Qt::RightButton : Qt::LeftButton;
        const bool twice = step.value(QStringLiteral("double")).toBool();
        glide(at, [this, at, mods, button, twice, then] {
            // After this event loop turn, and before the next step: the click may
            // open a dialog or a menu that waits, and the next step works in it.
            QTimer::singleShot(0, this, [this, at, mods, button, twice] { press(at, button, mods, twice); });
            then(0.7);
        });
        return;
    }
    if (step.contains(QStringLiteral("scroll"))) {
        QPoint at;
        if (!locate(step.value(QStringLiteral("scroll")).toString(), at)) return;
        const int notches = step.value(QStringLiteral("by")).toInt();
        glide(at, [this, at, notches, then] {
            auto left = std::make_shared<int>(std::abs(notches));
            auto* tick = new QTimer(this);
            connect(tick, &QTimer::timeout, this, [=] {
                if (*left <= 0) {
                    tick->stop();
                    tick->deleteLater();
                    then(0.6);
                    return;
                }
                --*left;
                QWidget* w = QApplication::widgetAt(at);
                QWidget* native = w;
                while (native && !native->windowHandle()) native = native->parentWidget();
                if (!native) return;
                const QPointF local = native->mapFromGlobal(QPointF(at));
                const QPoint delta(0, notches > 0 ? 120 : -120);
                QWheelEvent e(local, QPointF(at), QPoint(), delta, Qt::NoButton, Qt::NoModifier,
                              Qt::NoScrollPhase, false);
                QCoreApplication::sendEvent(native->windowHandle(), &e);
            });
            tick->start(ms(0.12));
        });
        return;
    }
    if (step.contains(QStringLiteral("drag"))) {
        QPoint from;
        if (!locate(step.value(QStringLiteral("drag")).toString(), from)) return;
        const auto by = step.value(QStringLiteral("by")).toArray();
        const QPoint to = from + QPoint(by.at(0).toInt(), by.at(1).toInt());
        const QString b = step.value(QStringLiteral("button")).toString(QStringLiteral("left"));
        const auto button = b == QStringLiteral("middle")  ? Qt::MiddleButton
                            : b == QStringLiteral("right") ? Qt::RightButton
                                                           : Qt::LeftButton;
        glide(from, [this, from, to, button, then] {
            dragging_ = true;  // see eventFilter()
            auto send = [](QPoint g, QEvent::Type type, Qt::MouseButton btn, Qt::MouseButtons held) {
                QWidget* w = QApplication::widgetAt(g);
                if (!w) return;
                QWidget* native = w;
                while (native && !native->windowHandle()) native = native->parentWidget();
                if (!native) return;
                const QPointF local = native->mapFromGlobal(QPointF(g));
                QMouseEvent e(type, local, local, QPointF(g), btn, held, Qt::NoModifier);
                QCoreApplication::sendEvent(native->windowHandle(), &e);
            };
            send(from, QEvent::MouseButtonPress, button, button);
            // Move in small steps, as a hand does.
            // A few big steps, not many small ones: each move draws the view again.
            const int n = 16;
            auto* tick = new QTimer(this);
            auto i = std::make_shared<int>(0);
            connect(tick, &QTimer::timeout, this, [=, this] {
                ++*i;
                const double t = std::min(1.0, static_cast<double>(*i) / n);
                const QPoint p = from + (to - from) * t;
                QCursor::setPos(p);
                pointer_ = p;
                send(p, QEvent::MouseMove, Qt::NoButton, button);
                if (t >= 1.0) {
                    tick->stop();
                    tick->deleteLater();
                    send(p, QEvent::MouseButtonRelease, button, Qt::NoButton);
                    dragging_ = false;
                    then(0.6);
                }
            });
            tick->start(speed_ <= 0 ? 0 : ms(0.05));
        });
        return;
    }
    fail(QStringLiteral("SketchBillet doesn't know this step: %1")
             .arg(QString::fromUtf8(QJsonDocument(step).toJson(QJsonDocument::Compact))));
}

bool DemoPlayer::locate(const QString& target, QPoint& global, QWidget** widget) {
    // What it's looking for may not be there yet (a panel that's still filling
    // in): look again for up to 3 seconds before giving up.
    auto fail = [this](const QString& why) {
        if (tries_ < 60) {
            ++tries_;
            QTimer::singleShot(50, this, [this] { runStep(steps_.at(at_ - 1).toObject()); });
        } else {
            DemoPlayer::fail(why);
        }
    };
    auto found = [&](QWidget* w, QPoint g) {
        global = g;
        if (widget) *widget = w;
        return true;
    };
    const auto colon = target.indexOf(QLatin1Char(':'));
    const QString kind = colon < 0 ? target : target.left(colon);
    const QString what = colon < 0 ? QString() : target.mid(colon + 1);

    if (target.startsWith(QLatin1Char('#'))) {
        if (auto* w = findNamed(target.mid(1))) return found(w, center(w, w->rect()));
        fail(QStringLiteral("there's nothing visible called %1").arg(target.mid(1)));
        return false;
    }
    if (kind == QStringLiteral("action")) {
        // A plugin's item has a button action of its own ("<id>.button"); the app's own don't.
        auto* w = findNamed(what + QStringLiteral(".button.tool"));
        if (!w) w = findNamed(what + QStringLiteral(".tool"));
        // Or any button showing the action (the view bar's, say).
        for (auto* top : QApplication::topLevelWidgets()) {
            if (w || !top->isVisible()) continue;
            for (auto* b : top->findChildren<QToolButton*>()) {
                if (b->isVisible() && b->defaultAction() && b->defaultAction()->objectName() == what) {
                    w = b;
                    break;
                }
            }
        }
        if (w) {
            if (!w->isEnabled()) {
                fail(QStringLiteral("%1 is greyed out").arg(what));
                return false;
            }
            return found(w, center(w, w->rect()));
        }
        fail(QStringLiteral("%1 has no button on the toolbar's open tab").arg(what));
        return false;
    }
    if (kind == QStringLiteral("tree")) {
        auto* tree = qobject_cast<QTreeWidget*>(findNamed(QStringLiteral("sb.featuretree.tree")));
        const auto items = tree ? tree->findItems(what, Qt::MatchExactly | Qt::MatchRecursive, 0)
                                : QList<QTreeWidgetItem*>{};
        if (items.isEmpty()) {
            fail(QStringLiteral("the feature tree has no row called %1").arg(what));
            return false;
        }
        tree->scrollToItem(items.front());
        return found(tree->viewport(), center(tree->viewport(), tree->visualItemRect(items.front())));
    }
    if (kind == QStringLiteral("menu")) {
        if (auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget())) {
            for (auto* a : menu->actions()) {
                if (plain(a->text()) == what && a->isVisible()) {
                    if (!a->isEnabled()) {
                        fail(QStringLiteral("the menu item %1 is greyed out").arg(what));
                        return false;
                    }
                    return found(menu, center(menu, menu->actionGeometry(a)));
                }
            }
        }
        fail(QStringLiteral("no open menu has an item %1").arg(what));
        return false;
    }
    if (kind == QStringLiteral("list")) {
        QAbstractItemView* view = nullptr;
        if (auto* popup = QApplication::activePopupWidget()) {
            view = qobject_cast<QAbstractItemView*>(popup);
            if (!view) view = popup->findChild<QAbstractItemView*>();
        }
        if (view && view->model()) {
            const auto hits =
                view->model()->match(view->model()->index(0, 0), Qt::DisplayRole, what, 1, Qt::MatchExactly);
            if (!hits.isEmpty()) {
                view->scrollTo(hits.front());
                return found(view->viewport(), center(view->viewport(), view->visualRect(hits.front())));
            }
        }
        fail(QStringLiteral("no open list has %1").arg(what));
        return false;
    }
    if (kind == QStringLiteral("button")) {
        QList<QWidget*> roots;
        if (auto* m = QApplication::activeModalWidget()) roots << m;
        roots << QApplication::topLevelWidgets();
        for (auto* root : roots) {
            if (!root->isVisible()) continue;
            for (auto* b : root->findChildren<QAbstractButton*>()) {
                if (b->isVisible() && plain(b->text()) == what) return found(b, center(b, b->rect()));
            }
        }
        fail(QStringLiteral("there's no button %1").arg(what));
        return false;
    }
    if (kind == QStringLiteral("tab")) {
        if (auto* bar = qobject_cast<QTabBar*>(findNamed(QStringLiteral("sb.ribbon.tabs")))) {
            for (int i = 0; i < bar->count(); ++i) {
                if (bar->isTabVisible(i) && plain(bar->tabText(i)) == what)
                    return found(bar, center(bar, bar->tabRect(i)));
            }
        }
        fail(QStringLiteral("there's no toolbar tab %1").arg(what));
        return false;
    }
    if (kind == QStringLiteral("view")) {
        auto* view = findNamed(QStringLiteral("sb.viewer.panel"));
        if (!view) {
            fail(QStringLiteral("the 3D view isn't showing"));
            return false;
        }
        double fx = 0.5, fy = 0.5;
        if (!what.isEmpty()) {
            const auto parts = what.split(QLatin1Char(','));
            fx = parts.value(0).toDouble();
            fy = parts.value(1).toDouble();
        }
        return found(view, view->mapToGlobal(QPoint(static_cast<int>(fx * view->width()),
                                                    static_cast<int>(fy * view->height()))));
    }
    fail(QStringLiteral("SketchBillet doesn't know the target %1").arg(target));
    return false;
}

void DemoPlayer::glide(QPoint to, std::function<void()> then) {
    const QPoint from = pointer_;
    const double distance = std::hypot(to.x() - from.x(), to.y() - from.y());
    const int duration = ms(std::min(1.0, 0.25 + distance / 1800.0));
    if (duration <= 0 || distance < 1) {
        QCursor::setPos(to);
        pointer_ = to;
        then();
        return;
    }
    auto* tick = new QTimer(this);
    auto clock = std::make_shared<QElapsedTimer>();
    clock->start();
    connect(tick, &QTimer::timeout, this, [this, tick, clock, from, to, duration, then] {
        const double t = std::min(1.0, static_cast<double>(clock->elapsed()) / duration);
        const double eased = t < 0.5 ? 2 * t * t : 1 - std::pow(-2 * t + 2, 2) / 2;  // slow, fast, slow
        const QPoint p = from + (to - from) * eased;
        QCursor::setPos(p);
        pointer_ = p;
        if (t >= 1.0) {
            tick->stop();
            tick->deleteLater();
            then();
        }
    });
    tick->start(16);
}

void DemoPlayer::press(QPoint g, Qt::MouseButton button, Qt::KeyboardModifiers mods, bool isDouble) {
    QWidget* w = QApplication::widgetAt(g);
    QWidget* native = w;
    while (native && !native->windowHandle()) native = native->parentWidget();
    if (!native) return;
    QWindow* window = native->windowHandle();
    const QPointF local = native->mapFromGlobal(QPointF(g));
    auto send = [&](QEvent::Type type, Qt::MouseButtons held) {
        QMouseEvent e(type, local, local, QPointF(g), button, held, mods);
        QCoreApplication::sendEvent(window, &e);
    };
    if (speed_ > 0) ripple(g);
    send(QEvent::MouseButtonPress, button);
    send(QEvent::MouseButtonRelease, Qt::NoButton);
    if (isDouble) {
        send(QEvent::MouseButtonDblClick, button);
        send(QEvent::MouseButtonRelease, Qt::NoButton);
    }
}

void DemoPlayer::ripple(QPoint g) {
    if (!ring_) ring_ = new Ring;
    static_cast<Ring*>(ring_.data())->play(g);
}

void DemoPlayer::keys(const QString& sequence) {
    const QKeySequence seq(sequence);
    if (seq.isEmpty()) return;
    const auto combo = seq[0];
    QWidget* to = typing_ ? typing_.data() : QApplication::focusWidget();
    if (!to) to = QApplication::focusWidget();
    if (!to) to = QApplication::activeWindow();
    if (!to) to = &shell_;
    QKeyEvent down(QEvent::KeyPress, combo.key(), combo.keyboardModifiers());
    QKeyEvent up(QEvent::KeyRelease, combo.key(), combo.keyboardModifiers());
    QCoreApplication::sendEvent(to, &down);
    QCoreApplication::sendEvent(to, &up);
}

void DemoPlayer::typeText(const QString& text, bool enter, std::function<void()> done) {
    // One key at a time, at a typing pace.
    auto i = std::make_shared<int>(0);
    auto* tick = new QTimer(this);
    connect(tick, &QTimer::timeout, this, [this, tick, i, text, enter, done] {
        if (*i >= text.size()) {
            tick->stop();
            tick->deleteLater();
            if (enter) keys(QStringLiteral("Return"));
            done();
            return;
        }
        const QChar c = text.at((*i)++);
        QWidget* to = typing_ ? typing_.data() : QApplication::focusWidget();
        if (!to) return;
        const int key = c.toUpper().unicode();
        QKeyEvent down(QEvent::KeyPress, key, Qt::NoModifier, QString(c));
        QKeyEvent up(QEvent::KeyRelease, key, Qt::NoModifier, QString(c));
        QCoreApplication::sendEvent(to, &down);
        QCoreApplication::sendEvent(to, &up);
    });
    tick->start(ms(0.06));
}

}  // namespace sb::app
