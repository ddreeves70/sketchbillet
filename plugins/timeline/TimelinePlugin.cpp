#include "sb/plugins/Timeline.h"

#include <QApplication>
#include <QContextMenuEvent>
#include <QHBoxLayout>
#include <QHelpEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScrollArea>
#include <QScrollBar>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QToolTip>

#include <algorithm>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "TimelineIcons.h"
#include "sb/model/Document.h"
#include "sb/model/Pattern.h"
#include "sb/plugin/Events.h"
#include "sb/ui/UiRegistry.h"

namespace sb::plugins {

namespace {

constexpr const char* kManifest = R"({
  "id": "sb.timeline",
  "name": "Timeline",
  "version": "0.1.0",
  "description": "The timeline bar: the design's steps in order, with the rollback marker.",
  "requires": ["sb.modeling"],
  "tier": "ui"
})";

constexpr const char* kPanelId = "sb.timeline.panel";

// The strip's measurements, in pixels.
constexpr int kChip = 34;           // a step's square
constexpr int kGap = 12;            // between steps: room for the marker
constexpr int kPad = 10;            // before the first step and after the last
constexpr int kHeight = 48;         // the strip's height
const QColor kMarker(35, 71, 168);  // SketchBillet's layout blue, as on sketchbillet.org

QString qs(const std::string& s) {
    return QString::fromStdString(s);
}

// A step's icon: its toolbar button's picture.
std::string_view iconOf(const model::Feature& f) {
    namespace i = sb::icons::timeline;
    switch (f.type()) {
        case model::FeatureType::Box:
            return i::box_svg;
        case model::FeatureType::Cylinder:
            return i::cylinder_svg;
        case model::FeatureType::Boolean:
            switch (static_cast<const model::BooleanFeature&>(f).op()) {
                case kernel::BoolOp::Union:
                    return i::union_svg;
                case kernel::BoolOp::Cut:
                    return i::cut_svg;
                case kernel::BoolOp::Intersect:
                    return i::intersect_svg;
            }
            return i::union_svg;
        case model::FeatureType::Move:
            return i::move_svg;
        case model::FeatureType::Sketch:
            return i::new_sketch_svg;
        case model::FeatureType::Extrude:
            return i::extrude_svg;
        case model::FeatureType::Fillet:
            return i::fillet_svg;
        case model::FeatureType::Chamfer:
            return i::chamfer_svg;
        case model::FeatureType::Revolve:
            return i::revolve_svg;
        case model::FeatureType::Hole:
            return i::hole_svg;
        case model::FeatureType::Pattern:
            return static_cast<const model::PatternFeature&>(f).layout().kind ==
                           model::PatternLayout::Kind::Circular
                       ? i::pattern_circular_svg
                       : i::pattern_linear_svg;
        case model::FeatureType::Mirror:
            return i::mirror_svg;
        case model::FeatureType::Plane:
            return i::new_plane_svg;
        case model::FeatureType::Shell:
            return i::shell_svg;
    }
    return {};
}

// What a step's status says, after its name in the tip.
QString statusText(model::FeatureStatus s) {
    switch (s) {
        case model::FeatureStatus::Ok:
            return QObject::tr("ok");
        case model::FeatureStatus::Failed:
            return QObject::tr("failed");
        case model::FeatureStatus::Blocked:
            return QObject::tr("blocked");
        case model::FeatureStatus::Suppressed:
            return QObject::tr("turned off");
        case model::FeatureStatus::RolledBack:
            return QObject::tr("rolled back: after the marker, so it isn't built");
    }
    return {};
}

// The strip: one square per step, the marker in a gap between them. Drawn here,
// not made of buttons, so steps and the marker can be dragged.
class Strip final : public QWidget {
public:
    struct Item {
        FeatureId id;
        QString name;
        QIcon icon;
        model::FeatureStatus status = model::FeatureStatus::Ok;
        QString tip;
        bool picked = false;
    };

    std::function<void(std::size_t)> moveMarker;                   // to a gap: steps before it are built
    std::function<void(std::size_t, std::size_t)> moveStep;        // step i to gap g
    std::function<void(std::size_t, bool add)> pick;               // a click on step i
    std::function<void(std::optional<std::size_t>, QPoint)> menu;  // right-click, on a step or not

    explicit Strip(QWidget* parent) : QWidget(parent) {
        setObjectName(QStringLiteral("sb.timeline.strip"));
        setMouseTracking(true);
        setFixedHeight(kHeight);
    }

    void show(std::vector<Item> items, std::size_t marker) {
        items_ = std::move(items);
        marker_ = std::min(marker, items_.size());
        setFixedWidth(kPad * 2 + static_cast<int>(items_.size()) * (kChip + kGap));
        // For tests and scripts: where each step is, and the marker.
        QVariantList rects;
        QStringList names, states, picked;
        for (std::size_t i = 0; i < items_.size(); ++i) {
            rects.push_back(chipRect(i));
            names.push_back(items_[i].name);
            states.push_back(statusText(items_[i].status).section(QLatin1Char(':'), 0, 0));
            if (items_[i].picked) picked.push_back(items_[i].name);
        }
        setProperty("chipRects", rects);
        setProperty("steps", names);
        setProperty("states", states);
        setProperty("picked", picked);
        setProperty("marker", static_cast<int>(marker_));
        update();
    }

    QRect chipRect(std::size_t i) const {
        return {kPad + kGap / 2 + static_cast<int>(i) * (kChip + kGap), (kHeight - kChip) / 2, kChip, kChip};
    }

    // The middle of the gap with this many steps before it.
    int gapX(std::size_t gap) const { return kPad + static_cast<int>(gap) * (kChip + kGap); }

    // The gap nearest a point.
    std::size_t gapAt(int x) const {
        const double g = double(x - kPad) / (kChip + kGap);
        return static_cast<std::size_t>(
            std::clamp<long>(std::lround(g), 0, static_cast<long>(items_.size())));
    }

    std::optional<std::size_t> chipAt(QPoint p) const {
        for (std::size_t i = 0; i < items_.size(); ++i)
            if (chipRect(i).adjusted(-2, -6, 2, 6).contains(p)) return i;
        return std::nullopt;
    }

    int markerX() const { return gapX(marker_); }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QPalette& pal = palette();
        // A line along the middle, like a film strip's track.
        p.setPen(QPen(pal.color(QPalette::Mid), 2));
        if (!items_.empty()) p.drawLine(gapX(0), kHeight / 2, gapX(items_.size()), kHeight / 2);
        for (std::size_t i = 0; i < items_.size(); ++i) {
            const Item& it = items_[i];
            const QRect r = chipRect(i);
            const bool after = i >= marker_;
            p.setOpacity(after ? 0.35 : 1.0);
            QColor fill =
                it.picked ? pal.color(QPalette::Highlight).lighter(170) : pal.color(QPalette::Button);
            QColor edge = it.picked ? pal.color(QPalette::Highlight) : pal.color(QPalette::Mid);
            if (it.status == model::FeatureStatus::Failed || it.status == model::FeatureStatus::Blocked)
                edge = QColor(196, 43, 28);
            p.setPen(QPen(edge, it.picked ? 2 : 1));
            p.setBrush(fill);
            p.drawRoundedRect(QRectF(r).adjusted(0.5, 0.5, -0.5, -0.5), 6, 6);
            const bool off = it.status == model::FeatureStatus::Suppressed;
            it.icon.paint(&p, r.adjusted(6, 6, -6, -6), Qt::AlignCenter,
                          off ? QIcon::Disabled : QIcon::Normal);
            if (off) {  // struck through, like the feature tree's grey
                p.setPen(QPen(pal.color(QPalette::Mid), 2));
                p.drawLine(r.bottomLeft() + QPoint(6, -6), r.topRight() + QPoint(-6, 6));
            }
            p.setOpacity(1.0);
        }
        // The marker, or where it'll go while it's dragged.
        const int x = dragMarker_ ? gapX(*dragMarker_) : markerX();
        p.setPen(Qt::NoPen);
        p.setBrush(kMarker);
        p.drawRect(QRect(x - 2, 4, 4, kHeight - 8));
        QPainterPath handle;  // a small flag on top, to grab
        handle.addRoundedRect(QRectF(x - 6, 1, 12, 9), 2, 2);
        p.drawPath(handle);
        // Where a dragged step will go.
        if (dragStep_ && dropGap_) {
            p.setBrush(pal.color(QPalette::Highlight));
            p.drawRect(QRect(gapX(*dropGap_) - 1, 2, 3, kHeight - 4));
        }
    }

    void mousePressEvent(QMouseEvent* e) override {
        if (e->button() != Qt::LeftButton) return;
        const QPoint p = e->position().toPoint();
        pressAt_ = p;
        if (std::abs(p.x() - markerX()) <= 6) {
            dragMarker_ = marker_;
            return;
        }
        pressed_ = chipAt(p);
    }

    void mouseMoveEvent(QMouseEvent* e) override {
        const QPoint p = e->position().toPoint();
        if (dragMarker_) {
            dragMarker_ = gapAt(p.x());
            update();
            return;
        }
        if (pressed_ && (e->buttons() & Qt::LeftButton) &&
            (p - pressAt_).manhattanLength() >= QApplication::startDragDistance()) {
            dragStep_ = pressed_;
            dropGap_ = gapAt(p.x());
            setCursor(Qt::ClosedHandCursor);
            update();
            return;
        }
        // Over the marker, the cursor says it can be dragged.
        setCursor(std::abs(p.x() - markerX()) <= 6 ? Qt::SizeHorCursor : Qt::ArrowCursor);
    }

    void mouseReleaseEvent(QMouseEvent* e) override {
        if (e->button() != Qt::LeftButton) return;
        if (dragMarker_) {
            const std::size_t to = *dragMarker_;
            dragMarker_.reset();
            update();
            if (to != marker_ && moveMarker) moveMarker(to);
            return;
        }
        if (dragStep_) {
            const std::size_t step = *dragStep_, gap = dropGap_.value_or(step);
            dragStep_.reset();
            dropGap_.reset();
            pressed_.reset();
            unsetCursor();
            update();
            // Dropped next to itself: it stays where it is.
            if (gap != step && gap != step + 1 && moveStep) moveStep(step, gap);
            return;
        }
        if (pressed_ && pick) pick(*pressed_, e->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier));
        pressed_.reset();
    }

    void contextMenuEvent(QContextMenuEvent* e) override {
        if (menu) menu(chipAt(e->pos()), e->globalPos());
    }

    bool event(QEvent* e) override {
        if (e->type() == QEvent::ToolTip) {
            auto* help = static_cast<QHelpEvent*>(e);
            if (const auto i = chipAt(help->pos())) {
                QToolTip::showText(help->globalPos(), items_[*i].tip, this, chipRect(*i));
            } else if (std::abs(help->pos().x() - markerX()) <= 6) {
                QToolTip::showText(help->globalPos(),
                                   tr("The rollback marker. Drag it back to see the part as it was, and to "
                                      "put new steps in there. Steps after it aren't built."),
                                   this);
            } else {
                QToolTip::hideText();
            }
            return true;
        }
        return QWidget::event(e);
    }

private:
    std::vector<Item> items_;
    std::size_t marker_ = 0;
    QPoint pressAt_;
    std::optional<std::size_t> pressed_, dragStep_, dropGap_, dragMarker_;
};

// The bar: four buttons to step the marker, then the strip.
class TimelineBar final : public QWidget {
public:
    TimelineBar(plugin::PluginContext& ctx, ui::UiRegistry& ui) : ctx_(ctx), ui_(ui) {
        setObjectName(QStringLiteral("sb.timeline.widget"));
        auto* row = new QHBoxLayout(this);
        row->setContentsMargins(4, 2, 4, 2);
        row->setSpacing(2);
        const auto button = [&](const char* name, QStyle::StandardPixmap icon, const QString& tip) {
            auto* b = new QToolButton(this);
            b->setObjectName(QString::fromLatin1(name));
            b->setIcon(style()->standardIcon(icon));
            b->setToolTip(tip);
            b->setAutoRaise(true);
            row->addWidget(b);
            return b;
        };
        start_ = button("sb.timeline.start", QStyle::SP_MediaSkipBackward,
                        tr("Roll back to the start, before the first step"));
        back_ = button("sb.timeline.back", QStyle::SP_MediaSeekBackward, tr("Roll back one step"));
        forward_ = button("sb.timeline.forward", QStyle::SP_MediaSeekForward, tr("Roll forward one step"));
        end_ = button("sb.timeline.end", QStyle::SP_MediaSkipForward,
                      tr("Roll forward to the end: every step built"));
        connect(start_, &QToolButton::clicked, this, [this] { rollTo(0); });
        connect(back_, &QToolButton::clicked, this, [this] { rollTo(markerIndex() - 1); });
        connect(forward_, &QToolButton::clicked, this, [this] { rollTo(markerIndex() + 1); });
        connect(end_, &QToolButton::clicked, this, [this] { rollTo(stepCount()); });

        strip_ = new Strip(this);
        scroll_ = new QScrollArea(this);
        scroll_->setObjectName(QStringLiteral("sb.timeline.scroll"));
        scroll_->setWidget(strip_);
        scroll_->setFrameShape(QFrame::NoFrame);
        scroll_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scroll_->setFixedHeight(kHeight + scroll_->horizontalScrollBar()->sizeHint().height());
        row->addWidget(scroll_, 1);

        strip_->moveMarker = [this](std::size_t gap) { rollTo(gap); };
        strip_->moveStep = [this](std::size_t step, std::size_t gap) { moveTo(step, gap); };
        strip_->pick = [this](std::size_t i, bool add) { pickStep(i, add); };
        strip_->menu = [this](std::optional<std::size_t> i, QPoint at) { showMenu(i, at); };

        using namespace plugin;
        auto later = [this](const auto&) { scheduleFill(); };
        subs_.push_back(ctx_.events().subscribe<RebuildFinished>(later));
        subs_.push_back(ctx_.events().subscribe<DocumentChanged>(later));
        subs_.push_back(ctx_.events().subscribe<ActiveDocumentChanged>(later));
        subs_.push_back(ctx_.events().subscribe<DocumentClosed>(later));
        subs_.push_back(ctx_.events().subscribe<SelectionChanged>(later));
        fill();
    }

private:
    const model::Document* design() const { return ctx_.documents().find(ctx_.documents().active()); }
    std::size_t stepCount() const { return design() ? design()->featureCount() : 0; }
    std::size_t markerIndex() const { return design() ? design()->markerIndex() : 0; }

    void scheduleFill() {
        if (fillPending_) return;
        fillPending_ = true;
        QTimer::singleShot(0, this, [this] {
            fillPending_ = false;
            fill();
        });
    }

    void fill() {
        const auto* d = design();
        std::vector<Strip::Item> items;
        if (d) {
            const auto* report = ctx_.documents().lastRebuild(d->id());
            const auto& picked = ctx_.selection().features();
            for (std::size_t i = 0; i < d->featureCount(); ++i) {
                const auto& f = d->featureAt(i);
                Strip::Item it;
                it.id = f.id();
                it.name = qs(f.name());
                it.icon = ui_.icon(iconOf(f));
                const auto* r = report ? report->result(f.id()) : nullptr;
                // Before the first rebuild after a change, what the design says.
                it.status = r ? r->status
                              : (d->afterMarker(f.id()) ? model::FeatureStatus::RolledBack
                                                        : (f.suppressed() ? model::FeatureStatus::Suppressed
                                                                          : model::FeatureStatus::Ok));
                it.tip = QStringLiteral("%1: %2").arg(it.name, statusText(it.status));
                if (r)
                    for (const auto& issue : r->issues) it.tip += QLatin1Char('\n') + qs(issue.message);
                it.picked = std::find(picked.begin(), picked.end(), f.id()) != picked.end();
                items.push_back(std::move(it));
            }
        }
        strip_->show(std::move(items), markerIndex());
        const std::size_t n = stepCount(), m = markerIndex();
        start_->setEnabled(m > 0);
        back_->setEnabled(m > 0);
        forward_->setEnabled(m < n);
        end_->setEnabled(m < n);
        // Keep the marker in sight.
        scroll_->ensureVisible(strip_->markerX(), kHeight / 2, 60, 0);
    }

    void run(std::string_view command, const model::Args& args) {
        const auto problems = ctx_.documents().run(ctx_.documents().active(), command, args);
        if (!problems.empty()) ui_.showProblems(problems);
    }

    void rollTo(std::size_t gap) {
        const auto* d = design();
        if (!d || gap > d->featureCount() || gap == d->markerIndex()) return;
        if (gap == 0) {
            run("timeline.rollback", {{"to", std::string("start")}});
        } else if (gap == d->featureCount()) {
            run("timeline.rollback", {{"to", std::string("end")}});
        } else {
            run("timeline.rollback", {{"after", d->featureAt(gap - 1).id()}});
        }
    }

    void moveTo(std::size_t step, std::size_t gap) {
        const auto* d = design();
        if (!d || step >= d->featureCount()) return;
        const FeatureId id = d->featureAt(step).id();
        if (gap >= d->featureCount()) {
            run("timeline.move", {{"feature", id}, {"to", std::string("end")}});
        } else {
            run("timeline.move", {{"feature", id}, {"before", d->featureAt(gap).id()}});
        }
    }

    void pickStep(std::size_t i, bool add) {
        const auto* d = design();
        if (!d || i >= d->featureCount()) return;
        const FeatureId id = d->featureAt(i).id();
        std::vector<FeatureId> picked;
        if (add) {
            picked = ctx_.selection().features();
            const auto at = std::find(picked.begin(), picked.end(), id);
            if (at != picked.end()) {
                picked.erase(at);
            } else {
                picked.push_back(id);
            }
        } else {
            picked = {id};
        }
        ctx_.selection().set(d->id(), std::move(picked));
    }

    void showMenu(std::optional<std::size_t> i, QPoint at) {
        const auto* d = design();
        if (!d) return;
        QMenu menu(this);
        menu.setObjectName(QStringLiteral("sb.timeline.menu"));
        if (i && *i < d->featureCount()) {
            const auto& f = d->featureAt(*i);
            const FeatureId id = f.id();
            const std::size_t gap = *i + 1;
            auto* here = menu.addAction(tr("Roll back to here"), this, [this, gap] { rollTo(gap); });
            here->setEnabled(gap != d->markerIndex());
            const bool off = f.suppressed();
            menu.addAction(off ? tr("Turn on") : tr("Turn off"), this, [this, id, off] {
                run("feature.suppress", {{"steps", std::vector<FeatureId>{id}}, {"off", !off}});
            });
            menu.addAction(tr("Delete"), this, [this, id] { run("feature.delete", {{"feature", id}}); });
        }
        auto* end = menu.addAction(tr("Roll forward to the end"), this, [this] { rollTo(stepCount()); });
        end->setEnabled(d->rolledBack());
        menu.exec(at);
    }

    plugin::PluginContext& ctx_;
    ui::UiRegistry& ui_;
    QToolButton *start_ = nullptr, *back_ = nullptr, *forward_ = nullptr, *end_ = nullptr;
    QScrollArea* scroll_ = nullptr;
    Strip* strip_ = nullptr;
    bool fillPending_ = false;
    std::vector<plugin::Subscription> subs_;
};

void registerTimeline(plugin::PluginContext& ctx) {
    auto* ui = ctx.service<ui::UiRegistry>();
    if (!ui) {
        ctx.log("No desktop app here, so the timeline bar isn't shown.");
        return;
    }
    ui::PanelSpec spec{kPanelId, QObject::tr("Timeline"), ui::PanelArea::Bottom,
                       [&ctx, ui] { return new TimelineBar(ctx, *ui); }};
    ui->addPanel(std::move(spec));
}

}  // namespace

plugin::BuiltinPlugin timelinePlugin() {
    return {kManifest, &registerTimeline};
}

}  // namespace sb::plugins
