#include "Ribbon.h"

#include <QAction>
#include <QContextMenuEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QMenu>
#include <QStackedWidget>
#include <QTabBar>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

namespace sb::app {

namespace {

QString key(const QString& name) {
    return name.toLower().remove(QLatin1Char(' '));
}

}  // namespace

Ribbon::Ribbon(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("sb.ribbon"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // The top row: File, the quick buttons, then Help at the right.
    auto* top = new QWidget(this);
    top->setObjectName(QStringLiteral("sb.ribbon.top"));
    top_ = new QHBoxLayout(top);
    top_->setContentsMargins(6, 2, 6, 2);
    top_->setSpacing(2);
    auto* file = new QToolButton(top);
    file->setObjectName(QStringLiteral("sb.ribbon.file"));
    file->setText(tr("File"));
    file->setPopupMode(QToolButton::InstantPopup);
    file->setAutoRaise(true);
    fileMenu_ = new QMenu(tr("File"), file);
    fileMenu_->setObjectName(QStringLiteral("sb.ribbon.file.menu"));
    file->setMenu(fileMenu_);
    top_->addWidget(file);
    top_->addStretch(1);
    auto* help = new QToolButton(top);
    help->setObjectName(QStringLiteral("sb.ribbon.help"));
    help->setText(tr("Help"));
    help->setPopupMode(QToolButton::InstantPopup);
    help->setAutoRaise(true);
    helpMenu_ = new QMenu(tr("Help"), help);
    helpMenu_->setObjectName(QStringLiteral("sb.ribbon.help.menu"));
    help->setMenu(helpMenu_);
    top_->addWidget(help);
    layout->addWidget(top);

    bar_ = new QTabBar(this);
    bar_->setObjectName(QStringLiteral("sb.ribbon.tabs"));
    bar_->setDocumentMode(true);
    bar_->setExpanding(false);
    bar_->setDrawBase(false);
    layout->addWidget(bar_);

    pages_ = new QStackedWidget(this);
    pages_->setObjectName(QStringLiteral("sb.ribbon.pages"));
    layout->addWidget(pages_);
    auto* line = new QFrame(this);
    line->setFrameShape(QFrame::HLine);
    line->setFrameShadow(QFrame::Sunken);
    layout->addWidget(line);

    connect(bar_, &QTabBar::currentChanged, this, [this](int index) {
        if (placing_ || index < 0) return;
        const auto name = bar_->tabData(index).toString();
        for (auto& t : tabs_) {
            if (t.name == name) pages_->setCurrentWidget(t.page);
        }
        if (mode_.isEmpty()) usualTab_ = name;
    });
}

void Ribbon::addQuick(QAction* action) {
    auto* b = new QToolButton(this);
    b->setDefaultAction(action);
    b->setAutoRaise(true);
    b->setToolButtonStyle(Qt::ToolButtonIconOnly);
    b->setIconSize(QSize(20, 20));
    b->setObjectName(action->objectName().isEmpty() ? QString()
                                                    : action->objectName() + QStringLiteral(".quick"));
    top_->insertWidget(quickAt_++, b);
}

Ribbon::Tab& Ribbon::tab(const QString& name) {
    for (auto& t : tabs_) {
        if (t.name == name) return t;
    }
    Tab t;
    t.name = name;
    t.page = new QWidget(pages_);
    t.page->setObjectName(QStringLiteral("sb.ribbon.") + key(name));
    t.panels = new QHBoxLayout(t.page);
    t.panels->setContentsMargins(4, 4, 4, 2);
    t.panels->setSpacing(0);
    t.panels->addStretch(1);  // panels go before it, the main button after
    pages_->addWidget(t.page);
    tabs_.push_back(t);
    placeTabs();
    return tabs_.back();
}

Ribbon::Panel& Ribbon::panel(Tab& t, const QString& name) {
    for (auto& p : t.panelList) {
        if (p.name == name) return p;
    }
    Panel p;
    p.name = name;
    p.box = new QWidget(t.page);
    p.box->setObjectName(QStringLiteral("sb.ribbon.%1.%2.panel").arg(key(t.name), key(name)));
    auto* v = new QVBoxLayout(p.box);
    v->setContentsMargins(4, 0, 4, 0);
    v->setSpacing(0);
    auto* row = new QWidget(p.box);
    p.buttons = new QHBoxLayout(row);
    p.buttons->setContentsMargins(0, 0, 0, 0);
    p.buttons->setSpacing(1);
    v->addWidget(row, 1, Qt::AlignHCenter | Qt::AlignTop);
    // The panel's name, underneath: a dropdown of everything in the panel.
    p.drop = new QToolButton(p.box);
    p.drop->setObjectName(QStringLiteral("sb.ribbon.%1.%2").arg(key(t.name), key(name)));
    p.drop->setText(name);
    p.drop->setToolTip(tr("Everything in %1").arg(name));
    p.drop->setPopupMode(QToolButton::InstantPopup);
    p.drop->setAutoRaise(true);
    QFont small = p.drop->font();
    small.setPointSizeF(small.pointSizeF() * 0.9);
    p.drop->setFont(small);
    p.menu = new QMenu(name, p.drop);
    p.menu->setObjectName(p.drop->objectName() + QStringLiteral(".menu"));
    p.menu->setToolTipsVisible(true);
    p.menu->installEventFilter(this);  // right-click an item to pin it
    p.drop->setMenu(p.menu);
    v->addWidget(p.drop, 0, Qt::AlignHCenter | Qt::AlignBottom);
    t.panelList.push_back(p);
    placePanels(t);
    return t.panelList.back();
}

// Lays out a tab's panels in order, with a thin line between them, before the
// stretch (and the main button after it).
void Ribbon::placePanels(Tab& t) {
    for (auto& p : t.panelList) t.panels->removeWidget(p.box);
    for (auto* line : t.lines) {
        t.panels->removeWidget(line);
        line->hide();
        line->deleteLater();
    }
    t.lines.clear();
    std::vector<Panel*> sorted;
    for (const auto& name : t.order) {
        for (auto& p : t.panelList) {
            if (p.name == name) sorted.push_back(&p);
        }
    }
    for (auto& p : t.panelList) {
        if (!t.order.contains(p.name)) sorted.push_back(&p);
    }
    int at = 0;
    for (auto* p : sorted) {
        if (at > 0) {
            auto* line = new QFrame(t.page);
            line->setFrameShape(QFrame::VLine);
            line->setFrameShadow(QFrame::Sunken);
            t.panels->insertWidget(at++, line);
            t.lines.push_back(line);
        }
        t.panels->insertWidget(at++, p->box);
    }
}

void Ribbon::setPanelOrder(const QString& name, const QStringList& order) {
    auto& t = tab(name);
    t.order = order;
    placePanels(t);
}

void Ribbon::addTab(const QString& name) {
    tab(name);
}

void Ribbon::setTabOrder(const QStringList& order) {
    order_ = order;
    placeTabs();
}

void Ribbon::setTabMode(const QString& name, bool always, const QString& mode) {
    auto& t = tab(name);
    t.always = always;
    t.mode = mode;
    placeTabs();
}

bool Ribbon::shows(const Tab& t) const {
    return t.always || t.mode == mode_;
}

// Puts the tabs in the bar in order, showing only those for the mode that's on.
void Ribbon::placeTabs() {
    std::vector<const Tab*> sorted;
    for (const auto& name : order_) {
        for (const auto& t : tabs_) {
            if (t.name == name) sorted.push_back(&t);
        }
    }
    for (const auto& t : tabs_) {
        if (!order_.contains(t.name)) sorted.push_back(&t);
    }
    const QString current = currentTab();
    placing_ = true;
    while (bar_->count() > 0) bar_->removeTab(0);
    for (const auto* t : sorted) {
        const int i = bar_->addTab(t->name);
        bar_->setTabData(i, t->name);
        bar_->setTabVisible(i, shows(*t));
    }
    // Keep the tab the student had, if it still shows; else the first that does.
    int pick = -1;
    for (int i = 0; i < bar_->count(); ++i) {
        if (!bar_->isTabVisible(i)) continue;
        if (pick < 0 || bar_->tabData(i).toString() == current) pick = i;
        if (bar_->tabData(i).toString() == current) break;
    }
    if (pick >= 0) {
        bar_->setCurrentIndex(pick);
        const auto name = bar_->tabData(pick).toString();
        for (auto& t : tabs_) {
            if (t.name == name) pages_->setCurrentWidget(t.page);
        }
    }
    placing_ = false;  // only now: putting tabs back isn't the student picking one
}

QMenu* Ribbon::panelMenu(const QString& tabName, const QString& panelName, const QStringList& path) {
    auto& p = panel(tab(tabName), panelName);
    QMenu* m = p.menu;
    QString sofar;
    for (const auto& part : path) {
        sofar += QLatin1Char('/') + part;
        auto& sub = p.submenus[sofar];
        if (!sub) sub = m->addMenu(part);
        m = sub;
    }
    return m;
}

void Ribbon::addTool(const QString& tabName, const QString& panelName, QAction* item, QAction* button,
                     bool onPanel) {
    auto& p = panel(tab(tabName), panelName);
    p.menu->addAction(item);
    auto* b = new QToolButton(p.box);
    b->setDefaultAction(button);
    b->setAutoRaise(true);
    b->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);  // pictures, with words for new users
    b->setIconSize(QSize(24, 24));
    if (!button->objectName().isEmpty()) b->setObjectName(button->objectName() + QStringLiteral(".tool"));
    // Right-click a button to take it off the panel.
    b->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(b, &QToolButton::customContextMenuRequested, this,
            [this, tabName, panelName, item, b](const QPoint& at) {
                askPin(tabName, panelName, item, b->mapToGlobal(at));
            });
    p.buttons->addWidget(b);
    p.tools.push_back(
        {QStringLiteral("%1/%2/%3").arg(tabName, panelName, item->objectName()), item, b, onPanel});
    showTools();
}

void Ribbon::addPinHints() {
    for (auto& t : tabs_) {
        for (auto& p : t.panelList) {
            if (p.tools.empty()) continue;
            p.menu->addSeparator();
            auto* hint = p.menu->addAction(tr("Right-click a tool to pin it to the panel"));
            hint->setObjectName(p.menu->objectName() + QStringLiteral(".hint"));
            hint->setEnabled(false);
        }
    }
}

Ribbon::Tool* Ribbon::findTool(const QString& tabName, const QString& panelName, QAction* item) {
    for (auto& t : tabs_) {
        if (t.name != tabName) continue;
        for (auto& p : t.panelList) {
            if (p.name != panelName) continue;
            for (auto& tool : p.tools) {
                if (tool.item == item) return &tool;
            }
        }
    }
    return nullptr;
}

const Ribbon::Tool* Ribbon::findTool(const QString& tabName, const QString& panelName, QAction* item) const {
    return const_cast<Ribbon*>(this)->findTool(tabName, panelName, item);
}

bool Ribbon::onPanel(const QString& tabName, const QString& panelName, QAction* item) const {
    const auto* tool = findTool(tabName, panelName, item);
    if (!tool) return false;
    return pinned_.contains(tool->key) || (tool->byDefault && !unpinned_.contains(tool->key));
}

void Ribbon::showTools() {
    for (auto& t : tabs_) {
        for (auto& p : t.panelList) {
            for (auto& tool : p.tools) tool.button->setVisible(onPanel(t.name, p.name, tool.item));
        }
    }
}

void Ribbon::setPins(const QStringList& pinned, const QStringList& unpinned) {
    pinned_ = pinned;
    unpinned_ = unpinned;
    showTools();
}

void Ribbon::pin(const QString& tabName, const QString& panelName, QAction* item, bool on) {
    const auto* tool = findTool(tabName, panelName, item);
    if (!tool) return;
    pinned_.removeAll(tool->key);
    unpinned_.removeAll(tool->key);
    // Only what differs from how the tool started is kept.
    if (on && !tool->byDefault) pinned_ << tool->key;
    if (!on && tool->byDefault) unpinned_ << tool->key;
    showTools();
    emit pinsChanged();
}

void Ribbon::resetPins() {
    pinned_.clear();
    unpinned_.clear();
    showTools();
    emit pinsChanged();
}

// The small menu for right-clicking a tool: pin it to its panel, or unpin it.
void Ribbon::askPin(const QString& tabName, const QString& panelName, QAction* item, const QPoint& at) {
    const bool on = onPanel(tabName, panelName, item);
    QMenu m(this);
    const QString name = item->text().remove(QLatin1Char('&')).remove(QStringLiteral("..."));
    auto* a = m.addAction(on ? tr("Unpin %1 from %2").arg(name, panelName)
                             : tr("Pin %1 to %2").arg(name, panelName));
    a->setObjectName(on ? QStringLiteral("sb.ribbon.unpin") : QStringLiteral("sb.ribbon.pin"));
    if (m.exec(at) == a) pin(tabName, panelName, item, !on);
}

// Right-clicking an item in a panel's dropdown pins it (or unpins it).
bool Ribbon::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::ContextMenu) {
        auto* menu = qobject_cast<QMenu*>(watched);
        for (auto& t : tabs_) {
            for (auto& p : t.panelList) {
                if (p.menu != menu) continue;
                auto* e = static_cast<QContextMenuEvent*>(event);
                if (auto* item = menu->actionAt(e->pos()); item && findTool(t.name, p.name, item)) {
                    askPin(t.name, p.name, item, e->globalPos());
                    return true;
                }
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

void Ribbon::setPrimary(const QString& tabName, QAction* action) {
    auto& t = tab(tabName);
    if (!t.primary) {
        t.primary = new QToolButton(t.page);
        t.primary->setObjectName(QStringLiteral("sb.ribbon.%1.primary").arg(key(tabName)));
        t.primary->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        t.primary->setIconSize(QSize(20, 20));
        // Green, like a "done" button, so it's easy to find in the same place every time.
        t.primary->setStyleSheet(
            QStringLiteral("QToolButton { background: #1f7a4d; color: #ffffff; border: 0; border-radius: 6px;"
                           " padding: 8px 14px; font-weight: 600; }"
                           "QToolButton:hover { background: #18633e; }"
                           "QToolButton:disabled { background: #8a93a0; }"));
        t.panels->addWidget(t.primary, 0, Qt::AlignVCenter);
    }
    t.primary->setDefaultAction(action);
}

void Ribbon::setMode(const QString& mode) {
    if (mode == mode_) return;
    const QString before = currentTab();
    if (mode_.isEmpty()) usualTab_ = before;  // to go back to when the mode ends
    mode_ = mode;
    placeTabs();
    if (!mode.isEmpty()) {
        // A tab of this mode's own is picked when the mode turns on.
        for (const auto& t : tabs_) {
            if (!t.always && t.mode == mode) {
                showTab(t.name);
                return;
            }
        }
        if (!visibleTabs().contains(before)) showTab(visibleTabs().value(0));
    } else if (visibleTabs().contains(usualTab_)) {
        showTab(usualTab_);
    }
}

void Ribbon::showTab(const QString& name) {
    for (int i = 0; i < bar_->count(); ++i) {
        if (bar_->tabData(i).toString() == name && bar_->isTabVisible(i)) {
            bar_->setCurrentIndex(i);
            for (auto& t : tabs_) {
                if (t.name == name) pages_->setCurrentWidget(t.page);
            }
            if (mode_.isEmpty()) usualTab_ = name;
            return;
        }
    }
}

QString Ribbon::currentTab() const {
    const int i = bar_->currentIndex();
    return i < 0 ? QString() : bar_->tabData(i).toString();
}

QStringList Ribbon::visibleTabs() const {
    QStringList names;
    for (int i = 0; i < bar_->count(); ++i) {
        if (bar_->isTabVisible(i)) names << bar_->tabData(i).toString();
    }
    return names;
}

}  // namespace sb::app
