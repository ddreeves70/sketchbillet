#include "sb/plugins/Viewer.h"

#include <QAction>
#include <QActionGroup>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QResizeEvent>
#include <QSplitter>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "ViewerIcons.h"
#include "sb/kernel/Placement.h"
#include "sb/model/Document.h"
#include "sb/plugin/Events.h"
#include "sb/ui/UiRegistry.h"
#include "sb/view/FlatView.h"
#include "sb/view/Graphics.h"
#include "sb/view/Layout.h"
#include "sb/view/ViewWidget.h"
#include "sb/viewer/Viewer.h"
#include "sb/viewoptions/ViewOptions.h"

namespace sb::plugins {

namespace {

constexpr const char* kManifest = R"({
  "id": "sb.viewer",
  "name": "3D viewer",
  "version": "0.1.0",
  "description": "The 3D view of a design: panes, standard views, the view cube, and what other plugins ask it to draw.",
  "requires": ["sb.modeling"],
  "uses": ["sb.viewoptions"],
  "tier": "ui"
})";

QString qs(std::string_view s) {
    return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size()));
}

// "Two side by side" -> "sidebyside", for object names.
QString slug(std::string_view name) {
    QString out;
    for (const QChar c : qs(name).toLower()) {
        if (c.isLetterOrNumber()) out += c;
    }
    return out;
}

class ViewerPanel;

kernel::Vec3 cross(const kernel::Vec3& a, const kernel::Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

view::DrawingPlane drawingPlane(const viewer::ToolPlane& p) {
    return {p.origin, cross(p.xAxis, p.yAxis)};
}

// The sb.viewer/5 service: one drawing tool, one set of handles and one set of
// choices, for every pane of every viewer.
class ViewerService final : public viewer::IViewer {
public:
    void startTool(viewer::IViewTool& tool, const viewer::ToolPlane& plane) override;
    void stopTool(const viewer::IViewTool& tool) override;
    viewer::IViewTool* tool() const override { return tool_; }
    void face(const viewer::ToolPlane& plane) override;
    void setHandles(viewer::IHandleUser& user, std::vector<viewer::Handle> handles) override;
    void clearHandles(const viewer::IHandleUser& user) override;

    std::vector<viewer::Handle> handles() const override { return given_; }
    viewer::IHandleUser* handleUser() const override { return handleUser_; }
    void previewMove(const FeatureId& body, const kernel::Motion& motion) override;
    void previewComponent(const InstanceId& component, const kernel::Motion& motion) override;
    void clearPreview() override;
    std::optional<viewer::BodyPreview> preview() const override { return preview_; }
    void offerChoices(viewer::IChoiceUser& user, std::vector<viewer::Choice> choices) override;
    void clearChoices(const viewer::IChoiceUser& user) override;
    std::vector<viewer::Choice> choices() const override { return choices_; }
    viewer::IChoiceUser* choiceUser() const override { return choiceUser_; }
    void askNumbers(viewer::INumberUser& user, std::vector<viewer::NumberBox> boxes,
                    const std::string& focus) override;
    void clearNumbers(const viewer::INumberUser& user) override;
    std::vector<viewer::NumberBox> numberBoxes() const override { return numbers_; }
    viewer::INumberUser* numberUser() const override { return numberUser_; }

    // The number boxes, as the view shows them.
    std::vector<view::ViewInput> viewInputs() const {
        std::vector<view::ViewInput> out;
        for (const auto& b : numbers_) out.push_back({b.id, b.at, b.text});
        return out;
    }

    // The choices to draw, as the view draws them.
    std::vector<view::ViewChoice> viewChoices() const {
        std::vector<view::ViewChoice> out;
        for (const auto& c : choices_)
            out.push_back({c.id, c.label, c.origin, c.xAxis, c.yAxis, c.from, c.to, true, {c.r, c.g, c.b}});
        return out;
    }

    // The handles to draw, as the view draws them.
    const std::vector<view::ViewHandle>& viewHandles() const { return handles_; }

    // Where the running tool draws, for panes made while it runs.
    std::optional<view::DrawingPlane> plane() const {
        return tool_ ? std::optional<view::DrawingPlane>(drawingPlane(plane_)) : std::nullopt;
    }
    void attach(ViewerPanel* panel) { panels_.push_back(panel); }
    void detach(ViewerPanel* panel) { std::erase(panels_, panel); }

private:
    void applyAll();

    void showHandlesAll();
    void showPreviewAll();
    void showChoicesAll();
    void showNumbersAll(const std::string& focus);

    viewer::IViewTool* tool_ = nullptr;
    viewer::ToolPlane plane_{};
    viewer::IHandleUser* handleUser_ = nullptr;
    std::vector<view::ViewHandle> handles_;
    std::vector<viewer::Handle> given_;
    std::optional<viewer::BodyPreview> preview_;
    viewer::IChoiceUser* choiceUser_ = nullptr;
    std::vector<viewer::Choice> choices_;
    viewer::INumberUser* numberUser_ = nullptr;
    std::vector<viewer::NumberBox> numbers_;
    std::vector<ViewerPanel*> panels_;
};

// The view bar: a row of buttons floating at the bottom middle of the viewer,
// see-through, like Fusion's navigation bar.
class ViewBar final : public QWidget {
public:
    explicit ViewBar(QWidget* parent) : QWidget(parent) {
        setObjectName(QStringLiteral("sb.viewer.bar"));
        setAttribute(Qt::WA_TranslucentBackground);  // paintEvent draws its rounded back
        layout_ = new QHBoxLayout(this);
        layout_->setContentsMargins(6, 3, 6, 3);
        layout_->setSpacing(1);
    }

    // A button for an action (it takes the action's icon, tip and check mark).
    QToolButton* add(QAction* action) {
        auto* b = new QToolButton(this);
        b->setDefaultAction(action);
        b->setAutoRaise(true);
        b->setIconSize(QSize(20, 20));
        layout_->addWidget(b);
        return b;
    }

    // A button that opens a menu.
    QToolButton* addMenu(const QIcon& icon, const QString& name, const QString& tip, QMenu* menu) {
        auto* b = new QToolButton(this);
        b->setObjectName(name);
        b->setIcon(icon);
        b->setToolTip(tip);
        b->setAutoRaise(true);
        b->setIconSize(QSize(20, 20));
        b->setPopupMode(QToolButton::InstantPopup);
        b->setMenu(menu);
        layout_->addWidget(b);
        return b;
    }

    void addSeparator() {
        auto* line = new QFrame(this);
        line->setFrameShape(QFrame::VLine);
        line->setFrameShadow(QFrame::Sunken);
        line->setFixedHeight(20);
        layout_->addWidget(line);
    }

    // At the bottom middle of the viewer.
    void place() {
        auto* host = parentWidget();
        if (!host) return;
        adjustSize();
        move((host->width() - width()) / 2, host->height() - height() - kMargin);
        raise();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        QColor back = palette().color(QPalette::Window);
        back.setAlpha(200);
        QColor line = palette().color(QPalette::Mid);
        line.setAlpha(160);
        p.setPen(line);
        p.setBrush(back);
        p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 6, 6);
    }

private:
    static constexpr int kMargin = 10;  // pixels from the viewer's bottom edge
    QHBoxLayout* layout_ = nullptr;
};

// The panel: 1, 2 or 4 panes, with the view bar floating at the bottom. Each
// pane is a ViewWidget with its own camera; the bar's views turn the active
// pane (the one last clicked). Without 3D graphics, a FlatView takes the panes'
// place.
class ViewerPanel final : public QWidget {
public:
    // `number` counts the viewers opened (1 for the first), so each pane has a
    // key the view options can remember, like "viewer1.pane2".
    ViewerPanel(plugin::PluginContext& ctx, ViewerService& service, int number)
        : ctx_(ctx), service_(service), options_(ctx.service<viewoptions::IViewOptions>()), number_(number) {
        service_.attach(this);
        setObjectName(QStringLiteral("sb.viewer.panel"));
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);

        body_ = new QWidget(this);
        auto* bodyLayout = new QVBoxLayout(body_);
        bodyLayout->setContentsMargins(0, 0, 0, 0);
        layout->addWidget(body_, 1);

        if (const auto saved = view::parseDisplayStyle(ctx_.settings().text("display_style")))
            style_ = *saved;
        bar_ = new ViewBar(this);
        buildBar();

        using namespace plugin;
        auto later = [this](const auto&) { scheduleRefresh(); };
        subs_.push_back(ctx_.events().subscribe<ActiveDocumentChanged>(later));
        subs_.push_back(ctx_.events().subscribe<DocumentClosed>(later));
        subs_.push_back(ctx_.events().subscribe<RebuildFinished>(later));
        subs_.push_back(ctx_.events().subscribe<SelectionChanged>(later));
        subs_.push_back(ctx_.events().subscribe<PickModeChanged>(later));
        subs_.push_back(
            ctx_.events().subscribe<OverlaysChanged>([this](const OverlaysChanged&) { showOverlays(); }));
        subs_.push_back(ctx_.events().subscribe<viewoptions::ViewOptionChanged>(
            [this](const viewoptions::ViewOptionChanged&) { showOwnBars(); }));

        problem_ = view::graphicsProblem();
        if (problem_.isEmpty()) {
            setPaneLayout(view::PaneLayout::Single);
        } else {
            showFlat();
        }
        refresh();
    }

    ~ViewerPanel() override {
        service_.detach(this);
        // The window is closing: forget the panes, but keep their own options
        // for next time.
        if (options_) {
            for (std::size_t i = 0; i < panes_.size(); ++i) options_->removePane(paneKey(i), false);
        }
    }

    // The running tool's plane (or none) in every pane.
    void applyTool() {
        const auto plane = service_.plane();
        for (auto* pane : panes_) pane->setDrawingPlane(plane);
        if (flat_) flat_->setDrawingPlane(plane);
    }

    // The handles in every pane.
    void applyHandles() {
        for (auto* pane : panes_) pane->setHandles(service_.viewHandles());
    }

    // The body drawn moved (or none) in every pane.
    void applyPreview() {
        for (auto* pane : panes_) showPreview(*pane);
    }
    void showPreview(view::ViewWidget& pane) {
        if (const auto p = service_.preview(); p && !p->component.isNil())
            pane.setComponentPreview(p->component, p->motion);
        else if (p)
            pane.setPreview(p->body, p->motion);
        else
            pane.clearPreview();
    }

    // The number boxes in the active pane (and the flat view); none in the others.
    void applyNumbers(const std::string& focus = {}) {
        const auto inputs = service_.viewInputs();
        for (auto* pane : panes_)
            pane->setInputs(pane == activePane() ? inputs : std::vector<view::ViewInput>{},
                            pane == activePane() ? focus : std::string());
        if (flat_) flat_->setInputs(inputs, focus);
    }

    // The choices in every pane, and in the flat view.
    void applyChoices() {
        const auto choices = service_.viewChoices();
        for (auto* pane : panes_) pane->setChoices(choices);
        if (flat_) flat_->setChoices(choices);
    }

    // Turns the active pane to look straight at a plane.
    void face(const viewer::ToolPlane& plane) {
        if (auto* pane = activePane()) pane->lookAlong({drawingPlane(plane).normal, plane.yAxis});
    }

private:
    // Sends a view's drawing events to the running tool.
    template <typename View>
    void forward(View* view, std::string key) {
        auto point = [key](const view::PlanePoint& p) {
            return viewer::ToolPoint{p.at, p.pixel, key, p.add};
        };
        connect(view, &View::planeMoved, this, [this, point](const view::PlanePoint& p) {
            if (auto* t = service_.tool()) t->moved(point(p));
        });
        connect(view, &View::planeClicked, this, [this, point](const view::PlanePoint& p) {
            if (auto* t = service_.tool()) t->clicked(point(p));
        });
        connect(view, &View::planeDragged, this,
                [this, point](const view::PlanePoint& from, const view::PlanePoint& to) {
                    if (auto* t = service_.tool()) t->dragged(point(from), point(to));
                });
        connect(view, &View::planeDropped, this,
                [this, point](const view::PlanePoint& from, const view::PlanePoint& to) {
                    if (auto* t = service_.tool()) t->dropped(point(from), point(to));
                });
        connect(view, &View::planeCancelled, this, [this] {
            if (auto* t = service_.tool()) t->cancelled();
        });
    }

    // Sends what's typed in a view's number boxes to whoever asked for them.
    template <typename View>
    void forwardNumbers(View* view) {
        connect(view, &View::inputTyped, this, [this](const std::string& id, const QString& text) {
            if (auto* u = service_.numberUser()) u->typed(id, text.toStdString());
        });
        connect(view, &View::inputEntered, this, [this](const std::string& id) {
            if (auto* u = service_.numberUser()) u->entered(id);
        });
        connect(view, &View::inputsCancelled, this, [this] {
            if (auto* u = service_.numberUser()) u->cancelled();
        });
    }

    // Sends a view's clicks on choices to whoever offered them.
    template <typename View>
    void forwardChoices(View* view) {
        connect(view, &View::choiceClicked, this, [this](const std::string& id) {
            if (auto* u = service_.choiceUser()) u->chosen(id);
        });
        connect(view, &View::choicesCancelled, this, [this] {
            if (auto* u = service_.choiceUser()) u->cancelled();
        });
    }

    // Sends a pane's handle drags to whoever put the handles there.
    void forwardHandles(view::ViewWidget* pane) {
        connect(pane, &view::ViewWidget::handleDragged, this,
                [this](const std::string& id, const kernel::Vec3& by) {
                    if (auto* u = service_.handleUser()) u->dragged(id, by);
                });
        connect(pane, &view::ViewWidget::handleDropped, this,
                [this](const std::string& id, const kernel::Vec3& by) {
                    if (auto* u = service_.handleUser()) u->dropped(id, by);
                });
        connect(pane, &view::ViewWidget::handleCancelled, this, [this] {
            if (auto* u = service_.handleUser()) u->cancelled();
        });
    }

    std::string paneKey(std::size_t i) const {
        return "viewer" + std::to_string(number_) + ".pane" + std::to_string(i + 1);
    }

    // "Viewer 1", or "Viewer 1, top left pane" when there's more than one.
    std::string paneName(std::size_t i) const {
        static const char* const two[2][2] = {{"left", "right"}, {"top", "bottom"}};
        static const char* const four[] = {"top left", "top right", "bottom left", "bottom right"};
        const std::string viewer = "Viewer " + std::to_string(number_);
        switch (layout_) {
            case view::PaneLayout::Single:
                return viewer;
            case view::PaneLayout::SideBySide:
                return viewer + ", " + two[0][i % 2] + " pane";
            case view::PaneLayout::Stacked:
                return viewer + ", " + two[1][i % 2] + " pane";
            case view::PaneLayout::Four:
                return viewer + ", " + four[i % 4] + " pane";
        }
        return viewer;
    }

    QIcon icon(std::string_view svg) const {
        auto* ui = ctx_.service<ui::UiRegistry>();
        return ui ? ui->icon(svg) : QIcon();
    }

    QAction* action(const QString& id, const QString& text, const QString& tip, QObject* parent) {
        auto* a = new QAction(text, parent);
        a->setObjectName(id);
        a->setToolTip(tip);
        a->setStatusTip(tip);
        return a;
    }

    // The view bar, left to right: what a left drag does (orbit, pan, zoom),
    // Fit all, the standard views, how bodies are drawn, the grid, and panes.
    void buildBar() {
        namespace i = icons::viewer;
        auto* drags = new QActionGroup(this);
        auto addDrag = [&](view::LeftDrag drag, const char* id, const QString& text, const QString& tip,
                           std::string_view svg) {
            auto* a = action(QString::fromLatin1(id), text, tip, this);
            a->setIcon(icon(svg));
            a->setCheckable(true);
            a->setChecked(drag == drag_);
            drags->addAction(a);
            connect(a, &QAction::triggered, this, [this, drag] { setLeftDrag(drag); });
            bar_->add(a);
        };
        addDrag(view::LeftDrag::Orbit, "sb.viewer.drag.orbit", tr("Orbit"),
                tr("Orbit: dragging with the left mouse button turns the camera around the part."),
                i::orbit_svg);
        addDrag(view::LeftDrag::Pan, "sb.viewer.drag.pan", tr("Pan"),
                tr("Pan: dragging with the left mouse button slides the camera sideways. Click Orbit to go "
                   "back."),
                i::pan_svg);
        addDrag(view::LeftDrag::Zoom, "sb.viewer.drag.zoom", tr("Zoom"),
                tr("Zoom: drag right with the left mouse button to move closer, left to move away. The wheel "
                   "zooms too."),
                i::zoom_svg);
        auto* fit = action(QStringLiteral("sb.viewer.fit"), tr("Fit all"),
                           tr("Fit all: zoom so every body fits, in every pane."), this);
        fit->setIcon(icon(i::fit_svg));
        connect(fit, &QAction::triggered, this, [this] {
            for (auto* pane : panes_) pane->fitAll();
        });
        bar_->add(fit);
        bar_->addSeparator();

        // Views: every standard view, for the active pane.
        auto* views = new QMenu(this);
        for (auto v : {view::StandardView::Front, view::StandardView::Top, view::StandardView::Right,
                       view::StandardView::Iso, view::StandardView::Back, view::StandardView::Left,
                       view::StandardView::Bottom}) {
            if (v == view::StandardView::Back) views->addSeparator();
            auto* a =
                action(QStringLiteral("sb.viewer.view.") + slug(view::viewName(v)), qs(view::viewName(v)),
                       tr("Look at the design from the %1, in the active pane.")
                           .arg(qs(view::viewName(v)).toLower()),
                       views);
            connect(a, &QAction::triggered, this, [this, v] {
                if (auto* pane = activePane()) pane->setStandardView(v);
            });
            views->addAction(a);
        }
        bar_->addMenu(icon(i::views_svg), QStringLiteral("sb.viewer.views"),
                      tr("Views: look from the front, top, right, at an angle (Iso), and more."), views);

        // Display: how bodies are drawn, and the view options that aren't the grid's (the axes).
        auto* display = new QMenu(this);
        auto* styles = new QActionGroup(display);
        for (auto style : view::allDisplayStyles()) {
            auto* a = action(QStringLiteral("sb.viewer.style.") + slug(view::displayStyleName(style)),
                             qs(view::displayStyleName(style)), QString(), display);
            a->setCheckable(true);
            a->setChecked(style == style_);
            styles->addAction(a);
            display->addAction(a);
            connect(a, &QAction::triggered, this, [this, style] { setDisplayStyle(style); });
        }
        connect(display, &QMenu::aboutToShow, this, [this, display] {
            clearOptions(display);
            fillOptions(display, [](const viewoptions::OptionSpec& o) { return o.group != "Grid"; });
        });
        bar_->addMenu(icon(i::display_svg), QStringLiteral("sb.viewer.display"),
                      tr("Display: draw bodies shaded, with or without edges, or as see-through wireframes; "
                         "show or hide the axes."),
                      display);

        // Grid: the grid's view options, and the View options panel.
        if (options_) {
            auto* grid = new QMenu(this);
            connect(grid, &QMenu::aboutToShow, this, [this, grid] {
                grid->clear();
                fillOptions(grid, [](const viewoptions::OptionSpec& o) { return o.group == "Grid"; });
                grid->addSeparator();
                auto* all = grid->addAction(tr("All view options..."));
                all->setObjectName(QStringLiteral("sb.viewer.alloptions"));
                connect(all, &QAction::triggered, this, [this] {
                    if (auto* ui = ctx_.service<ui::UiRegistry>()) ui->showPanel("sb.viewoptions.panel");
                });
            });
            bar_->addMenu(icon(i::grid_svg), QStringLiteral("sb.viewer.grid"),
                          tr("Grid and snap: show the grid, its spacing and style, and snapping to it."),
                          grid);
        }
        bar_->addSeparator();

        // Panes: one, two or four, each with its own camera.
        auto* panes = new QMenu(this);
        auto* group = new QActionGroup(panes);
        for (auto layout : view::allLayouts()) {
            auto* a = panes->addAction(qs(view::layoutName(layout)));
            a->setObjectName(QStringLiteral("sb.viewer.layout.") + slug(view::layoutName(layout)));
            a->setCheckable(true);
            a->setChecked(layout == view::PaneLayout::Single);
            group->addAction(a);
            connect(a, &QAction::triggered, this, [this, layout] { setPaneLayout(layout); });
        }
        bar_->addMenu(icon(i::panes_svg), QStringLiteral("sb.viewer.panes"),
                      tr("Panes: split the viewer into 1, 2 or 4 panes, each with its own camera."), panes);
        bar_->place();
    }

    // Removes the view options a menu listed last time (the items after the
    // display styles), so they're listed fresh.
    void clearOptions(QMenu* menu) {
        const auto items = menu->actions();
        for (auto* a : items) {
            if (!a->objectName().startsWith(QStringLiteral("sb.viewer.style."))) {
                menu->removeAction(a);
                if (a->parent() == menu) a->deleteLater();
            }
        }
    }

    // Lists view options in a menu, under their groups' names, the way the
    // View menu does: check marks, choices and sizes. They change every pane.
    void fillOptions(QMenu* menu, const std::function<bool(const viewoptions::OptionSpec&)>& which) {
        if (!options_) return;
        std::string group;
        for (const auto* o : options_->options()) {
            if (!which(*o)) continue;
            if (o->group != group) {
                group = o->group;
                menu->addSection(qs(group));
            }
            const std::string id = o->id;
            switch (o->kind) {
                case viewoptions::OptionKind::Flag: {
                    auto* a = menu->addAction(qs(o->text));
                    a->setCheckable(true);
                    a->setChecked(options_->flag(id));
                    a->setToolTip(qs(o->tip));
                    connect(a, &QAction::triggered, this, [this, id](bool on) { options_->set(id, on); });
                    break;
                }
                case viewoptions::OptionKind::Choice: {
                    auto* sub = menu->addMenu(qs(o->text));
                    const auto now = options_->choice(id);
                    for (const auto& c : o->choices) {
                        auto* a = sub->addAction(qs(c.text));
                        a->setCheckable(true);
                        a->setChecked(c.value == now);
                        const std::string value = c.value;
                        connect(a, &QAction::triggered, this,
                                [this, id, value] { options_->set(id, value); });
                    }
                    break;
                }
                case viewoptions::OptionKind::Length:
                case viewoptions::OptionKind::Number: {
                    auto* sub = menu->addMenu(qs(o->text));
                    const double now = options_->number(id);
                    const bool mm = o->kind == viewoptions::OptionKind::Length;
                    for (const double v : o->presets) {
                        const QString text = qs(viewoptions::numberText(v));
                        auto* a = sub->addAction(mm ? tr("%1 mm").arg(text) : text);
                        a->setCheckable(true);
                        a->setChecked(std::abs(v - now) < 1e-9);
                        connect(a, &QAction::triggered, this, [this, id, v] { options_->set(id, v); });
                    }
                    sub->addSeparator();
                    connect(sub->addAction(tr("Other...")), &QAction::triggered, this,
                            [this, id] { options_->ask(id, {}); });
                    break;
                }
            }
        }
    }

    void setLeftDrag(view::LeftDrag drag) {
        drag_ = drag;
        for (auto* pane : panes_) pane->setLeftDrag(drag);
    }

    void setDisplayStyle(view::DisplayStyle style) {
        style_ = style;
        for (auto* pane : panes_) pane->setDisplayStyle(style);
        ctx_.settings().set("display_style", view::displayStyleName(style));  // remembered next time
    }

    void resizeEvent(QResizeEvent* event) override {
        QWidget::resizeEvent(event);
        if (bar_) bar_->place();
    }

    // Splits the viewer. Panes are kept when they can be, and every pane turns
    // to the layout's starting view.
    void setPaneLayout(view::PaneLayout layout) {
        if (!problem_.isEmpty()) return;
        layout_ = layout;
        const int count = view::paneCount(layout);
        while (static_cast<int>(panes_.size()) > count) {
            // Closed for good: the pane's own options go with it.
            if (options_) options_->removePane(paneKey(panes_.size() - 1), true);
            bars_.pop_back();
            delete frames_.back();  // deletes its pane too
            frames_.pop_back();
            panes_.pop_back();
        }
        while (static_cast<int>(panes_.size()) < count) addPane();
        active_ = std::min(active_, count - 1);

        // Put the frames into fresh splitters, then throw the old ones away.
        auto* old = splitter_;
        auto splitter = [](Qt::Orientation o) {
            auto* s = new QSplitter(o);
            s->setChildrenCollapsible(false);
            s->setHandleWidth(4);
            return s;
        };
        switch (layout) {
            case view::PaneLayout::Single:
            case view::PaneLayout::SideBySide:
                splitter_ = splitter(Qt::Horizontal);
                for (auto* f : frames_) splitter_->addWidget(f);
                break;
            case view::PaneLayout::Stacked:
                splitter_ = splitter(Qt::Vertical);
                for (auto* f : frames_) splitter_->addWidget(f);
                break;
            case view::PaneLayout::Four: {
                splitter_ = splitter(Qt::Vertical);
                for (int row = 0; row < 2; ++row) {
                    auto* r = splitter(Qt::Horizontal);
                    r->addWidget(frames_[static_cast<std::size_t>(row * 2)]);
                    r->addWidget(frames_[static_cast<std::size_t>(row * 2 + 1)]);
                    splitter_->addWidget(r);
                }
                break;
            }
        }
        body_->layout()->addWidget(splitter_);
        delete old;
        const auto views = view::defaultViews(layout);
        for (std::size_t i = 0; i < panes_.size(); ++i) panes_[i]->setStandardView(views[i]);
        if (auto* a =
                findChild<QAction*>(QStringLiteral("sb.viewer.layout.") + slug(view::layoutName(layout))))
            a->setChecked(true);
        // Tell the view options about the panes (their names depend on the layout).
        if (options_) {
            for (std::size_t i = 0; i < panes_.size(); ++i) options_->addPane({paneKey(i), paneName(i)});
        }
        showActive();
        showOverlays();
        showOwnBars();
    }

    void addPane() {
        auto* frame = new QFrame;
        frame->setObjectName(QStringLiteral("sb.viewer.paneframe"));
        auto* l = new QVBoxLayout(frame);
        l->setContentsMargins(2, 2, 2, 2);
        l->setSpacing(0);
        const std::size_t index = panes_.size();

        // Shown when the pane has its own view options.
        auto* bar = new QWidget(frame);
        bar->setObjectName(QStringLiteral("sb.viewer.ownbar"));
        bar->setStyleSheet(QStringLiteral("QWidget#sb\\.viewer\\.ownbar { background: #fff4d6; }"));
        auto* barLayout = new QHBoxLayout(bar);
        barLayout->setContentsMargins(6, 2, 2, 2);
        auto* note = new QLabel(tr("Own view options"), bar);
        note->setObjectName(QStringLiteral("sb.viewer.ownnote"));
        barLayout->addWidget(note, 1);
        auto* same = new QToolButton(bar);
        same->setObjectName(QStringLiteral("sb.viewer.ownreset"));
        same->setText(tr("Same as all panes"));
        same->setToolTip(tr("Use the same view options as all the other panes."));
        connect(same, &QToolButton::clicked, this, [this, index] {
            if (options_) options_->resetPane(paneKey(index));
        });
        barLayout->addWidget(same);
        bar->hide();
        l->addWidget(bar);
        bars_.push_back(bar);

        auto* pane = new view::ViewWidget(frame);
        pane->setObjectName(QStringLiteral("sb.viewer.pane"));
        pane->setLeftDrag(drag_);
        pane->setDisplayStyle(style_);
        l->addWidget(pane);
        pane->setContent(content_);
        pane->setOverlays(viewOverlays(ctx_.overlays().forPane(paneKey(index))));
        connect(pane, &view::ViewWidget::picked, this, [this](const std::vector<FeatureId>& ids) {
            const DocId doc = ctx_.documents().active();
            if (ctx_.documents().find(doc)) ctx_.selection().set(doc, ids);
        });
        connect(pane, &view::ViewWidget::componentsPicked, this, [this](const std::vector<InstanceId>& ids) {
            const DocId doc = ctx_.documents().active();
            if (!ctx_.documents().find(doc)) return;
            if (ids.empty())
                ctx_.selection().clear();
            else
                ctx_.selection().setComponents(doc, ids);
        });
        auto picked = [this](const std::vector<view::ScenePick>& picks) {
            const DocId doc = ctx_.documents().active();
            if (ctx_.documents().find(doc))
                ctx_.selection().setElements(doc, pickedEdges(ctx_.documents(), picks));
        };
        connect(pane, &view::ViewWidget::edgesPicked, this, picked);
        connect(pane, &view::ViewWidget::facesPicked, this, picked);
        connect(pane, &view::ViewWidget::measurePicked, this, picked);
        connect(pane, &view::ViewWidget::activated, this, [this, pane] {
            const auto it = std::find(panes_.begin(), panes_.end(), pane);
            if (it == panes_.end()) return;
            active_ = static_cast<int>(it - panes_.begin());
            if (options_) options_->setActivePane(paneKey(static_cast<std::size_t>(active_)));
            showActive();
        });
        connect(pane, &view::ViewWidget::menuRequested, this, [this, index](QPoint at) {
            if (options_) options_->showPaneMenu(paneKey(index), at.x(), at.y());
        });
        connect(pane, &view::ViewWidget::failed, this, [this](const QString& why) {
            problem_ = why;
            QMetaObject::invokeMethod(this, [this] { showFlat(); }, Qt::QueuedConnection);
        });
        forward(pane, paneKey(index));
        forwardHandles(pane);
        forwardChoices(pane);
        forwardNumbers(pane);
        pane->setDrawingPlane(service_.plane());
        pane->setHandles(service_.viewHandles());
        showPreview(*pane);
        pane->setChoices(service_.viewChoices());
        frames_.push_back(frame);
        panes_.push_back(pane);
    }

    // A thin blue frame marks the active pane, when there is more than one.
    void showActive() {
        for (std::size_t i = 0; i < frames_.size(); ++i) {
            const bool on = panes_.size() > 1 && static_cast<int>(i) == active_;
            frames_[i]->setStyleSheet(
                on ? QStringLiteral("QFrame#sb\\.viewer\\.paneframe { border: 2px solid #3d8bfd; }")
                   : QStringLiteral("QFrame#sb\\.viewer\\.paneframe { border: 2px solid transparent; }"));
        }
        applyNumbers();  // number boxes go with the active pane
    }

    view::ViewWidget* activePane() const {
        if (panes_.empty()) return nullptr;
        return panes_[static_cast<std::size_t>(std::clamp(active_, 0, static_cast<int>(panes_.size()) - 1))];
    }

    // 3D graphics can't work here: the flat view, and a card saying why.
    void showFlat() {
        if (flat_) return;
        for (auto* f : frames_) f->hide();
        delete splitter_;
        splitter_ = nullptr;
        if (options_) {
            for (std::size_t i = 0; i < panes_.size(); ++i) options_->removePane(paneKey(i), false);
        }
        frames_.clear();
        panes_.clear();
        bars_.clear();
        bar_->hide();  // nothing to turn, split or redraw in the flat view
        flat_ = new view::FlatView(body_);
        body_->layout()->addWidget(flat_);
        flat_->setOverlays(viewOverlays(ctx_.overlays().forPane({})));
        forward(flat_, paneKey(0));
        forwardChoices(flat_);
        forwardNumbers(flat_);
        flat_->setDrawingPlane(service_.plane());
        flat_->setChoices(service_.viewChoices());
        flat_->setInputs(service_.viewInputs());
        refresh();
    }

    // Plugins' overlays (the grid, ...): the viewer draws them without knowing
    // what they are. A pane draws its own overlays instead of the every-pane
    // ones with the same name.
    void showOverlays() {
        for (std::size_t i = 0; i < panes_.size(); ++i)
            panes_[i]->setOverlays(viewOverlays(ctx_.overlays().forPane(paneKey(i))));
        if (flat_) flat_->setOverlays(viewOverlays(ctx_.overlays().forPane({})));
    }

    // The note above a pane that has its own view options, naming them.
    void showOwnBars() {
        for (std::size_t i = 0; i < bars_.size(); ++i) {
            const auto own = options_ ? options_->ownOptions(paneKey(i)) : std::vector<std::string>{};
            QStringList names;
            for (const auto& id : own) {
                if (const auto* o = options_->find(id)) names << qs(o->text);
            }
            bars_[i]->setVisible(!own.empty());
            bars_[i]->setToolTip(tr("Different in this pane: %1. Right-click in the pane to change them.")
                                     .arg(names.join(QStringLiteral(", "))));
        }
    }

    void scheduleRefresh() {
        if (refreshPending_) return;
        refreshPending_ = true;
        QMetaObject::invokeMethod(this, [this] { refresh(); }, Qt::QueuedConnection);
    }

    void refresh() {
        refreshPending_ = false;
        switchCameras();
        content_ = viewerScene(ctx_.documents(), ctx_.selection());
        for (auto* pane : panes_) pane->setContent(content_);
        if (flat_) flat_->setText(flatText());
    }

    // Each open design keeps its own camera in each pane: switching to another
    // design keeps this one's, and puts back the other's (or fits it, the first time).
    void switchCameras() {
        const DocId active = ctx_.documents().active();
        if (active == shownDoc_) return;
        if (ctx_.documents().find(shownDoc_)) {
            auto& kept = cameras_[shownDoc_];
            kept.clear();
            for (auto* pane : panes_) kept.push_back(pane->cameraPlace());
        }
        std::erase_if(cameras_, [this](const auto& c) { return !ctx_.documents().find(c.first); });
        const auto it = cameras_.find(active);
        for (std::size_t i = 0; i < panes_.size(); ++i) {
            if (it != cameras_.end() && i < it->second.size() && it->second[i].scale > 0) {
                panes_[i]->setCameraPlace(it->second[i]);
            } else if (!shownDoc_.isNil()) {
                panes_[i]->fitWhenShown();
            }
        }
        shownDoc_ = active;
    }

    QString flatText() const {
        QString text = problem_;
        const DocId id = ctx_.documents().active();
        const auto* doc = ctx_.documents().find(id);
        const auto* report = ctx_.documents().lastRebuild(id);
        if (!doc || doc->featureCount() == 0) {
            text += tr("\n\nStart with Create > Box or Create > Cylinder.");
        } else if (report) {
            QStringList names;
            for (const auto& body : report->bodies()) {
                if (const auto* f = doc->find(body)) names << QString::fromStdString(f->name());
            }
            text += tr("\n\nFinished bodies: %1")
                        .arg(names.isEmpty() ? tr("none yet") : names.join(QStringLiteral(", ")));
        }
        return text;
    }

    plugin::PluginContext& ctx_;
    ViewerService& service_;
    viewoptions::IViewOptions* options_ = nullptr;  // null if sb.viewoptions isn't loaded
    int number_ = 1;
    std::vector<QWidget*> bars_;  // one per pane: "This pane has its own view options"
    ViewBar* bar_ = nullptr;
    view::LeftDrag drag_ = view::LeftDrag::Orbit;
    view::DisplayStyle style_ = view::DisplayStyle::ShadedWithEdges;
    QWidget* body_ = nullptr;
    QSplitter* splitter_ = nullptr;
    view::FlatView* flat_ = nullptr;
    std::vector<QFrame*> frames_;
    std::vector<view::ViewWidget*> panes_;
    view::PaneLayout layout_ = view::PaneLayout::Single;
    int active_ = 0;
    view::SceneContent content_;
    DocId shownDoc_;                                           // the design the panes show
    std::map<DocId, std::vector<view::CameraPlace>> cameras_;  // other open designs' cameras, by pane
    QString problem_;
    bool refreshPending_ = false;
    std::vector<plugin::Subscription> subs_;
};

void ViewerService::startTool(viewer::IViewTool& tool, const viewer::ToolPlane& plane) {
    tool_ = &tool;
    plane_ = plane;
    applyAll();
}

void ViewerService::stopTool(const viewer::IViewTool& tool) {
    if (tool_ != &tool) return;
    tool_ = nullptr;
    applyAll();
}

void ViewerService::face(const viewer::ToolPlane& plane) {
    for (auto* panel : panels_) panel->face(plane);
}

void ViewerService::applyAll() {
    for (auto* panel : panels_) panel->applyTool();
}

void ViewerService::setHandles(viewer::IHandleUser& user, std::vector<viewer::Handle> handles) {
    handleUser_ = &user;
    given_ = handles;
    handles_.clear();
    for (const auto& h : handles) {
        handles_.push_back(
            {h.id,
             h.kind == viewer::HandleKind::Square ? view::HandleKind::Square : view::HandleKind::Arrow,
             h.at,
             h.direction,
             h.length,
             {h.r, h.g, h.b},
             true});
    }
    showHandlesAll();
}

void ViewerService::clearHandles(const viewer::IHandleUser& user) {
    if (handleUser_ != &user) return;
    handleUser_ = nullptr;
    handles_.clear();
    given_.clear();
    showHandlesAll();
}

void ViewerService::showHandlesAll() {
    for (auto* panel : panels_) panel->applyHandles();
}

void ViewerService::previewMove(const FeatureId& body, const kernel::Motion& motion) {
    preview_ = viewer::BodyPreview{body, motion};
    showPreviewAll();
}

void ViewerService::previewComponent(const InstanceId& component, const kernel::Motion& motion) {
    preview_ = viewer::BodyPreview{FeatureId{}, motion, component};
    showPreviewAll();
}

void ViewerService::clearPreview() {
    if (!preview_) return;
    preview_.reset();
    showPreviewAll();
}

void ViewerService::showPreviewAll() {
    for (auto* panel : panels_) panel->applyPreview();
}

void ViewerService::offerChoices(viewer::IChoiceUser& user, std::vector<viewer::Choice> choices) {
    choiceUser_ = &user;
    choices_ = std::move(choices);
    showChoicesAll();
}

void ViewerService::clearChoices(const viewer::IChoiceUser& user) {
    if (choiceUser_ != &user) return;
    choiceUser_ = nullptr;
    choices_.clear();
    showChoicesAll();
}

void ViewerService::showChoicesAll() {
    for (auto* panel : panels_) panel->applyChoices();
}

void ViewerService::askNumbers(viewer::INumberUser& user, std::vector<viewer::NumberBox> boxes,
                               const std::string& focus) {
    numberUser_ = &user;
    numbers_ = std::move(boxes);
    showNumbersAll(focus);
}

void ViewerService::clearNumbers(const viewer::INumberUser& user) {
    if (numberUser_ != &user) return;
    numberUser_ = nullptr;
    numbers_.clear();
    showNumbersAll({});
}

void ViewerService::showNumbersAll(const std::string& focus) {
    for (auto* panel : panels_) panel->applyNumbers(focus);
}

void registerViewer(plugin::PluginContext& ctx) {
    // The service lives as long as the plugin: a listener SketchBillet keeps while the
    // plugin is loaded holds it (and so does the panel factory below).
    auto service = std::make_shared<ViewerService>();
    ctx.provide<viewer::IViewer>(*service);
    ctx.listen<plugin::DocumentClosed>([service](const plugin::DocumentClosed&) {});
    auto* ui = ctx.service<ui::UiRegistry>();
    if (!ui) {
        ctx.log("No desktop app here, so the 3D viewer isn't shown.");
        return;
    }
    auto opened = std::make_shared<int>(0);  // viewers opened so far, to number them
    ui::PanelSpec spec{"sb.viewer.main", QObject::tr("Viewer"), ui::PanelArea::Center,
                       [&ctx, opened, service] { return new ViewerPanel(ctx, *service, ++*opened); }};
    spec.allowMultiple = true;
    ui->addPanel(std::move(spec));
}

}  // namespace

namespace {

void addAssemblyAround(const plugin::DocumentList& docs, const DocId& part, view::SceneContent& out) {
    const DocId assemblyId = docs.assemblyOf(part);
    const auto* assembly = docs.find(assemblyId);
    const auto* report = docs.lastRebuild(assemblyId);
    if (!assembly || !report) return;
    const auto edited = docs.editedComponent(assemblyId);
    const auto& components = assembly->components();
    const auto here = std::find_if(components.begin(), components.end(),
                                   [&](const model::Component& c) { return c.id == edited; });
    if (here == components.end()) return;
    // Where each component is: placed by its joints, or where it was put.
    auto placed = [report](const model::Component& c) {
        const auto* p = report->placementOf(c.id);
        return p ? *p : c.placement;
    };
    const kernel::Placement fromPart = kernel::inverse(placed(*here));
    for (const auto& c : components) {
        if (c.id == edited) continue;
        const auto* other = assembly->findPart(c.part);
        const auto* built = report->part(c.part);
        if (!other || !built) continue;
        const kernel::Placement around = kernel::compose(fromPart, placed(c));
        for (const auto& body : built->bodies()) {
            const auto* shape = built->shapeOf(body);
            if (!shape) continue;
            const auto color = other->colorOf(body);
            out.bodies.push_back({body, *shape, false, {color.r, color.g, color.b}, c.id, around, true});
        }
    }
}

}  // namespace

view::SceneContent viewerScene(const plugin::DocumentList& docs, const plugin::Selection& selection) {
    view::SceneContent out;
    const DocId id = docs.active();
    const auto* report = docs.lastRebuild(id);
    const auto* design = docs.find(id);
    if (!report || !design) return out;
    if (selection.doc() == id) out.selected = selection.features();
    if (design->kind() == model::DocKind::Assembly) {
        // Each component draws its part's bodies, placed. Components of the same
        // part share the part's shapes, so the view meshes each shape once.
        if (selection.doc() == id) out.selectedComponents = selection.components();
        for (const auto& c : design->components()) {
            const auto* part = design->findPart(c.part);
            const auto* built = report->part(c.part);
            if (!part || !built) continue;
            for (const auto& body : built->bodies()) {
                const auto* shape = built->shapeOf(body);
                if (!shape) continue;
                const auto color = part->colorOf(body);
                const auto* at = report->placementOf(c.id);  // placed by its joints, or where it was put
                out.bodies.push_back(
                    {body, *shape, false, {color.r, color.g, color.b}, c.id, at ? *at : c.placement});
            }
        }
        return out;
    }
    // Each body is drawn in its own color, or its material's (Document::colorOf()).
    auto colorOf = [design](const FeatureId& f) {
        const auto c = design->colorOf(f);
        return view::Rgb{c.r, c.g, c.b};
    };
    const auto& bodies = report->bodies();
    if (const auto& only = selection.pickOnly(); !only.isNil()) {
        // Picking again on one body, as it was before the steps that used it.
        if (const auto* shape = report->shapeOf(only))
            out.bodies.push_back({only, *shape, false, colorOf(only)});
        out.selected.clear();
    } else {
        for (const auto& body : bodies) {
            if (const auto* shape = report->shapeOf(body))
                out.bodies.push_back({body, *shape, false, colorOf(body)});
        }
    }
    // A selected step that a later step used up (the tool of a cut, say) is
    // drawn see-through, so the student can see what they picked.
    for (const auto& picked : out.selected) {
        if (std::find(bodies.begin(), bodies.end(), picked) != bodies.end()) continue;
        const auto* r = report->result(picked);
        if (r && r->status == model::FeatureStatus::Ok && r->shape.valid())
            out.bodies.push_back({picked, r->shape, true, colorOf(picked)});
    }
    // Editing a part in an assembly: the rest of the assembly is drawn around
    // it, faded, where it is from the part's point of view (the part stays at its
    // own origin, so its sketches and steps are where they always are).
    addAssemblyAround(docs, id, out);
    // Picking edges: the picked ones, found by name in this rebuild.
    out.pickEdges = selection.pickMode() == plugin::PickMode::Edges;
    out.pickFaces = selection.pickMode() == plugin::PickMode::Faces;
    out.pickMeasure = selection.pickMode() == plugin::PickMode::Measure;
    if (selection.doc() == id) {
        for (const auto& e : selection.elements()) {
            const auto* r = report->result(e.body);
            if (!r || !r->names) continue;
            const auto found = model::resolve(e.ref, *r->names);
            if (found.found()) out.picked.push_back({e.body, found.index, e.ref.kind});
        }
    }
    return out;
}

std::vector<plugin::PickedElement> pickedEdges(const plugin::DocumentList& docs,
                                               const std::vector<view::ScenePick>& picks) {
    std::vector<plugin::PickedElement> out;
    const auto* report = docs.lastRebuild(docs.active());
    if (!report) return out;
    for (const auto& p : picks) {
        const auto* r = report->result(p.body);
        if (!r || !r->names) continue;
        out.push_back({p.body, model::referTo(*r->names, p.kind, p.index)});
    }
    return out;
}

// Plugins draw just outside the cube with the plugin API's number, so it must be the view's.
static_assert(plugin::kViewCubeHalfSize == view::kViewCubeHalfSize);
static_assert(plugin::kViewCubeBevel == view::kViewCubeBevel);

std::vector<view::ViewOverlay> viewOverlays(const std::vector<plugin::Overlay>& overlays) {
    std::vector<view::ViewOverlay> out;
    for (const auto& o : overlays) {
        if (!o.visible) continue;
        view::ViewOverlay v;
        switch (o.shape) {
            case plugin::OverlayShape::Lines:
                v.shape = view::OverlayShape::Lines;
                break;
            case plugin::OverlayShape::Points:
                v.shape = view::OverlayShape::Points;
                break;
            case plugin::OverlayShape::Lattice:
                v.shape = view::OverlayShape::Lattice;
                break;
            case plugin::OverlayShape::Labels:
                v.shape = view::OverlayShape::Labels;
                break;
            case plugin::OverlayShape::Dimension:
                v.shape = view::OverlayShape::Dimension;
                break;
        }
        v.dimension = {o.dimension.along, o.dimension.normal, o.dimension.arrowAtFrom, o.dimension.arrowAtTo,
                       o.dimension.textAtEnd};
        v.points = o.points;
        v.texts = o.texts;
        v.origin = o.lattice.origin;
        v.xAxis = o.lattice.xAxis;
        v.yAxis = o.lattice.yAxis;
        v.spacing = o.lattice.spacing;
        v.halfSize = o.lattice.halfSize;
        v.dots = o.lattice.dots;
        v.color = {o.color.r, o.color.g, o.color.b};
        v.alpha = o.color.a;
        v.size = o.size;
        v.onViewCube = o.onViewCube;
        v.overBodies = o.overBodies;
        out.push_back(std::move(v));
    }
    return out;
}

plugin::BuiltinPlugin viewerPlugin() {
    return {kManifest, &registerViewer};
}

}  // namespace sb::plugins
