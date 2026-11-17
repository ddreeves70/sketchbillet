#pragma once

#include <QString>
#include <QStringList>
#include <QWidget>

#include <deque>
#include <map>
#include <vector>

class QAction;
class QHBoxLayout;
class QMenu;
class QStackedWidget;
class QTabBar;
class QToolButton;

namespace sb::app {

// The toolbar at the top of the window: a row with File, New, Open, Save, Undo
// and Redo and a Help button, then tabs (Model, Sketch, Inspect, View), each
// with panels of buttons. Every panel has its name underneath, with a dropdown
// of everything in it (Create ▾), like Fusion's toolbar. A tab can have one main
// button at its right end (Finish sketch).
//
// Students can pin any tool in a panel's dropdown to the panel as a button, or
// unpin a button, by right-clicking it; pins are kept as keys like
// "Model/Create/sb.modeling.cylinder" (tab, panel, the tool's object name).
//
// Object names, for tests and screenshots: the tab bar "sb.ribbon.tabs", a
// panel's dropdown button "sb.ribbon.<tab>.<panel>" in lower case without
// spaces, like "sb.ribbon.model.create", and its menu the same plus ".menu".
class Ribbon : public QWidget {
    Q_OBJECT
public:
    explicit Ribbon(QWidget* parent = nullptr);

    // The File dropdown at the left of the top row.
    QMenu* fileMenu() const { return fileMenu_; }
    // The Help (?) dropdown at the right of the top row.
    QMenu* helpMenu() const { return helpMenu_; }
    // Adds a small button to the top row, after File (New, Open, Save, Undo, Redo).
    void addQuick(QAction* action);

    // Makes the tab if it isn't there yet. Tabs named in `order` come first, in
    // that order; others follow in the order first made.
    void addTab(const QString& tab);
    void setTabOrder(const QStringList& order);
    // Panels named here come first in the tab, in this order; others follow in
    // the order first made.
    void setPanelOrder(const QString& tab, const QStringList& order);
    // When a tab shows: always, or only while `mode` is on (the usual toolbar is
    // an empty mode). See plugin::TabSpec.
    void setTabMode(const QString& tab, bool always, const QString& mode);

    // The dropdown of a panel (made, with its tab, if needed). `path` after the
    // panel names a submenu of it: {"Grid style"}.
    QMenu* panelMenu(const QString& tab, const QString& panel, const QStringList& path = {});
    // Adds a tool to the panel: `item` in its dropdown, and a button that runs
    // `button` (its shorter words; may be `item` itself), shown when `onPanel`
    // unless the student unpinned it, or when they pinned it.
    void addTool(const QString& tab, const QString& panel, QAction* item, QAction* button, bool onPanel);
    // Adds "Right-click a tool to pin it" at the end of every dropdown with tools.
    // Call once every tool is added.
    void addPinHints();

    // Pins: tools the student put on their panel, and ones they took off it.
    void setPins(const QStringList& pinned, const QStringList& unpinned);
    QStringList pinned() const { return pinned_; }
    QStringList unpinned() const { return unpinned_; }
    // Pins or unpins a tool (by its dropdown item), as right-clicking it does.
    void pin(const QString& tab, const QString& panel, QAction* item, bool on);
    // Whether a tool is a button on its panel now.
    bool onPanel(const QString& tab, const QString& panel, QAction* item) const;
    // Every tool back to the buttons it started with.
    void resetPins();
    // The tab's main button, at its right end, in green.
    void setPrimary(const QString& tab, QAction* action);

    // Switches to a toolbar mode: tabs for other modes hide, and a tab that only
    // shows in this mode is picked; going back picks the tab the student had.
    void setMode(const QString& mode);
    // Picks a tab by name, if it shows.
    void showTab(const QString& tab);
    QString currentTab() const;
    QStringList visibleTabs() const;

signals:
    // A tool was pinned or unpinned: save pinned() and unpinned().
    void pinsChanged();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    struct Tool {
        QString key;  // "Model/Create/sb.modeling.cylinder"
        QAction* item = nullptr;
        QToolButton* button = nullptr;
        bool byDefault = false;
    };
    struct Panel {
        QString name;
        QWidget* box = nullptr;
        QHBoxLayout* buttons = nullptr;
        QToolButton* drop = nullptr;
        QMenu* menu = nullptr;
        std::map<QString, QMenu*> submenus;
        std::vector<Tool> tools;
    };
    struct Tab {
        QString name;
        QWidget* page = nullptr;
        QHBoxLayout* panels = nullptr;
        QToolButton* primary = nullptr;
        std::deque<Panel> panelList;  // a deque, so a Panel& stays good as more are added
        std::vector<QWidget*> lines;  // the thin lines between panels
        QStringList order;
        bool always = true;
        QString mode;
    };
    Tab& tab(const QString& name);
    Panel& panel(Tab& t, const QString& name);
    void placeTabs();
    void placePanels(Tab& t);
    bool shows(const Tab& t) const;
    Tool* findTool(const QString& tab, const QString& panel, QAction* item);
    const Tool* findTool(const QString& tab, const QString& panel, QAction* item) const;
    void showTools();  // each tool's button shown or hidden, as pinned
    void askPin(const QString& tab, const QString& panel, QAction* item, const QPoint& at);

    QMenu* fileMenu_ = nullptr;
    QMenu* helpMenu_ = nullptr;
    QHBoxLayout* top_ = nullptr;
    int quickAt_ = 1;  // where the next quick button goes in the top row
    QTabBar* bar_ = nullptr;
    QStackedWidget* pages_ = nullptr;
    std::deque<Tab> tabs_;  // a deque, so a Tab& stays good as more are added
    QStringList order_;
    QString mode_;
    QString usualTab_;  // the tab picked before a mode's own tab took over
    bool placing_ = false;
    QStringList pinned_, unpinned_;
};

}  // namespace sb::app
