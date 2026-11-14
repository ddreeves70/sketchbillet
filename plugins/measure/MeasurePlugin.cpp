#include "sb/plugins/Measure.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QStringList>
#include <QVBoxLayout>

#include <memory>
#include <string>
#include <vector>

#include "MeasureIcons.h"
#include "sb/model/Document.h"
#include "sb/plugin/Events.h"
#include "sb/ui/UiRegistry.h"

namespace sb::plugins {

namespace {

constexpr const char* kManifest = R"({
  "id": "sb.measure",
  "name": "Measure",
  "version": "0.1.0",
  "description": "Tools > Measure: click a face, edge or point to see its size, or two to see the distance and angle between them.",
  "uses": ["sb.viewer"],
  "tier": "ui"
})";

constexpr const char* kPanelId = "sb.measure.panel";

// The line, its end dots and the distance: the sketch blue, so it stands out
// from the orange of the picked faces, edges and points.
constexpr plugin::OverlayColor kLineColor{30, 111, 224, 255};
// The distance's words: near black, to read over the line and the body.
constexpr plugin::OverlayColor kLabelColor{31, 35, 40, 255};

QString qs(const std::string& s) {
    return QString::fromStdString(s);
}

// What's picked to measure in the design the selection is in, measured; empty
// when not measuring or nothing's picked.
model::Measurement current(plugin::PluginContext& ctx) {
    auto& sel = ctx.selection();
    if (sel.pickMode() != plugin::PickMode::Measure || sel.elements().empty()) return {};
    const auto* doc = ctx.documents().find(sel.doc());
    const auto* report = ctx.documents().lastRebuild(sel.doc());
    const auto* kernel = ctx.documents().kernel();
    if (!doc || !report || !kernel) return {};
    std::vector<model::MeasurePick> picks;
    for (const auto& e : sel.elements()) picks.push_back({e.body, e.ref});
    return model::measure(*doc, *report, *kernel, picks);
}

LengthUnit unitOf(plugin::PluginContext& ctx) {
    const auto* doc = ctx.documents().find(ctx.selection().doc());
    return doc ? doc->lengthUnit() : LengthUnit::Millimeter;
}

// Draws the line between the nearest points in every viewer, or takes it away.
void drawLine(plugin::PluginContext& ctx) {
    const auto o = measureOverlays(current(ctx), unitOf(ctx));
    ctx.overlays().set("line", o.line);
    ctx.overlays().set("ends", o.ends);
    ctx.overlays().set("label", o.label);
}

// The Measure panel: what's measured, in words, and a Done button.
class MeasurePanel final : public QWidget {
public:
    explicit MeasurePanel(plugin::PluginContext& ctx) : ctx_(ctx) {
        setObjectName(QStringLiteral("sb.measure.widget"));
        setMinimumWidth(340);  // most lines fit on one line
        result_ = new QLabel(this);
        result_->setObjectName(QStringLiteral("sb.measure.result"));
        result_->setWordWrap(true);
        result_->setTextInteractionFlags(Qt::TextSelectableByMouse);  // to copy a number
        result_->setContentsMargins(4, 2, 4, 2);
        auto* done = new QPushButton(tr("Done"), this);
        done->setObjectName(QStringLiteral("sb.measure.done"));
        done->setToolTip(tr("Stop measuring: clicks pick bodies again."));
        connect(done, &QPushButton::clicked, this,
                [this] { ctx_.selection().setPickMode(plugin::PickMode::Bodies); });
        auto* buttons = new QHBoxLayout;
        buttons->addStretch();
        buttons->addWidget(done);
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(4, 4, 4, 4);
        layout->addWidget(result_);
        layout->addLayout(buttons);
        auto later = [this](const auto&) { fill(); };
        subs_.push_back(ctx_.events().subscribe<plugin::SelectionChanged>(later));
        subs_.push_back(ctx_.events().subscribe<plugin::PickModeChanged>(later));
        subs_.push_back(ctx_.events().subscribe<plugin::RebuildFinished>(later));
        subs_.push_back(ctx_.events().subscribe<plugin::ActiveDocumentChanged>(later));
        fill();
    }

private:
    void fill() {
        const auto m = current(ctx_);
        QStringList lines;
        for (const auto& line : model::describe(m, unitOf(ctx_))) lines << qs(line);
        for (const auto& issue : m.issues) lines << qs(issue.message);
        const auto picked =
            ctx_.selection().pickMode() == plugin::PickMode::Measure ? ctx_.selection().elements().size() : 0;
        if (picked == 0) {
            lines << tr("Click a face, edge or point in the view to measure it.");
        } else if (picked == 1) {
            lines << QString() << tr("Click another one to measure between them.");
        }
        result_->setText(lines.join(QLatin1Char('\n')));
        updateGeometry();  // floating over the view, the panel is as tall as its words
    }

    plugin::PluginContext& ctx_;
    QLabel* result_ = nullptr;
    std::vector<plugin::Subscription> subs_;
};

void registerMeasure(plugin::PluginContext& ctx) {
    auto* ui = ctx.service<ui::UiRegistry>();
    if (!ui) {
        ctx.log("No desktop app here, so the Measure tool isn't shown. Use sketchbillet_cli measure.");
        return;
    }
    ui::PanelSpec spec{kPanelId, QObject::tr("Measure"), ui::PanelArea::Right,
                       [&ctx] { return new MeasurePanel(ctx); }};
    spec.startHidden = true;     // Tools > Measure opens it
    spec.floatsOverView = true;  // over the view, under the view cube
    ui->addPanel(std::move(spec));

    auto& sel = ctx.selection();
    ctx.addAction(plugin::ActionSpec{
        .id = "sb.measure.measure",
        .text = "Measure",
        .menu = "Tools",
        .shortcut = "Ctrl+Shift+I",
        .toolbar = true,
        .icon = std::string(icons::measure::measure_svg),
        .tip = "Click a face, edge or point to see its size, or two to see the distance and angle between "
               "them. Click Measure again, or Done, to stop.",
        .run =
            [&sel] {
                sel.setPickMode(sel.pickMode() == plugin::PickMode::Measure ? plugin::PickMode::Bodies
                                                                            : plugin::PickMode::Measure);
            },
        .checked = [&sel] { return sel.pickMode() == plugin::PickMode::Measure; }});

    // The panel shows while measuring; the line follows what's picked.
    ctx.listen<plugin::PickModeChanged>([&ctx, ui](const plugin::PickModeChanged& e) {
        if (e.mode == plugin::PickMode::Measure) {
            ui->showPanel(kPanelId);
        } else {
            ui->hidePanel(kPanelId);
        }
        drawLine(ctx);
    });
    ctx.listen<plugin::SelectionChanged>([&ctx](const auto&) { drawLine(ctx); });
    ctx.listen<plugin::RebuildFinished>([&ctx](const auto&) { drawLine(ctx); });
    ctx.listen<plugin::ActiveDocumentChanged>([&ctx](const auto&) { drawLine(ctx); });
    drawLine(ctx);
}

}  // namespace

MeasureOverlays measureOverlays(const model::Measurement& m, LengthUnit unit) {
    MeasureOverlays out;
    out.line.shape = plugin::OverlayShape::Lines;
    out.line.color = kLineColor;
    out.line.size = 2;
    out.ends.shape = plugin::OverlayShape::Points;
    out.ends.color = kLineColor;
    out.ends.size = 7;
    out.label.shape = plugin::OverlayShape::Labels;
    out.label.color = kLabelColor;
    out.label.size = 16;
    const bool show = m.distance && *m.distance > 1e-9;
    for (auto* o : {&out.line, &out.ends, &out.label}) {
        o->visible = show;
        o->overBodies = true;  // the nearest points are on the body's faces, or inside it
    }
    if (!show) return out;
    out.line.points = {m.from, m.to};
    out.ends.points = {m.from, m.to};
    out.label.points = {{(m.from.x + m.to.x) / 2, (m.from.y + m.to.y) / 2, (m.from.z + m.to.z) / 2}};
    out.label.texts = {lengthText(*m.distance, unit)};
    return out;
}

plugin::BuiltinPlugin measurePlugin() {
    return {kManifest, &registerMeasure};
}

}  // namespace sb::plugins
