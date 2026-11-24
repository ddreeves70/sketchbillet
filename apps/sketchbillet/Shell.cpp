#include "Shell.h"

#include <DockAreaWidget.h>
#include <DockManager.h>
#include <DockWidget.h>
#include <FloatingDockContainer.h>

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGuiApplication>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeySequence>
#include <QLabel>
#include <QLocale>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QResizeEvent>
#include <QScreen>
#include <QSettings>
#include <QStatusBar>
#include <QStyle>
#include <QTabBar>
#include <QToolBar>
#include <QToolButton>

#include <algorithm>
#include <cctype>
#include <map>

#include "AppIcons.h"
#include "AppUi.h"
#include "CrashReport.h"
#include "Icons.h"
#include "ImportIcon.h"
#include "Ribbon.h"
#include "Updates.h"
#include "ViewOverlay.h"
#include "sb/core/Version.h"
#include "sb/io/SketchBilletFile.h"
#include "sb/io/Steps.h"
#include "sb/io/ThreeMf.h"
#include "sb/model/AssemblyCommands.h"
#include "sb/model/Commands.h"
#include "sb/plugin/Events.h"

namespace sb::app {

namespace {

constexpr int kLayoutVersion = 2;  // bump when panel ids change, so old layouts are ignored
const char* kLastLayout = "layouts/last";
const char* kNamedLayouts = "layouts/named";
const char* kGeometry = "window/geometry";
constexpr int kViewCubeRoom = 118;  // pixels at the view's top right kept clear for the view cube
constexpr int kOverlayGap = 6;      // pixels between floating panels on the same side
// Per panel that can float over the view: "panels/<id>/overView" (true or
// false), ".../overlaySize" and ".../overlayFolded".
QString panelKey(const QString& id, const char* what) {
    return QStringLiteral("panels/%1/%2").arg(id, QLatin1String(what));
}

QString qs(const std::string& s) {
    return QString::fromStdString(s);
}

ads::DockWidgetArea toAds(ui::PanelArea area) {
    switch (area) {
        case ui::PanelArea::Left:
            return ads::LeftDockWidgetArea;
        case ui::PanelArea::Right:
            return ads::RightDockWidgetArea;
        case ui::PanelArea::Bottom:
            return ads::BottomDockWidgetArea;
        case ui::PanelArea::Center:
            return ads::CenterDockWidgetArea;
    }
    return ads::LeftDockWidgetArea;
}

// The plugin a panel or action belongs to: the loaded plugin whose id, plus a
// dot, starts it. Panels of plugins that failed to load are dropped.
bool ownerLoaded(const plugin::PluginHost& host, const std::string& id) {
    for (const auto& p : host.plugins()) {
        const auto& pid = p.manifest.id;
        if (p.state == plugin::PluginState::Loaded && !pid.empty() && id.rfind(pid + ".", 0) == 0)
            return true;
    }
    return false;
}

}  // namespace

Shell::Shell(plugin::PluginHost& host, AppUi& ui, QSettings& settings, QWidget* parent)
    : QMainWindow(parent), host_(host), ui_(ui), settings_(settings) {
    setWindowTitle(QStringLiteral("SketchBillet"));
    resize(1280, 800);

    askUnsaved = [this](const QString& name) {
        const auto answer = QMessageBox::question(
            this, tr("Save changes?"), tr("%1 has changes that aren't saved. Save them first?").arg(name),
            QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
        return answer == QMessageBox::Save      ? Unsaved::Save
               : answer == QMessageBox::Discard ? Unsaved::Discard
                                                : Unsaved::Cancel;
    };
    askSavePath = [this](const QString& suggested) {
        QString path = QFileDialog::getSaveFileName(this, tr("Save design"), suggested,
                                                    tr("SketchBillet designs (*.sb)"));
        if (!path.isEmpty() && !path.endsWith(QStringLiteral(".sb"), Qt::CaseInsensitive)) {
            path += QStringLiteral(".sb");
        }
        return path;
    };
    showPrintCheck = [this](const model::PrintCheckResult& result) {
        QStringList lines;
        for (const auto& issue : result.issues) lines << QString::fromStdString(issue.message);
        if (result.ready())
            QMessageBox::information(this, tr("Check for Printing"), lines.join(QStringLiteral("\n\n")));
        else
            QMessageBox::warning(this, tr("Check for Printing"), lines.join(QStringLiteral("\n\n")));
    };
    openLink = [](const QUrl& url) { QDesktopServices::openUrl(url); };
    askCrashReport = [this](const QString& when, const QString& body, int count) {
        QMessageBox box(QMessageBox::Warning, tr("SketchBillet closed unexpectedly"),
                        count > 1
                            ? tr("SketchBillet closed unexpectedly %1 times, last on %2. Would you like to "
                                 "tell SketchBillet's makers, so they can fix it?")
                                  .arg(count)
                                  .arg(when)
                            : tr("SketchBillet closed unexpectedly on %1. Would you like to tell "
                                 "SketchBillet's makers, so they can fix it?")
                                  .arg(when),
                        QMessageBox::NoButton, this);
        box.setInformativeText(tr(
            "Report It opens a new GitHub issue in your web browser, filled in with a report SketchBillet "
            "wrote: its version, your computer's system, what went wrong and the last thing you clicked. It "
            "has no designs or file names in it. Read it, add what you were doing, then click Create. That "
            "needs a free GitHub account; without one, click Copy Report and send it to your teacher."));
        box.setDetailedText(body);
        auto* report = box.addButton(tr("Report It"), QMessageBox::AcceptRole);
        auto* copy = box.addButton(tr("Copy Report"), QMessageBox::ActionRole);
        box.addButton(tr("Don't Report"), QMessageBox::RejectRole);
        box.setDefaultButton(report);
        box.exec();
        return box.clickedButton() == report ? CrashChoice::Report
               : box.clickedButton() == copy ? CrashChoice::Copy
                                             : CrashChoice::Skip;
    };
    showUpdateBox = [this](const UpdateResult& result) {
        if (result.kind != UpdateResult::Kind::Newer) {
            if (result.kind == UpdateResult::Kind::Newest)
                QMessageBox::information(this, tr("Check for Updates"), result.message);
            else
                QMessageBox::warning(this, tr("Check for Updates"), result.message);
            return;
        }
        QMessageBox box(QMessageBox::Information, tr("Check for Updates"), result.message,
                        QMessageBox::NoButton, this);
        auto* get = box.addButton(tr("Open the Download Page"), QMessageBox::AcceptRole);
        box.addButton(tr("Not Now"), QMessageBox::RejectRole);
        box.exec();
        if (box.clickedButton() == get) QDesktopServices::openUrl(result.url);
    };
    askImportPath = [this] {
        return QFileDialog::getOpenFileName(
            this, tr("Import a part"), QString(),
            tr("Parts from other programs (*.step *.stp *.stl *.3mf *.STEP *.STP *.STL *.3MF);;"
               "STEP, from CAD programs (*.step *.stp *.STEP *.STP);;"
               "STL, from 3D modeling programs and slicers (*.stl *.STL);;"
               "3MF, from slicers and 3D modeling programs (*.3mf *.3MF)"));
    };
    askFilePath = [this](const QString& title, const QString& types, bool save, const QString& suggested) {
        return save ? QFileDialog::getSaveFileName(this, title, suggested, types)
                    : QFileDialog::getOpenFileName(this, title, suggested, types);
    };
    askExportPath = [this](const QString& suggested) {
        const QString stl = tr("STL, for 3D printing (*.stl)");
        const QString threeMf = tr("3MF, for 3D printing, with the part's name, material and color (*.3mf)");
        const QString step = tr("STEP, for other CAD programs (*.step *.stp)");
        QString filter = stl;
        QString path = QFileDialog::getSaveFileName(
            this, tr("Export"), suggested, stl + QStringLiteral(";;") + threeMf + QStringLiteral(";;") + step,
            &filter);
        if (!path.isEmpty() && QFileInfo(path).suffix().isEmpty()) {
            path += filter == step      ? QStringLiteral(".step")
                    : filter == threeMf ? QStringLiteral(".3mf")
                                        : QStringLiteral(".stl");
        }
        return path;
    };

    ads::CDockManager::setConfigFlag(ads::CDockManager::OpaqueSplitterResize, true);
    ads::CDockManager::setConfigFlag(ads::CDockManager::FocusHighlighting, true);
    docks_ = new ads::CDockManager(this);

    problemLabel_ = new QLabel(this);
    problemLabel_->setObjectName(QStringLiteral("sb.shell.problems"));
    problemLabel_->setStyleSheet(QStringLiteral("color: #c42b1c"));
    statusBar()->addWidget(problemLabel_, 1);
    const auto v = sb::version();
    statusBar()->addPermanentWidget(new QLabel(
        tr("SketchBillet %1").arg(QString::fromUtf8(v.data(), static_cast<qsizetype>(v.size()))), this));
    ui_.setSinks([this](const Diagnostics& d) { showProblems(d); },
                 [this](const QString& m) { showMessage(m); });
    ui_.setPanelSink([this](const std::string& id, bool show) { showPanel(qs(id), show); });

    buildMenus();
    buildPanels();
    defaultLayout_ = saveLayout();

    using namespace plugin;
    auto later = [this](const auto&) {
        if (refreshPending_) return;
        refreshPending_ = true;
        QMetaObject::invokeMethod(this, [this] { refresh(); }, Qt::QueuedConnection);
    };
    subs_.push_back(host_.events().subscribe<DocumentChanged>([this, later](const DocumentChanged& e) {
        problemLabel_->clear();  // a change replaces the old problems
        later(e);
    }));
    subs_.push_back(host_.events().subscribe<RebuildFinished>(later));
    subs_.push_back(host_.events().subscribe<ActiveDocumentChanged>(later));
    subs_.push_back(host_.events().subscribe<SelectionChanged>(later));
    subs_.push_back(host_.events().subscribe<PickModeChanged>(later));
    subs_.push_back(
        host_.events().subscribe<SettingChanged>([this](const SettingChanged&) { refreshActions(); }));
    subs_.push_back(
        host_.events().subscribe<ActionsChanged>([this](const ActionsChanged&) { refreshActions(); }));
    subs_.push_back(host_.events().subscribe<ToolbarModeChanged>(
        [this](const ToolbarModeChanged&) { refreshActions(); }));

    // Start with an empty design, so Create works straight away.
    host_.documents().add(std::make_unique<model::Document>(model::DocKind::Part, "Untitled"));

    // Put the window and panels back where they were last time.
    restoreGeometry(settings_.value(QLatin1String(kGeometry)).toByteArray());
    const auto last = settings_.value(QLatin1String(kLastLayout)).toByteArray();
    if (!last.isEmpty()) restoreLayout(last);
    rebuildLayoutMenu();
    refresh();
}

Shell::~Shell() {
    subs_.clear();
    ui_.setSinks({}, {});
    ui_.setPanelSink({});
}

// ---------------------------------------------------------------------------
// Menus and panels
// ---------------------------------------------------------------------------

void Shell::buildMenus() {
    // The toolbar: File and the quick buttons on top, then tabs of panels.
    ribbon_ = new Ribbon(this);
    setMenuWidget(ribbon_);
    ribbon_->setTabOrder({tr("Model"), tr("Assemble"), tr("Sketch"), tr("Inspect"), tr("View")});
    ribbon_->setPanelOrder(tr("Assemble"), {tr("Insert"), tr("Component"), tr("Parts")});
    // Most-used panels first, from left to right.
    ribbon_->setPanelOrder(
        tr("Model"), {tr("Sketch"), tr("Create"), tr("Modify"), tr("Combine"), tr("Pattern"), tr("Inspect"),
                      tr("Select"), tr("Insert"), tr("Make"), tr("Steps")});
    ribbon_->setPanelOrder(tr("Sketch"), {tr("Draw"), tr("Edit"), tr("Constrain"), tr("Dimension")});
    ribbon_->setPanelOrder(tr("Inspect"), {tr("Measure"), tr("Design")});
    ribbon_->setPanelOrder(tr("View"), {tr("Display"), tr("Grid"), tr("Axes"), tr("Panels"), tr("Layout")});

    auto* file = ribbon_->fileMenu();
    auto* newAct = file->addAction(tr("&New"), this, [this] { newDesign(); });
    newAct->setObjectName(QStringLiteral("sb.shell.new"));
    newAct->setShortcut(QKeySequence::New);
    auto* newAssemblyAct = file->addAction(tr("New &assembly"), this, [this] { newAssembly(); });
    newAssemblyAct->setObjectName(QStringLiteral("sb.shell.new_assembly"));
    newAssemblyAct->setStatusTip(
        tr("Start a design that puts parts together, like a car's body and wheels."));
    auto* openAct = file->addAction(tr("&Open..."), this, [this] {
        const auto path = QFileDialog::getOpenFileName(this, tr("Open design"), QString(),
                                                       tr("SketchBillet designs (*.sb)"));
        if (!path.isEmpty()) openDesign(path.toStdWString());
    });
    openAct->setObjectName(QStringLiteral("sb.shell.open"));
    openAct->setShortcut(QKeySequence::Open);
    samplesMenu_ = file->addMenu(tr("Open Sa&mple"));
    samplesMenu_->setObjectName(QStringLiteral("sb.shell.samples"));
    setSamplesFolder({});
    save_ = file->addAction(tr("&Save"), this, [this] { save(); });
    save_->setObjectName(QStringLiteral("sb.shell.save"));
    save_->setShortcut(QKeySequence::Save);
    auto* saveAs = file->addAction(tr("Save &as..."), this, [this] {
        const auto path =
            askSavePath(qs(host_.documents().find(activeFile())->name()) + QStringLiteral(".sb"));
        if (!path.isEmpty()) saveDesign(path.toStdWString());
    });
    saveAs->setObjectName(QStringLiteral("sb.shell.saveas"));
    saveAs->setShortcut(QKeySequence::SaveAs);
    auto* closeAct = file->addAction(tr("&Close"), this, [this] { closeDesign(activeFile()); });
    closeAct->setObjectName(QStringLiteral("sb.shell.close"));
    closeAct->setShortcut(QKeySequence::Close);
    closeAct->setStatusTip(
        tr("Close this design. SketchBillet asks first if it has changes that aren't saved."));
    // Ctrl+Tab and Ctrl+Shift+Tab go from one open design to the next.
    auto* next = new QAction(tr("Next Design"), this);
    next->setObjectName(QStringLiteral("sb.shell.nextdesign"));
    next->setShortcuts({QKeySequence(Qt::CTRL | Qt::Key_Tab), QKeySequence::NextChild});
    connect(next, &QAction::triggered, this, [this] { nextDesign(1); });
    addAction(next);
    auto* previous = new QAction(tr("Previous Design"), this);
    previous->setObjectName(QStringLiteral("sb.shell.previousdesign"));
    previous->setShortcuts(
        {QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Backtab), QKeySequence::PreviousChild});
    connect(previous, &QAction::triggered, this, [this] { nextDesign(-1); });
    addAction(previous);
    connect(ribbon_->doneButton(), &QToolButton::clicked, this, [this] { finishEditingPart(); });
    auto* tabs = ribbon_->designTabs();
    connect(tabs, &QTabBar::currentChanged, this, [this, tabs](int index) {
        if (syncingTabs_ || index < 0) return;
        if (const auto id = DocId::parse(tabs->tabData(index).toString().toStdString()))
            host_.documents().setActive(*id);
    });
    connect(tabs, &QTabBar::tabCloseRequested, this, [this, tabs](int index) {
        if (const auto id = DocId::parse(tabs->tabData(index).toString().toStdString())) closeDesign(*id);
    });
    file->addSeparator();
    auto* importAct = file->addAction(tr("&Import..."), this, [this] {
        const auto path = askImportPath();
        if (!path.isEmpty()) importPart(path.toStdWString());
    });
    importAct->setObjectName(QStringLiteral("sb.shell.import"));
    importAct->setShortcut(QKeySequence(QStringLiteral("Ctrl+I")));
    importAct->setIcon(svgIcon(icons::modeling::import_svg));
    importAct->setStatusTip(
        tr("Add a part from another program, from a STEP, STL or 3MF file, as a new body."));
    importAct->setToolTip(importAct->statusTip());
    export_ = file->addAction(tr("&Export..."), this, [this] {
        const auto* doc = host_.documents().find(activeDesign());
        const auto path = askExportPath(qs(doc ? doc->name() : "Untitled") + QStringLiteral(".stl"));
        if (!path.isEmpty()) exportDesign(path.toStdWString());
    });
    export_->setObjectName(QStringLiteral("sb.shell.export"));
    export_->setShortcut(QKeySequence(QStringLiteral("Ctrl+E")));
    export_->setIcon(svgIcon(icons::app::export_svg));
    export_->setStatusTip(
        tr("Write the part as STL or 3MF (for 3D printing) or STEP (for other CAD programs)."));
    export_->setToolTip(export_->statusTip());
    auto* check = file->addAction(tr("Check for &Printing..."), this, [this] { checkForPrinting(); });
    check->setObjectName(QStringLiteral("sb.shell.check_print"));
    check->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+P")));
    check->setIcon(svgIcon(icons::app::check_print_svg));
    check->setIconText(tr("Check print"));
    check->setStatusTip(tr("Check the part is closed all round and has no walls too thin to print."));
    check->setToolTip(check->statusTip());
    file->addSeparator();
    auto* quit = file->addAction(tr("&Quit"), this, [this] { close(); });
    quit->setObjectName(QStringLiteral("sb.shell.quit"));
    quit->setShortcut(QKeySequence::Quit);

    undo_ = new QAction(tr("Undo"), this);
    connect(undo_, &QAction::triggered, this, [this] { host_.documents().undo(activeFile()); });
    undo_->setObjectName(QStringLiteral("sb.shell.undo"));
    undo_->setShortcut(QKeySequence::Undo);
    redo_ = new QAction(tr("Redo"), this);
    connect(redo_, &QAction::triggered, this, [this] { host_.documents().redo(activeFile()); });
    redo_->setObjectName(QStringLiteral("sb.shell.redo"));
    redo_->setShortcuts(
        {QKeySequence(QStringLiteral("Ctrl+Y")), QKeySequence(QStringLiteral("Ctrl+Shift+Z"))});
    copy_ = new QAction(tr("Copy steps"), this);
    connect(copy_, &QAction::triggered, this, [this] { copySteps(); });
    copy_->setObjectName(QStringLiteral("sb.shell.copy"));
    copy_->setShortcut(QKeySequence::Copy);
    copy_->setStatusTip(
        tr("Copy the picked steps, and the steps they use, so you can paste them into any design."));
    paste_ = new QAction(tr("Paste steps"), this);
    connect(paste_, &QAction::triggered, this, [this] { pasteSteps(); });
    paste_->setObjectName(QStringLiteral("sb.shell.paste"));
    paste_->setShortcut(QKeySequence::Paste);
    paste_->setStatusTip(tr("Add the copied steps to the end of this design."));
    connect(QGuiApplication::clipboard(), &QClipboard::dataChanged, this, [this] { refresh(); });

    newAct->setIcon(svgIcon(icons::app::new_svg));
    openAct->setIcon(svgIcon(icons::app::open_svg));
    save_->setIcon(svgIcon(icons::app::save_svg));
    undo_->setIcon(svgIcon(icons::app::undo_svg));
    redo_->setIcon(svgIcon(icons::app::redo_svg));
    // Their tips follow their words, so Undo's says what it undoes ("Undo Add Box1").
    for (auto* a : {newAct, openAct, save_, undo_, redo_}) ribbon_->addQuick(a);

    // SketchBillet's own tools, in the panels where students look for them.
    copy_->setIcon(svgIcon(icons::app::copy_svg));
    paste_->setIcon(svgIcon(icons::app::paste_svg));
    ribbon_->addTool(tr("Model"), tr("Steps"), copy_, copy_, false);
    ribbon_->addTool(tr("Model"), tr("Steps"), paste_, paste_, false);
    ribbon_->addTool(tr("Model"), tr("Insert"), importAct, importAct, true);
    ribbon_->addTool(tr("Model"), tr("Make"), export_, export_, true);
    ribbon_->addTool(tr("Model"), tr("Make"), check, check, false);
    ribbon_->addTool(tr("Inspect"), tr("Measure"), check, check, true);

    // Plugin actions, in the tabs and panels they name ("Model/Create"), made in
    // the order first named.
    for (const auto& spec : host_.actions()) {
        auto* a = new QAction(qs(spec.text), this);
        a->setObjectName(qs(spec.id));
        a->setToolTip(qs(spec.tip));
        a->setStatusTip(qs(spec.tip));
        if (!spec.shortcut.empty()) a->setShortcut(QKeySequence(qs(spec.shortcut)));
        a->setCheckable(static_cast<bool>(spec.checked));
        a->setIcon(svgIcon(spec.icon));
        connect(a, &QAction::triggered, this, [this, spec] { runAction(spec); });
        // Every tool has a button, shown when it's on its panel (`toolbar`, or pinned).
        auto* button = new QAction(
            spec.buttonText.empty() ? qs(spec.text).remove(QStringLiteral("...")) : qs(spec.buttonText),
            this);
        button->setObjectName(qs(spec.id + ".button"));
        button->setToolTip(qs(spec.tip));
        button->setStatusTip(qs(spec.tip));
        button->setCheckable(a->isCheckable());
        button->setIcon(a->icon());
        connect(button, &QAction::triggered, this, [this, spec] { runAction(spec); });
        for (const auto& place : qs(spec.menu).split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
            auto parts = place.trimmed().split(QLatin1Char('/'), Qt::SkipEmptyParts);
            if (parts.isEmpty()) parts = {tr("Plugins")};
            const QString tab = parts.takeFirst();
            const QString panel = parts.isEmpty() ? tab : parts.takeFirst();
            if (spec.primary) {
                ribbon_->setPrimary(tab, button);
                continue;
            }
            if (!parts.isEmpty()) {
                ribbon_->panelMenu(tab, panel, parts)->addAction(a);  // in a submenu: not a button
                continue;
            }
            ribbon_->addTool(tab, panel, a, button, spec.toolbar);
        }
        actions_.push_back({spec, a, button});
    }
    for (const auto& tab : host_.tabs()) ribbon_->setTabMode(qs(tab.name), tab.always, qs(tab.mode));

    // The tools each student pinned to their panels, or took off them.
    ribbon_->addPinHints();
    ribbon_->setPins(settings_.value(QStringLiteral("toolbar/pinned")).toStringList(),
                     settings_.value(QStringLiteral("toolbar/unpinned")).toStringList());
    connect(ribbon_, &Ribbon::pinsChanged, this, [this] {
        settings_.setValue(QStringLiteral("toolbar/pinned"), ribbon_->pinned());
        settings_.setValue(QStringLiteral("toolbar/unpinned"), ribbon_->unpinned());
    });

    // View > Panels and View > Layout: show and hide panels, and saved layouts.
    windowMenu_ = ribbon_->panelMenu(tr("View"), tr("Panels"));
    layoutMenu_ = ribbon_->panelMenu(tr("View"), tr("Layout"));
    auto* saveLayoutButton = new QAction(svgIcon(icons::app::save_layout_svg), tr("Save layout"), this);
    saveLayoutButton->setObjectName(QStringLiteral("sb.shell.save_layout"));
    saveLayoutButton->setToolTip(
        tr("Save how the panels are arranged, under a name, to open again from Layout."));
    saveLayoutButton->setStatusTip(saveLayoutButton->toolTip());
    connect(saveLayoutButton, &QAction::triggered, this, [this] { saveLayoutAs(); });
    ribbon_->addButton(tr("View"), tr("Layout"), saveLayoutButton);
    auto* resetLayoutButton = new QAction(svgIcon(icons::app::reset_layout_svg), tr("Reset layout"), this);
    resetLayoutButton->setObjectName(QStringLiteral("sb.shell.reset_layout"));
    resetLayoutButton->setToolTip(tr("Put every panel back where it started."));
    resetLayoutButton->setStatusTip(resetLayoutButton->toolTip());
    connect(resetLayoutButton, &QAction::triggered, this, [this] { resetLayout(); });
    ribbon_->addButton(tr("View"), tr("Layout"), resetLayoutButton);

    helpMenu_ = ribbon_->helpMenu();
    auto* about = helpMenu_->addAction(tr("&About SketchBillet"), this, [this] {
        const auto ver = sb::version();
        QMessageBox::about(this, tr("About SketchBillet"),
                           tr("SketchBillet %1\nFree, open-source CAD.\nhttps://sketchbillet.org")
                               .arg(QString::fromUtf8(ver.data(), static_cast<qsizetype>(ver.size()))));
    });
    about->setObjectName(QStringLiteral("sb.shell.about"));

    // Every dropdown asks what can be used when it opens, and every item's
    // shortcut works wherever it is, even in a tab that isn't showing.
    for (auto* m : ribbon_->findChildren<QMenu*>()) {
        connect(m, &QMenu::aboutToShow, this, [this] { refreshActions(); });
        m->setToolTipsVisible(true);
    }
    for (auto* a : ribbon_->findChildren<QAction*>()) {
        if (!a->shortcut().isEmpty()) addAction(a);
    }
    for (auto* a :
         {newAct, openAct, save_, saveAs, importAct, export_, check, quit, undo_, redo_, copy_, paste_})
        addAction(a);
    for (const auto& item : actions_) addAction(item.item);
}

void Shell::buildPanels() {
    // The middle of the window is the first Center panel (sb.viewer's viewer).
    std::vector<const ui::PanelSpec*> specs;
    for (const auto& spec : ui_.panels()) {
        if (ownerLoaded(host_, spec.id) && spec.create) specs.push_back(&spec);
    }
    const auto centerSpec = std::find_if(specs.begin(), specs.end(),
                                         [](const auto* s) { return s->area == ui::PanelArea::Center; });
    auto makeDock = [this](const ui::PanelSpec& spec, const QString& id, const QString& title) {
        auto* dock = new ads::CDockWidget(docks_, title);
        dock->setObjectName(id);
        // Panels are as wide as their content needs (their minimum width), not squeezed.
        dock->setWidget(spec.create(), ads::CDockWidget::ForceNoScrollArea);
        dock->setMinimumSizeHintMode(ads::CDockWidget::MinimumSizeHintFromContentMinimumSize);
        return dock;
    };
    if (centerSpec != specs.end()) {
        centerArea_ =
            docks_->setCentralWidget(makeDock(**centerSpec, qs((*centerSpec)->id), (*centerSpec)->title));
    } else {
        auto* missing =
            new QLabel(tr("The 3D viewer plugin (sb.viewer) didn't load, so there's nothing to show "
                          "here. Reinstall SketchBillet to get it back."));
        missing->setAlignment(Qt::AlignCenter);
        missing->setWordWrap(true);
        auto* dock = new ads::CDockWidget(docks_, tr("Viewer"));
        dock->setObjectName(QStringLiteral("sb.shell.noviewer"));
        dock->setWidget(missing);
        centerArea_ = docks_->setCentralWidget(dock);
    }

    // Panels on the same side share its space as tabs, so the viewer keeps its room.
    std::map<ui::PanelArea, ads::CDockAreaWidget*> sides;
    for (const auto* spec : specs) {
        if (centerSpec != specs.end() && spec == *centerSpec) continue;
        auto* dock = makeDock(*spec, qs(spec->id), spec->title);
        if (spec->area == ui::PanelArea::Center) {
            docks_->addDockWidgetTabToArea(dock, centerArea_);
        } else if (auto it = sides.find(spec->area); it != sides.end()) {
            docks_->addDockWidgetTabToArea(dock, it->second);
        } else {
            sides[spec->area] = docks_->addDockWidget(toAds(spec->area), dock);
        }
        if (spec->startHidden) dock->toggleView(false);
    }

    // Panels that float over the view: a frame over the view's dock, which the
    // panel's widget moves into while it floats there. Left panels stack down
    // the view's top left; right ones down its top right, below the view cube.
    auto* viewDock = centerArea_ ? centerArea_->currentDockWidget() : nullptr;
    for (const auto* spec : specs) {
        if (!spec->floatsOverView || !viewDock) continue;
        const QString id = qs(spec->id);
        auto* dock = docks_->findDockWidget(id);
        if (!dock) continue;
        const bool right = spec->area == ui::PanelArea::Right;
        Overlay o;
        o.dock = dock;
        o.shown = !spec->startHidden;
        o.startsShown = o.shown;
        o.frame = new ViewOverlay(spec->title, viewDock, right);
        o.frame->setObjectName(QStringLiteral("sb.shell.overlay.") + id);
        o.frame->topInset = [this, viewDock, id, right] {
            // Below a toolbar across the top of the view, if it has one.
            auto* view = viewDock->widget();
            if (!view) return 0;
            const int viewTop = view->mapTo(viewDock, QPoint(0, 0)).y();
            int top = viewTop;
            for (auto* bar : view->findChildren<QToolBar*>()) {
                if (!bar->isVisibleTo(view)) continue;
                const int barTop = bar->mapTo(viewDock, QPoint(0, 0)).y();
                if (barTop <= viewTop + 2) top = std::max(top, barTop + bar->height());
            }
            if (right) top += kViewCubeRoom;
            // Below the floating panels before it on the same side.
            for (const auto& other : overlayOrder_) {
                if (other == id) break;
                const auto* f = overlays_.at(other).frame;
                // (place() adds its own 8 px margin to this.)
                if (f->right() == right && f->isVisible()) top = f->geometry().bottom() + 1 + kOverlayGap - 8;
            }
            return top;
        };
        o.frame->onPlaced = [this, id] { placeOverlaysAfter(id); };
        o.frame->onDock = [this, id] { setOverView(id, false); };
        o.frame->onChanged = [this, id] {
            const auto& f = *overlays_.at(id).frame;
            settings_.setValue(panelKey(id, "overlaySize"), f.wantedSize());
            settings_.setValue(panelKey(id, "overlayFolded"), f.folded());
        };
        const QSize size = settings_.value(panelKey(id, "overlaySize")).toSize();
        if (size.isValid()) o.frame->setWantedSize(size);
        o.frame->setFolded(settings_.value(panelKey(id, "overlayFolded"), false).toBool());
        o.show = new QAction(spec->title, this);
        o.show->setObjectName(QStringLiteral("sb.shell.show.") + id);
        o.show->setCheckable(true);
        connect(o.show, &QAction::triggered, this, [this, id](bool on) { showOverlay(id, on); });
        o.over = new QAction(tr("%1 over the view").arg(spec->title), this);
        o.over->setObjectName(QStringLiteral("sb.shell.overview.") + id);
        o.over->setCheckable(true);
        o.over->setStatusTip(
            tr("Float %1 over the 3D view, see-through, instead of beside it.").arg(spec->title.toLower()));
        connect(o.over, &QAction::triggered, this, [this, id](bool on) { setOverView(id, on); });
        // Panels > title opens a docked panel that was closed where it starts: floating
        // over the view, as Reset layout puts it. (Plugins showing a docked panel, like
        // Measure while measuring, don't use this action, so they leave it docked.)
        auto* toggle = dock->toggleViewAction();
        toggle->setObjectName(QStringLiteral("sb.shell.dock.") + id);
        QObject::disconnect(toggle, nullptr, dock, nullptr);
        connect(toggle, &QAction::triggered, this, [this, id, dock](bool on) {
            if (!on) {
                dock->toggleView(false);
                return;
            }
            setOverView(id, true);
            showPanel(id, true);
        });
        overlays_[id] = o;
        overlayOrder_.push_back(id);
    }

    for (const auto& id : dockIds()) {
        auto* dock = docks_->findDockWidget(id);
        if (!dock) continue;
        windowMenu_->addAction(dock->toggleViewAction());
        if (auto it = overlays_.find(id); it != overlays_.end()) windowMenu_->addAction(it->second.show);
    }
    if (!overlayOrder_.empty()) windowMenu_->addSeparator();
    for (const auto& id : overlayOrder_) {
        windowMenu_->addAction(overlays_.at(id).over);
        setOverView(id, settings_.value(panelKey(id, "overView"), true).toBool());
    }
    windowMenu_->addSeparator();
    // A button in the Panels group for each panel with a picture: it opens and closes
    // the panel, wherever it is (floating over the view or docked).
    for (const auto* spec : specs) {
        if (spec->icon.empty()) continue;
        const QString id = qs(spec->id);
        auto* dock = docks_->findDockWidget(id);
        if (!dock) continue;
        auto* b = new QAction(svgIcon(spec->icon), spec->title, this);
        b->setObjectName(QStringLiteral("sb.shell.panel.") + id);
        b->setCheckable(true);
        b->setToolTip(tr("Open or close the %1 panel.").arg(spec->title.toLower()));
        b->setStatusTip(b->toolTip());
        connect(b, &QAction::triggered, this, [this, id](bool on) { openPanel(id, on); });
        connect(dock, &ads::CDockWidget::viewToggled, this, [this, id] { syncPanelButton(id); });
        ribbon_->addButton(tr("View"), tr("Panels"), b);
        panelButtons_[id] = b;
        syncPanelButton(id);
    }
    for (const auto* spec : specs) {
        if (!spec->allowMultiple) continue;
        const QString id = qs(spec->id);
        auto* a = windowMenu_->addAction(tr("New %1").arg(spec->title.toLower()), this,
                                         [this, id] { openAnotherPanel(id); });
        a->setObjectName(QStringLiteral("sb.shell.new.") + id);
        a->setStatusTip(tr("Open another %1 next to the others.").arg(spec->title.toLower()));
    }

    // Plugins' status-bar items, at the bottom right, before SketchBillet's own.
    int at = 1;  // after the problems label
    for (const auto& item : ui_.statusItems()) {
        if (!ownerLoaded(host_, item.id) || !item.create) continue;
        if (auto* w = item.create()) {
            w->setObjectName(qs(item.id));
            statusBar()->insertPermanentWidget(at++, w);
        }
    }
}

QString Shell::openAnotherPanel(const QString& id) {
    for (const auto& spec : ui_.panels()) {
        if (qs(spec.id) != id || !spec.allowMultiple || !spec.create || !ownerLoaded(host_, spec.id))
            continue;
        // Number copies 2, 3, ... skipping ids still open.
        int n = std::max(copies_[id], 1);
        QString newId;
        do {
            newId = QStringLiteral("%1.%2").arg(id).arg(++n);
        } while (docks_->findDockWidget(newId));
        copies_[id] = n;
        auto* dock = new ads::CDockWidget(docks_, QStringLiteral("%1 %2").arg(spec.title).arg(n));
        dock->setObjectName(newId);
        dock->setWidget(spec.create(), ads::CDockWidget::ForceNoScrollArea);
        // Copies aren't remembered between runs: closing one deletes it.
        dock->setFeature(ads::CDockWidget::DockWidgetDeleteOnClose, true);
        docks_->addDockWidget(ads::RightDockWidgetArea, dock, centerArea_);
        return newId;
    }
    return {};
}

void Shell::saveLayoutAs() {
    bool ok = false;
    const auto name = QInputDialog::getText(this, tr("Save layout"), tr("Name for this layout:"),
                                            QLineEdit::Normal, tr("My layout"), &ok);
    if (ok && !name.trimmed().isEmpty()) saveNamedLayout(name.trimmed());
}

void Shell::rebuildLayoutMenu() {
    layoutMenu_->clear();
    for (const auto& name : layoutNames()) {
        layoutMenu_->addAction(name, this, [this, name] { openNamedLayout(name); });
    }
    if (!layoutNames().isEmpty()) layoutMenu_->addSeparator();
    layoutMenu_->addAction(tr("Save layout as..."), this, [this] { saveLayoutAs(); });
    layoutMenu_->addAction(tr("Reset layout"), this, [this] { resetLayout(); });
    auto* resetTools =
        layoutMenu_->addAction(tr("Reset toolbar buttons"), this, [this] { ribbon_->resetPins(); });
    resetTools->setObjectName(QStringLiteral("sb.shell.reset_toolbar"));
    resetTools->setStatusTip(tr("Put back the buttons each panel started with, undoing every pin."));
}

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------

void Shell::refreshActions() {
    const auto* doc = host_.documents().find(activeDesign());
    const auto& sel = host_.selection();
    const auto& mode = host_.toolbarMode();
    // While a mode is on, its own items win any shortcut they share with others
    // (Delete deletes what's picked in a sketch, not the step).
    QStringList modeKeys;
    for (const auto& item : actions_) {
        if (!mode.empty() && item.spec.toolbarMode == mode && !item.spec.shortcut.empty())
            modeKeys << QKeySequence(qs(item.spec.shortcut)).toString();
    }
    for (auto& [spec, a, button] : actions_) {
        if (!spec.shortcut.empty()) {
            const QKeySequence keys(qs(spec.shortcut));
            const bool yields =
                !mode.empty() && spec.toolbarMode != mode && modeKeys.contains(keys.toString());
            const QKeySequence want = yields ? QKeySequence() : keys;
            if (a->shortcut() != want) a->setShortcut(want);
        }
        // Enabled: a design to change (or the plugin's own code), the steps it needs
        // picked, and whatever the plugin says.
        bool ok = doc != nullptr || static_cast<bool>(spec.run);
        // An assembly has no steps of its own: steps go in its parts.
        if (doc && doc->kind() == model::DocKind::Assembly &&
            (spec.command.rfind("feature.", 0) == 0 || spec.command.rfind("sketch.", 0) == 0 ||
             spec.command.rfind("folder.", 0) == 0))
            ok = false;
        if (!spec.fromComponent.empty() || !spec.fromComponentPart.empty()) {
            ok = ok && sel.doc() == activeDesign() && sel.components().size() == 1;
        }
        if (!spec.fromSelection.empty()) {
            ok = ok && sel.doc() == activeDesign() && sel.features().size() == spec.fromSelection.size();
        }
        if (!spec.fromSelectionList.empty()) {
            ok = ok && sel.doc() == activeDesign() && !sel.features().empty();
        }
        if ((!spec.fromEdges.empty() && pickedEdges()) ||
            (!spec.fromFaces.empty() && pickedOnly(kernel::TopoKind::Face))) {
            // Picked edges or faces must all be on one body; with none picked, it starts picking.
            const auto& first = sel.elements().front().body;
            ok = ok && sel.doc() == activeDesign() &&
                 std::all_of(sel.elements().begin(), sel.elements().end(),
                             [&](const plugin::PickedElement& e) { return e.body == first; });
        }
        try {
            if (ok && spec.enabled) ok = spec.enabled();
        } catch (...) {
            ok = false;  // a plugin's function failed: grey the item out
        }
        a->setEnabled(ok);
        try {
            if (spec.checked) a->setChecked(spec.checked());
            if (spec.currentText) a->setText(qs(spec.currentText()));
        } catch (...) {
            // A plugin's check-mark function failed: leave the item as it was.
        }
        if (button) {
            button->setEnabled(ok);
            button->setChecked(a->isChecked());
            if (spec.currentText && spec.buttonText.empty()) button->setText(a->text());
        }
    }
    ribbon_->setMode(qs(mode));
}

void Shell::runAction(const plugin::ActionSpec& spec) {
    crash::note(spec.id);  // for a crash report, if this leads to one
    if (spec.openFile) {
        // A design file to open beside the others, in its own tab (a part's file).
        std::string file;
        try {
            file = spec.openFile();
        } catch (...) {
            showProblems({{"action.failed", Severity::Error,
                           tr("%1 stopped with an error. Ask the plugin's author for help.")
                               .arg(qs(spec.text))
                               .toStdString(),
                           spec.plugin}});
            return;
        }
        if (!file.empty()) openDesign(std::filesystem::path(std::u8string(file.begin(), file.end())));
        refresh();
        return;
    }
    if (spec.run) {
        // The plugin's own code (a setting, a dialog), not a change to a design.
        try {
            spec.run();
        } catch (const std::exception& e) {
            showProblems({{"action.failed", Severity::Error,
                           tr("%1 stopped with an error (%2). Ask the plugin's author for help.")
                               .arg(qs(spec.text), QString::fromUtf8(e.what()))
                               .toStdString(),
                           spec.plugin}});
        } catch (...) {
            showProblems({{"action.failed", Severity::Error,
                           tr("%1 stopped with an error. Ask the plugin's author for help.")
                               .arg(qs(spec.text))
                               .toStdString(),
                           spec.plugin}});
        }
        refreshActions();
        return;
    }
    auto& docs = host_.documents();
    const DocId doc = activeDesign();
    model::Args args = spec.args;
    const auto& picked = host_.selection().features();
    if (!spec.fromSelection.empty()) {
        if (picked.size() != spec.fromSelection.size() || host_.selection().doc() != doc) {
            showProblems({{"action.selection",
                           Severity::Error,
                           tr("%1 needs %2 steps picked in the feature tree, in order.")
                               .arg(qs(spec.text))
                               .arg(spec.fromSelection.size())
                               .toStdString(),
                           {}}});
            return;
        }
        for (std::size_t i = 0; i < picked.size(); ++i) args[spec.fromSelection[i]] = picked[i];
        // The body to change (a Boolean's A, a Move's body), picked in the feature
        // tree as a step a later step already changed (Box1, now part of Cut1): the
        // student means the body as it is now, so use the step it has become.
        const auto* design = docs.find(doc);
        for (std::size_t i = 0; design && i < picked.size(); ++i) {
            const auto& arg = spec.fromSelection[i];
            if (arg != "a" && arg != "body") continue;
            const FeatureId now = design->latestBody(picked[i]);
            if (now == picked[i] || std::find(picked.begin(), picked.end(), now) != picked.end()) continue;
            args[arg] = now;
            showMessage(
                tr("%1 is part of %2 now, so %3 uses %2.")
                    .arg(qs(design->find(picked[i])->name()), qs(design->find(now)->name()), qs(spec.text)));
        }
    }
    if (!spec.fromSelectionList.empty()) {
        if (picked.empty() || host_.selection().doc() != doc) {
            showProblems({{"action.selection",
                           Severity::Error,
                           tr("%1 needs one or more steps picked in the feature tree.")
                               .arg(qs(spec.text))
                               .toStdString(),
                           {}}});
            return;
        }
        args[spec.fromSelectionList] = picked;
    }
    auto& sel = host_.selection();
    if (!spec.fromEdges.empty()) {
        if (!pickedEdges() || sel.doc() != doc) {
            // No edges picked yet: pick edges, then click this again.
            sel.setPickMode(plugin::PickMode::Edges);
            statusBar()->showMessage(
                tr("Click the edges for %1 in the 3D view (Ctrl-click to pick more), then click %1 again.")
                    .arg(qs(spec.text)),
                15000);
            refreshActions();
            return;
        }
        std::vector<model::GeometryRef> edges;
        for (const auto& e : sel.elements()) edges.push_back(e.ref);
        args[spec.fromEdges] = std::move(edges);
        if (!spec.fromEdgesBody.empty()) args[spec.fromEdgesBody] = sel.elements().front().body;
    }
    if (!spec.fromFaces.empty()) {
        if (!pickedOnly(kernel::TopoKind::Face) || sel.doc() != doc) {
            // No faces picked yet: pick faces, then click this again.
            sel.setPickMode(plugin::PickMode::Faces);
            statusBar()->showMessage(
                tr("Click the faces for %1 in the 3D view (Ctrl-click to pick more), then click %1 again.")
                    .arg(qs(spec.text)),
                15000);
            refreshActions();
            return;
        }
        std::vector<model::GeometryRef> faces;
        for (const auto& e : sel.elements()) faces.push_back(e.ref);
        args[spec.fromFaces] = std::move(faces);
        if (!spec.fromFacesBody.empty()) args[spec.fromFacesBody] = sel.elements().front().body;
    }
    if (!spec.fromComponent.empty() || !spec.fromComponentPart.empty()) {
        const auto* design = docs.find(doc);
        const auto* c = design && sel.doc() == doc && sel.components().size() == 1
                            ? design->findComponent(sel.components().front())
                            : nullptr;
        const auto* part = c ? design->findPart(c->part) : nullptr;
        if (!part) {
            showProblems({{"action.component",
                           Severity::Error,
                           tr("%1 needs one component picked, in the feature tree or the 3D view.")
                               .arg(qs(spec.text))
                               .toStdString(),
                           {}}});
            return;
        }
        if (!spec.fromComponent.empty()) args[spec.fromComponent] = c->name;
        if (!spec.fromComponentPart.empty()) args[spec.fromComponentPart] = part->name();
    }
    if (!spec.fromFile.empty()) {
        QString suggested;
        try {
            if (spec.suggestedFile) suggested = qs(spec.suggestedFile());
        } catch (...) {
            // A plugin's function failed: start with no name.
        }
        const QString path = askFilePath(qs(spec.text), qs(spec.fileTypes), spec.saveFile, suggested);
        if (path.isEmpty()) return;  // the window was closed
        const auto u = QDir::cleanPath(path).toStdU16String();
        const auto utf8 = std::filesystem::path(u).u8string();
        args[spec.fromFile] = std::string(utf8.begin(), utf8.end());
    }
    const auto before = docs.find(doc)->featureCount();
    const auto componentsBefore = docs.find(doc)->components();
    auto problems = docs.run(doc, spec.command, args);
    if (!problems.empty()) {
        showProblems(problems);
        return;
    }
    if (!spec.fromEdges.empty() || !spec.fromFaces.empty()) {
        sel.setPickMode(plugin::PickMode::Bodies);  // done picking edges or faces
        statusBar()->clearMessage();
    }
    // Select the step the action made, so Properties shows its numbers.
    const auto* design = docs.find(doc);
    if (design && design->featureCount() > before) {
        host_.selection().set(doc, {design->featureAt(design->featureCount() - 1).id()});
    }
    // Or the components it put in an assembly, so they show where they are.
    if (design && design->components().size() > componentsBefore.size()) {
        std::vector<model::ComponentId> added;
        for (const auto& c : design->components()) {
            if (std::none_of(componentsBefore.begin(), componentsBefore.end(),
                             [&](const model::Component& b) { return b.id == c.id; }))
                added.push_back(c.id);
        }
        host_.selection().setComponents(doc, added);
    }
}

bool Shell::pickedEdges() const {
    return pickedOnly(kernel::TopoKind::Edge);
}

bool Shell::pickedOnly(kernel::TopoKind kind) const {
    const auto& elements = host_.selection().elements();
    return !elements.empty() &&
           std::all_of(elements.begin(), elements.end(),
                       [kind](const plugin::PickedElement& e) { return e.ref.kind == kind; });
}

QAction* Shell::action(const std::string& id) const {
    for (const auto& p : actions_) {
        if (p.spec.id == id) return p.item;
    }
    return nullptr;
}

QAction* Shell::button(const std::string& id) const {
    for (const auto& p : actions_) {
        if (p.spec.id == id) return p.button;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Designs
// ---------------------------------------------------------------------------

DocId Shell::activeDesign() const {
    return host_.documents().active();
}

DocId Shell::activeFile() const {
    return host_.documents().activeDesign();
}

std::filesystem::path Shell::activePath() const {
    auto it = paths_.find(activeFile());
    return it == paths_.end() ? std::filesystem::path() : it->second;
}

QString Shell::designTitle(const DocId& id) const {
    const auto it = paths_.find(id);
    if (it != paths_.end() && !it->second.empty())
        return QString::fromStdWString(it->second.stem().wstring());
    const auto* doc = host_.documents().find(id);
    return doc ? qs(doc->name()) : tr("Untitled");
}

bool Shell::askToSave(const DocId& id) {
    auto& docs = host_.documents();
    const auto* doc = docs.find(id);
    if (!doc || !doc->isModified()) return true;
    // Show the design being asked about, so the student knows which one it is.
    if (docs.activeDesign() != id) {
        docs.setActive(id);
        refresh();
    }
    switch (askUnsaved(designTitle(id))) {
        case Unsaved::Save:
            return save();
        case Unsaved::Discard:
            return true;
        case Unsaved::Cancel:
            return false;
    }
    return false;
}

bool Shell::confirmCloseAll() {
    for (const auto& id : host_.documents().ids()) {
        if (!askToSave(id)) return false;
    }
    return true;
}

bool Shell::closeDesign(const DocId& id) {
    auto& docs = host_.documents();
    if (!docs.find(id)) return false;
    if (!askToSave(id)) return false;
    const QString title = designTitle(id);
    docs.close(id);
    paths_.erase(id);
    // There's always a design to work on, so Create works straight away.
    if (docs.ids().empty()) docs.add(std::make_unique<model::Document>(model::DocKind::Part, "Untitled"));
    refresh();
    showMessage(tr("Closed %1").arg(title));
    return true;
}

void Shell::nextDesign(int step) {
    const auto ids = host_.documents().ids();
    if (ids.size() < 2) return;
    // In the order the tabs show, which the student can change by dragging them.
    std::vector<DocId> order;
    for (int i = 0; i < ribbon_->designTabs()->count(); ++i) {
        const auto id = DocId::parse(ribbon_->designTabs()->tabData(i).toString().toStdString());
        if (id && host_.documents().find(*id)) order.push_back(*id);
    }
    if (order.size() != ids.size()) order = ids;
    const auto at = std::find(order.begin(), order.end(), activeFile());
    const auto n = static_cast<int>(order.size());
    const int i = at == order.end() ? 0 : static_cast<int>(at - order.begin());
    host_.documents().setActive(order[static_cast<std::size_t>(((i + step) % n + n) % n)]);
}

// An empty, untouched design not saved anywhere (like the one SketchBillet starts with).
static bool blankDesign(const model::Document* doc, bool saved) {
    return doc && doc->featureCount() == 0 && !doc->isModified() && !doc->canUndo() && !saved;
}

void Shell::addDesign(std::unique_ptr<model::Document> doc, std::filesystem::path path) {
    auto& docs = host_.documents();
    // An empty, untouched design gives way to the one coming in.
    const DocId old = activeFile();
    const bool blank = blankDesign(docs.find(old), paths_.count(old) > 0);
    const DocId id = docs.add(std::move(doc));
    if (!path.empty()) paths_[id] = std::move(path);
    docs.setActive(id);
    if (blank && old != id) {
        docs.close(old);
        paths_.erase(old);
    }
    refresh();
}

bool Shell::newDesign() {
    // A name no other open design has: Untitled, Untitled 2, ... (the blank design
    // this one replaces doesn't count).
    const DocId old = activeFile();
    const bool blank = blankDesign(host_.documents().find(old), paths_.count(old) > 0);
    auto taken = [&](const QString& name) {
        for (const auto& id : host_.documents().ids()) {
            if (!(blank && id == old) && designTitle(id) == name) return true;
        }
        return false;
    };
    QString name = tr("Untitled");
    for (int n = 2; taken(name); ++n) name = tr("Untitled %1").arg(n);
    addDesign(std::make_unique<model::Document>(model::DocKind::Part, name.toStdString()), {});
    return true;
}

bool Shell::newAssembly() {
    const DocId old = activeFile();
    const bool blank = blankDesign(host_.documents().find(old), paths_.count(old) > 0);
    auto taken = [&](const QString& name) {
        for (const auto& id : host_.documents().ids()) {
            if (!(blank && id == old) && designTitle(id) == name) return true;
        }
        return false;
    };
    QString name = tr("Untitled assembly");
    for (int n = 2; taken(name); ++n) name = tr("Untitled assembly %1").arg(n);
    auto doc = std::make_unique<model::Document>(model::DocKind::Part, name.toStdString());
    if (auto problems = doc->execute(std::make_unique<model::MakeAssemblyCommand>()); !problems.empty()) {
        showProblems(problems);
        return false;
    }
    doc->clearHistory();  // a new design: nothing to undo yet
    doc->markSaved();
    addDesign(std::move(doc), {});
    showMessage(tr("A new assembly. Put parts in it with Assemble > Insert part."));
    return true;
}

Diagnostics Shell::openDesign(const std::filesystem::path& path) {
    auto opened = io::openFile(path);
    if (!opened.ok()) {
        showProblems(opened.issues);
        return opened.issues;
    }
    if (host_.documents().find(opened.document->id())) {
        // Already open (the same design): just show its tab.
        host_.documents().setActive(opened.document->id());
        showMessage(tr("%1 is open already.").arg(designTitle(opened.document->id())));
        return {};
    }
    addDesign(std::move(opened.document), path);
    // An assembly's linked parts: updated from their files, found somewhere else, or not found.
    QStringList notes;
    for (const auto& note : opened.notes) notes << QString::fromStdString(note.message);
    showMessage(notes.isEmpty() ? tr("Opened %1").arg(QString::fromStdWString(path.filename().wstring()))
                                : notes.join(QLatin1Char(' ')));
    return {};
}

void Shell::setUpdates(UpdateChecker* checker) {
    updates_ = checker;
    if (!checker || !checker->allowed()) return;
    connect(checker, &UpdateChecker::finished, this,
            [this](const UpdateResult& result, bool byHand) { showUpdate(result, byHand); });
    helpMenu_->addSeparator();
    auto* now = helpMenu_->addAction(tr("Check for &Updates..."), this, [this] { updates_->check(true); });
    now->setObjectName(QStringLiteral("sb.shell.checkupdates"));
    now->setStatusTip(tr("Ask GitHub whether a newer SketchBillet is out. Nothing is downloaded."));
    // Copies built by hand have no daily checks (Updates.h, UpdateChannel::None).
    if (checker->channel() != UpdateChannel::None) {
        auto* daily = helpMenu_->addAction(tr("Check for Updates Every &Day"));
        daily->setObjectName(QStringLiteral("sb.shell.dailyupdates"));
        daily->setCheckable(true);
        daily->setChecked(checker->daily());
        daily->setStatusTip(
            tr("Once a day, when SketchBillet starts, ask GitHub whether a newer version is out."));
        connect(daily, &QAction::toggled, this, [this](bool on) { updates_->setDaily(on); });
    }
    if (checker->due(QDateTime::currentDateTimeUtc())) checker->check(false);
}

void Shell::showUpdate(const UpdateResult& result, bool byHand) {
    if (byHand) {
        showUpdateBox(result);
        return;
    }
    // A daily check only speaks up when there's something new; a failed one waits for tomorrow.
    if (result.kind != UpdateResult::Kind::Newer) return;
    if (!updateLabel_) {
        updateLabel_ = new QLabel(this);
        updateLabel_->setObjectName(QStringLiteral("sb.shell.updatenote"));
        updateLabel_->setOpenExternalLinks(true);
        statusBar()->addPermanentWidget(updateLabel_);
    }
    const QString what = result.version.isEmpty() ? tr("A newer preview build is out")
                                                  : tr("SketchBillet %1 is out").arg(result.version);
    updateLabel_->setText(
        QStringLiteral("<a href=\"%1\">%2</a>")
            .arg(result.url.toString().toHtmlEscaped(), (what + tr(": get it")).toHtmlEscaped()));
    updateLabel_->setToolTip(result.message);
    updateLabel_->show();
}

std::optional<Shell::CrashChoice> Shell::offerCrashReport(const QString& folder) {
    const auto reports = crash::pending(folder);
    if (reports.empty()) return std::nullopt;
    const auto& newest = reports.front();
    const auto body = crash::issueBody(newest);
    const auto choice = askCrashReport(QLocale().toString(newest.when.toLocalTime(), QLocale::ShortFormat),
                                       body, static_cast<int>(reports.size()));
    if (choice == CrashChoice::Report) openLink(crash::issueUrl(newest));
    if (choice == CrashChoice::Copy) {
        QGuiApplication::clipboard()->setText(body);
        showMessage(tr("The crash report is copied. Paste it into an email or a message to your teacher."));
    }
    crash::forget(folder);
    return choice;
}

void Shell::setSamplesFolder(const std::filesystem::path& folder) {
    samplesFolder_ = folder;
    samples_.clear();
    QFile index(QString::fromStdWString((folder / "index.json").wstring()));
    if (!folder.empty() && index.open(QIODevice::ReadOnly)) {
        for (const auto& item : QJsonDocument::fromJson(index.readAll()).array()) {
            const auto o = item.toObject();
            samples_.push_back(
                {o.value(QStringLiteral("key")).toString(), o.value(QStringLiteral("title")).toString(),
                 o.value(QStringLiteral("group")).toString(), o.value(QStringLiteral("about")).toString(),
                 o.value(QStringLiteral("file")).toString(), o.value(QStringLiteral("order")).toInt()});
        }
    }
    // By group, then in each group's own order, as on the reference parts page.
    std::sort(samples_.begin(), samples_.end(), [](const Sample& a, const Sample& b) {
        return a.group != b.group ? a.group < b.group : a.order < b.order;
    });
    samplesMenu_->clear();
    if (samples_.empty()) {
        samplesMenu_->addAction(tr("No samples found"))->setEnabled(false);
        return;
    }
    QMenu* group = nullptr;
    for (const auto& sample : samples_) {
        if (!group || group->title() != sample.group) group = samplesMenu_->addMenu(sample.group);
        auto* a = group->addAction(sample.title, this, [this, key = sample.key] { openSample(key); });
        a->setObjectName(QStringLiteral("sb.shell.sample.") + sample.key);
        a->setToolTip(sample.about);
        a->setStatusTip(sample.about);
    }
    for (auto* m : samplesMenu_->findChildren<QMenu*>()) m->setToolTipsVisible(true);
}

Diagnostics Shell::openSample(const QString& key) {
    const auto it =
        std::find_if(samples_.begin(), samples_.end(), [&](const Sample& s) { return s.key == key; });
    if (it == samples_.end()) {
        Diagnostics problems{{"sample.missing",
                              Severity::Error,
                              tr("There's no sample called %1.").arg(key).toStdString(),
                              {}}};
        showProblems(problems);
        return problems;
    }
    auto opened = io::openFile(samplesFolder_ / it->file.toStdWString());
    if (!opened.ok()) {
        showProblems(opened.issues);
        return opened.issues;
    }
    if (host_.documents().find(opened.document->id())) {
        // Already open (opened before, or saved from it): just show it.
        host_.documents().setActive(opened.document->id());
        return {};
    }
    addDesign(std::move(opened.document), {});  // no file yet: Save asks where
    showMessage(tr("Opened the sample %1. Save it to keep your changes.").arg(it->title));
    return {};
}

Diagnostics Shell::importPart(const std::filesystem::path& path) {
    const DocId doc = activeDesign();
    auto& docs = host_.documents();
    const auto* design = docs.find(doc);
    if (!design) return {};
    const auto before = design->featureCount();
    const auto u = path.u8string();
    auto problems = docs.run(doc, "feature.import", {{"file", std::string(u.begin(), u.end())}});
    if (!problems.empty()) {
        showProblems(problems);
        return problems;
    }
    design = docs.find(doc);
    if (design && design->featureCount() > before) {
        const auto& step = design->featureAt(design->featureCount() - 1);
        host_.selection().set(doc, {step.id()});  // Properties shows it, with Reload from file
        const auto* report = docs.lastRebuild(doc);
        const auto* r = report ? report->result(step.id()) : nullptr;
        if (r && r->status != model::FeatureStatus::Ok && !r->issues.empty()) {
            showProblems(r->issues);  // added, but the file has no part SketchBillet can read
            return r->issues;
        }
    }
    showMessage(tr("Imported %1").arg(QString::fromStdWString(path.filename().wstring())));
    return {};
}

Diagnostics Shell::saveDesign(const std::filesystem::path& path) {
    const DocId id = activeFile();
    const auto* doc = host_.documents().find(id);
    if (!doc) return {};
    auto problems = io::writeFile(*doc, path);
    if (!problems.empty()) {
        showProblems(problems);
        return problems;
    }
    host_.documents().markSaved(id);
    paths_[id] = path;
    // Open assemblies that link to this file use the part as it's saved now.
    QStringList using_;
    std::error_code ec;
    const auto saved = std::filesystem::weakly_canonical(path, ec);
    auto& docs = host_.documents();
    for (const auto& other : docs.ids()) {
        const auto* assembly = docs.find(other);
        const auto* link = assembly ? assembly->partLink(id) : nullptr;
        if (other == id || !link) continue;
        const auto linked = std::filesystem::weakly_canonical(
            std::filesystem::path(std::u8string(link->path.begin(), link->path.end())), ec);
        if (linked != saved) continue;
        if (docs.run(other, "part.update", {{"part", doc->name()}}).empty()) using_ << designTitle(other);
    }
    const QString file = QString::fromStdWString(path.filename().wstring());
    showMessage(
        using_.isEmpty()
            ? tr("Saved %1").arg(file)
            : tr("Saved %1. %2 uses the new %3 now.").arg(file, using_.join(tr(" and ")), qs(doc->name())));
    refresh();
    return {};
}

void Shell::finishEditingPart() {
    auto& docs = host_.documents();
    const DocId file = activeFile();
    const auto component = docs.editedComponent(file);
    if (!docs.finishEditing(file)) return;
    // Back in the assembly, with the component that was edited picked.
    if (docs.find(file) && docs.find(file)->findComponent(component))
        host_.selection().setComponents(file, {component});
    refresh();
}

// The picked step, if exactly one built step is picked; otherwise the one
// finished body. Otherwise, the problem's code and message.
std::optional<std::pair<std::string, QString>> Shell::bodyToUse(const model::RebuildReport& report,
                                                                FeatureId& which, const std::string& code,
                                                                const QString& verb,
                                                                const QString& onlyOne) const {
    const auto& picked = host_.selection().features();
    if (host_.selection().doc() == activeDesign() && picked.size() == 1 && report.shapeOf(picked.front())) {
        which = picked.front();
    } else if (report.bodies().size() == 1) {
        which = report.bodies().front();
    } else if (report.bodies().empty()) {
        return std::pair{code + ".nothing",
                         tr("There's nothing to %1 yet. Make a shape first, like Create > Box.").arg(verb)};
    } else {
        return std::pair{code + ".which",
                         tr("This design has %1 bodies, and %2. Pick the step to %3 in the feature tree, or "
                            "join them with Combine > Union.")
                             .arg(report.bodies().size())
                             .arg(onlyOne, verb)};
    }
    return std::nullopt;
}

model::PrintCheckResult Shell::checkForPrinting() {
    model::PrintCheckResult result;
    auto& docs = host_.documents();
    const auto* doc = docs.find(activeDesign());
    const auto* report = docs.lastRebuild(activeDesign());
    const auto* kernel = docs.kernel();
    FeatureId which{};
    if (!doc || !report || !kernel) {
        result.issues.push_back(
            {"print.nothing",
             Severity::Error,
             tr("There's nothing to check yet. Make a shape first, like Create > Box.").toStdString(),
             {}});
    } else if (auto problem =
                   bodyToUse(*report, which, "print", tr("check"), tr("Check for Printing looks at one"))) {
        result.issues.push_back({problem->first, Severity::Error, problem->second.toStdString(), {}});
    } else {
        result = model::checkPrint(*doc, *report, *kernel, which, {});
    }
    showMessage(result.ready() ? tr("Ready to print.") : tr("Not ready to print: see what the check found."));
    showPrintCheck(result);
    return result;
}

Diagnostics Shell::exportDesign(const std::filesystem::path& path) {
    auto fail = [this](std::string code, QString message) {
        Diagnostics problems{{std::move(code), Severity::Error, message.toStdString(), {}}};
        showProblems(problems);
        return problems;
    };
    auto& docs = host_.documents();
    const DocId id = activeDesign();
    const auto* doc = docs.find(id);
    const auto* report = docs.lastRebuild(id);
    const auto* kernel = docs.kernel();
    if (!doc || !report || !kernel) return fail("export.nothing", tr("There's nothing to export yet."));

    std::string ending = path.extension().string();
    std::transform(ending.begin(), ending.end(), ending.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    kernel::ExportFormat format{};
    const bool threeMf = ending == ".3mf";
    if (ending == ".stl") {
        format = kernel::ExportFormat::Stl;
    } else if (ending == ".step" || ending == ".stp") {
        format = kernel::ExportFormat::Step;
    } else if (!threeMf) {
        return fail("export.format",
                    tr("SketchBillet can export .stl and .3mf files (for 3D printing) and .step files (for "
                       "other CAD programs). End the file name with .stl, .3mf or .step."));
    }

    FeatureId which{};
    if (auto problem = bodyToUse(*report, which, "export", tr("export"), tr("Export writes one"))) {
        return fail(problem->first, problem->second);
    }
    const auto* shape = report->shapeOf(which);
    if (!shape) return fail("export.nothing", tr("That step didn't build, so there's nothing to export."));
    if (threeMf) {
        // 3MF carries the step's name and material too, so it's written from the design.
        if (auto problems = io::export3mf(*doc, *report, *kernel, which, path); !problems.empty()) {
            showProblems(problems);
            return problems;
        }
        const auto* feature = doc->find(which);
        showMessage(tr("Exported %1 to %2")
                        .arg(feature ? qs(feature->name()) : tr("the part"),
                             QString::fromStdWString(path.filename().wstring())));
        return {};
    }
    auto problems = kernel->exportFile(*shape, format, path);
    if (!problems.empty()) {
        // The kernel's words are short; say how to fix it, keeping its code and details.
        for (auto& d : problems) {
            d.message =
                tr("SketchBillet couldn't write %1. Check that the folder is still there and that you're "
                   "allowed to save in it.")
                    .arg(QString::fromStdWString(path.filename().wstring()))
                    .toStdString();
        }
        showProblems(problems);
        return problems;
    }
    const auto* feature = doc->find(which);
    showMessage(tr("Exported %1 to %2")
                    .arg(feature ? qs(feature->name()) : tr("the part"),
                         QString::fromStdWString(path.filename().wstring())));
    return {};
}

bool Shell::save() {
    auto path = activePath();
    if (path.empty()) {
        const auto* doc = host_.documents().find(activeFile());
        const auto chosen = askSavePath(qs(doc ? doc->name() : "Untitled") + QStringLiteral(".sb"));
        if (chosen.isEmpty()) return false;
        path = chosen.toStdWString();
    }
    return saveDesign(path).empty();
}

// ---------------------------------------------------------------------------
// Copy and paste
// ---------------------------------------------------------------------------

namespace {

constexpr auto kStepsMime = "application/x-sketchbillet-steps";

QString clipboardSteps() {
    const auto* data = QGuiApplication::clipboard()->mimeData();
    if (!data) return {};
    if (data->hasFormat(QString::fromLatin1(kStepsMime))) {
        return QString::fromUtf8(data->data(QString::fromLatin1(kStepsMime)));
    }
    const QString text = data->text();
    return io::looksLikeSteps(text.toStdString()) ? text : QString();
}

}  // namespace

void Shell::copySteps() {
    const auto& sel = host_.selection();
    const auto* doc = host_.documents().find(activeDesign());
    if (!doc || sel.doc() != activeDesign() || sel.features().empty()) {
        showProblems({{"copy.nothing",
                       Severity::Error,
                       tr("Pick one or more steps in the feature tree, then copy.").toStdString(),
                       {}}});
        return;
    }
    const std::string text = io::stepsToText(*doc, sel.features());
    auto* mime = new QMimeData;  // the clipboard owns it
    mime->setData(QString::fromLatin1(kStepsMime), QByteArray::fromStdString(text));
    mime->setText(QString::fromStdString(text));
    QGuiApplication::clipboard()->setMimeData(mime);
    const auto copied = io::stepsFromText(text).steps.size();
    showMessage(copied == 1 ? tr("Copied 1 step.") : tr("Copied %1 steps.").arg(copied));
}

void Shell::pasteSteps() {
    const DocId id = activeDesign();
    if (!host_.documents().find(id)) return;
    const QString text = clipboardSteps();
    if (text.isEmpty()) {
        showProblems({{"paste.not_steps",
                       Severity::Error,
                       tr("There are no steps to paste. Pick steps in the feature tree and copy them first.")
                           .toStdString(),
                       {}}});
        return;
    }
    auto pasted = io::stepsFromText(text.toStdString());
    if (!pasted.ok()) {
        showProblems(pasted.issues);
        return;
    }
    std::vector<FeatureId> ids;
    for (const auto& f : pasted.steps) ids.push_back(f->id());
    const std::string label =
        ids.size() == 1 ? std::string("Paste 1 step") : "Paste " + std::to_string(ids.size()) + " steps";
    auto problems =
        host_.documents().execute(id, std::make_unique<model::AddFeaturesCommand>(
                                          std::move(pasted.steps), std::move(pasted.materials), label));
    if (!problems.empty()) {
        showProblems(problems);
        return;
    }
    host_.selection().set(id, ids);
    showMessage(ids.size() == 1 ? tr("Pasted 1 step.") : tr("Pasted %1 steps.").arg(ids.size()));
}

void Shell::selectSteps(const QStringList& names) {
    const auto* doc = host_.documents().find(activeDesign());
    if (!doc) return;
    if (doc->kind() == model::DocKind::Assembly) {
        // An assembly's components, or its joints, by name.
        std::vector<model::ComponentId> picked;
        std::vector<JointId> joints;
        for (const auto& name : names) {
            for (const auto& c : doc->components()) {
                if (qs(c.name) == name) picked.push_back(c.id);
            }
            for (const auto& j : doc->joints()) {
                if (qs(j.name) == name) joints.push_back(j.id);
            }
        }
        if (!joints.empty())
            host_.selection().setJoints(activeDesign(), std::move(joints));
        else
            host_.selection().setComponents(activeDesign(), std::move(picked));
        return;
    }
    std::vector<FeatureId> ids;
    for (const auto& name : names) {
        for (std::size_t i = 0; i < doc->featureCount(); ++i) {
            if (qs(doc->featureAt(i).name()) == name) ids.push_back(doc->featureAt(i).id());
        }
    }
    host_.selection().set(activeDesign(), ids);
}

bool Shell::selectElements(const QString& body, kernel::TopoKind kind, const QList<int>& numbers) {
    const auto& docs = host_.documents();
    const auto* doc = docs.find(activeDesign());
    const auto* report = docs.lastRebuild(activeDesign());
    if (!doc || !report) return false;
    if (doc->kind() == model::DocKind::Assembly) {
        // A component's part's body: "Tray 1", or "Tray 1:Box1" for one of several.
        const auto colon = body.indexOf(QLatin1Char(':'));
        const auto* c = doc->findComponent(std::string_view(body.left(colon).toStdString()));
        const auto* part = c ? doc->findPart(c->part) : nullptr;
        const auto* built = c ? report->part(c->part) : nullptr;
        if (!part || !built || built->bodies().empty()) return false;
        FeatureId step = built->bodies().front();
        if (colon >= 0) {
            step = {};
            for (std::size_t i = 0; i < part->featureCount(); ++i) {
                if (qs(part->featureAt(i).name()) == body.mid(colon + 1)) step = part->featureAt(i).id();
            }
        }
        const auto* r = built->result(step);
        if (!r || !r->names) return false;
        const auto order = model::listingOrder(r->names->elements, kind);
        std::vector<plugin::PickedElement> picked;
        for (const int n : numbers) {
            if (n < 1 || n > static_cast<int>(order.size())) return false;
            picked.push_back(
                {step, model::referTo(*r->names, kind, order[static_cast<std::size_t>(n - 1)]), c->id});
        }
        host_.selection().setElements(activeDesign(), std::move(picked));
        return true;
    }
    for (std::size_t i = 0; i < doc->featureCount(); ++i) {
        const auto& f = doc->featureAt(i);
        if (qs(f.name()) != body) continue;
        const auto* r = report->result(f.id());
        if (!r || !r->names) return false;
        const auto order = model::listingOrder(r->names->elements, kind);
        std::vector<plugin::PickedElement> picked;
        for (const int n : numbers) {
            if (n < 1 || n > static_cast<int>(order.size())) return false;
            picked.push_back(
                {f.id(), model::referTo(*r->names, kind, order[static_cast<std::size_t>(n - 1)])});
        }
        // While measuring, these are what's measured; otherwise they're picked for a step.
        if (host_.selection().pickMode() != plugin::PickMode::Measure)
            host_.selection().setPickMode(kind == kernel::TopoKind::Face ? plugin::PickMode::Faces
                                                                         : plugin::PickMode::Edges);
        host_.selection().setElements(activeDesign(), std::move(picked));
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// What the window shows
// ---------------------------------------------------------------------------

void Shell::syncDesignTabs() {
    auto* tabs = ribbon_->designTabs();
    auto& docs = host_.documents();
    syncingTabs_ = true;
    // Tabs of designs that were closed go; new designs get a tab at the end.
    for (int i = tabs->count() - 1; i >= 0; --i) {
        const auto id = DocId::parse(tabs->tabData(i).toString().toStdString());
        if (!id || !docs.find(*id)) tabs->removeTab(i);
    }
    for (const auto& id : docs.ids()) {
        const QString key = QString::fromStdString(id.toString());
        int at = -1;
        for (int i = 0; i < tabs->count() && at < 0; ++i) {
            if (tabs->tabData(i).toString() == key) at = i;
        }
        if (at < 0) {
            at = tabs->addTab(QString());
            tabs->setTabData(at, key);
        }
        const auto* doc = docs.find(id);
        const QString title = designTitle(id);
        // A * after the name: changes that aren't saved yet.
        tabs->setTabText(at, doc && doc->isModified() ? title + QStringLiteral("*") : title);
        const auto path = paths_.find(id);
        tabs->setTabToolTip(at, path != paths_.end() ? QString::fromStdWString(path->second.wstring())
                                                     : tr("%1 isn't saved yet.").arg(title));
        if (id == docs.activeDesign()) tabs->setCurrentIndex(at);
    }
    syncingTabs_ = false;
}

void Shell::refresh() {
    refreshPending_ = false;
    auto& docs = host_.documents();
    // The open file: an assembly even while one of its parts is edited in it.
    const auto* file = docs.find(activeFile());
    const auto* doc = docs.find(activeDesign());
    setWindowTitle(tr("%1[*] - SketchBillet").arg(designTitle(activeFile())));
    setWindowModified(file && file->isModified());
    syncDesignTabs();
    if (activeDesign() != activeFile() && file && doc) {
        const auto* c = file->findComponent(docs.editedComponent(activeFile()));
        ribbon_->showEditing(
            tr("Editing %1 in %2, where %3 is. The rest of %2 is faded. Changes to %1 "
               "change every %1 in %2.")
                .arg(qs(doc->name()), designTitle(activeFile()), qs(c ? c->name : doc->name())));
    } else {
        ribbon_->hideEditing();
    }

    undo_->setEnabled(file && file->canUndo());
    redo_->setEnabled(file && file->canRedo());
    export_->setEnabled(doc != nullptr);
    copy_->setEnabled(doc && host_.selection().doc() == activeDesign() &&
                      !host_.selection().features().empty());
    paste_->setEnabled(doc && !clipboardSteps().isEmpty());
    undo_->setText(file && file->canUndo() ? tr("Undo %1").arg(qs(file->undoLabel())) : tr("Undo"));
    redo_->setText(file && file->canRedo() ? tr("Redo %1").arg(qs(file->redoLabel())) : tr("Redo"));

    refreshActions();
}

void Shell::showProblems(const Diagnostics& problems) {
    lastProblems_ = problems;
    QStringList lines;
    for (const auto& d : problems) lines << qs(d.message);
    problemLabel_->setText(lines.join(QStringLiteral("  ")));
}

void Shell::showMessage(const QString& message) {
    problemLabel_->clear();
    statusBar()->showMessage(message, 5000);
}

// ---------------------------------------------------------------------------
// Layouts
// ---------------------------------------------------------------------------

QStringList Shell::dockIds() const {
    QStringList ids = docks_->dockWidgetsMap().keys();
    ids.sort();
    return ids;
}

QWidget* Shell::panelWidget(const QString& id) const {
    if (auto it = overlays_.find(id); it != overlays_.end() && it->second.frame->body()) {
        return it->second.frame->body();
    }
    auto* dock = docks_->findDockWidget(id);
    return dock ? dock->widget() : nullptr;
}

void Shell::setOverView(const QString& id, bool over) {
    auto it = overlays_.find(id);
    if (it == overlays_.end()) return;
    auto& o = it->second;
    if (over && !o.frame->body()) o.shown = !o.dock->isClosed();  // open docked, open floating
    if (over && !o.frame->body()) {
        if (QWidget* w = o.dock->takeWidget()) o.frame->setBody(w);
    } else if (!over && o.frame->body()) {
        o.dock->setWidget(o.frame->takeBody(), ads::CDockWidget::ForceNoScrollArea);
    }
    // While it floats, its dock stays closed, and Panels > title (View tab) shows or hides
    // the floating frame instead.
    o.dock->toggleView(!over && o.shown);
    o.dock->toggleViewAction()->setVisible(!over);
    o.show->setVisible(over);
    o.over->setChecked(over);
    showOverlay(id, over && o.shown);
    settings_.setValue(panelKey(id, "overView"), over);
    syncPanelButton(id);
}

bool Shell::overView(const QString& id) const {
    auto it = overlays_.find(id);
    return it != overlays_.end() && it->second.frame->body();
}

QWidget* Shell::overlay(const QString& id) const {
    auto it = overlays_.find(id);
    return it == overlays_.end() ? nullptr : it->second.frame;
}

void Shell::showPanel(const QString& id, bool show) {
    if (auto it = overlays_.find(id); it != overlays_.end() && it->second.frame->body()) {
        if (show) it->second.frame->setFolded(false);
        showOverlay(id, show);
        return;
    }
    if (auto* dock = docks_->findDockWidget(id)) {
        dock->toggleView(show);
        if (!show) return;
        dock->raise();
        dock->setAsCurrentTab();
    }
}

void Shell::openPanel(const QString& id, bool open) {
    // A docked panel that was closed opens where it starts, as from Panels > title.
    if (auto it = overlays_.find(id);
        open && it != overlays_.end() && !it->second.frame->body() && it->second.dock->isClosed()) {
        setOverView(id, true);
    }
    showPanel(id, open);
    syncPanelButton(id);
}

void Shell::syncPanelButton(const QString& id) {
    auto b = panelButtons_.find(id);
    if (b == panelButtons_.end()) return;
    bool open = false;
    if (auto o = overlays_.find(id); o != overlays_.end() && o->second.frame->body()) {
        open = o->second.shown;
    } else if (const auto* dock = docks_->findDockWidget(id)) {
        open = !dock->isClosed();
    }
    b->second->setChecked(open);
}

void Shell::showOverlay(const QString& id, bool show) {
    auto& o = overlays_.at(id);
    if (o.frame->body()) o.shown = show;
    o.show->setChecked(show);
    o.frame->setVisible(show);
    if (show) o.frame->place();
    placeOverlaysAfter(id);  // the ones under it move up or down
    syncPanelButton(id);
}

void Shell::placeOverlaysAfter(const QString& id) {
    auto it = std::find(overlayOrder_.begin(), overlayOrder_.end(), id);
    if (it == overlayOrder_.end()) return;
    const bool right = overlays_.at(id).frame->right();
    // The next visible one on the same side; placing it places the ones after it.
    for (++it; it != overlayOrder_.end(); ++it) {
        auto* f = overlays_.at(*it).frame;
        if (f->right() == right && f->isVisible()) {
            f->place();
            return;
        }
    }
}

void Shell::keepOverlaysOut() {
    for (auto& [id, o] : overlays_) {
        if (o.frame->body() && !o.dock->isClosed()) o.dock->toggleView(false);
    }
}

QByteArray Shell::saveLayout() const {
    return docks_->saveState(kLayoutVersion);
}

bool Shell::restoreLayout(const QByteArray& layout) {
    if (!docks_->restoreState(layout, kLayoutVersion)) return false;
    keepOverlaysOut();
    bringPanelsOnScreen();
    return true;
}

void Shell::saveNamedLayout(const QString& name) {
    settings_.setValue(QLatin1String(kNamedLayouts) + QLatin1Char('/') + name, saveLayout());
    rebuildLayoutMenu();
    showMessage(tr("Saved the layout %1").arg(name));
}

bool Shell::openNamedLayout(const QString& name) {
    const auto saved = settings_.value(QLatin1String(kNamedLayouts) + QLatin1Char('/') + name).toByteArray();
    if (saved.isEmpty() || !restoreLayout(saved)) {
        showProblems({{"layout.bad",
                       Severity::Error,
                       tr("The layout %1 was made by another version of SketchBillet. Arrange the panels and "
                          "save it again.")
                           .arg(name)
                           .toStdString(),
                       {}}});
        return false;
    }
    return true;
}

QStringList Shell::layoutNames() const {
    settings_.beginGroup(QLatin1String(kNamedLayouts));
    QStringList names = settings_.childKeys();
    settings_.endGroup();
    names.sort(Qt::CaseInsensitive);
    return names;
}

void Shell::resetLayout() {
    restoreLayout(defaultLayout_);
    for (auto& [id, o] : overlays_) {
        o.frame->setFolded(false);
        o.frame->setWantedSize({o.frame->right() ? 360 : 300, ViewOverlay::kAsTallAsItNeeds});
        settings_.remove(panelKey(id, "overlaySize"));
        settings_.remove(panelKey(id, "overlayFolded"));
        setOverView(id, true);
        showOverlay(id, o.startsShown);  // open, or closed, as when SketchBillet starts
    }
}

int Shell::bringPanelsOnScreen() {
    int moved = 0;
    for (auto* floating : docks_->floatingWidgets()) {
        const QRect frame = floating->frameGeometry();
        bool visible = false;
        for (const auto* screen : QGuiApplication::screens()) {
            if (screen->availableGeometry().intersects(frame)) visible = true;
        }
        if (!visible) {
            const QRect home = QGuiApplication::primaryScreen()->availableGeometry();
            floating->move(home.center() - QPoint(frame.width() / 2, frame.height() / 2));
            ++moved;
        }
    }
    return moved;
}

void Shell::closeEvent(QCloseEvent* event) {
    if (!confirmCloseAll()) {
        event->ignore();
        return;
    }
    settings_.setValue(QLatin1String(kGeometry), saveGeometry());
    settings_.setValue(QLatin1String(kLastLayout), saveLayout());
    event->accept();
}

}  // namespace sb::app
