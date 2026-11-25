#include "sb/plugins/Measure.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QStringList>
#include <QVBoxLayout>

#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "MeasureIcons.h"
#include "sb/model/AssemblyCommands.h"
#include "sb/model/AssemblyMass.h"
#include "sb/model/Document.h"
#include "sb/model/Overlaps.h"
#include "sb/plugin/Events.h"
#include "sb/ui/UiRegistry.h"

namespace sb::plugins {

namespace {

constexpr const char* kManifest = R"({
  "id": "sb.measure",
  "name": "Measure",
  "version": "0.1.0",
  "description": "Inspect > Measure: click a face, edge or point to see its size, or two to see the distance and angle between them. In an assembly, Check for Overlaps shows where parts overlap, in red, and Balance Point shows its mass and where it balances.",
  "uses": ["sb.viewer"],
  "tier": "ui"
})";

constexpr const char* kPanelId = "sb.measure.panel";
constexpr const char* kChecksPanelId = "sb.measure.checks.panel";

// Where parts overlap: red, and see-through, so the parts show around it.
constexpr plugin::OverlayColor kOverlapColor{220, 30, 30, 170};
// The balance point: a dark dot on a white one, to see on any part.
constexpr plugin::OverlayColor kBalanceColor{31, 35, 40, 255};
constexpr plugin::OverlayColor kBalanceRing{255, 255, 255, 255};

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

// The assembly checks: which are on, and what they found in the active
// assembly. They're done again after each rebuild, while they're on.
struct Checks {
    bool overlaps = false;  // Check for Overlaps
    bool balance = false;   // Balance Point
    model::OverlapCheck found;
    model::AssemblyMass mass;
    LengthUnit unit = LengthUnit::Millimeter;
};

bool assemblyActive(plugin::PluginContext& ctx) {
    const auto* doc = ctx.documents().find(ctx.documents().active());
    return doc && doc->kind() == model::DocKind::Assembly;
}

// Does the checks that are on, and draws what they found.
void runChecks(plugin::PluginContext& ctx, Checks& checks) {
    checks.found = {};
    checks.mass = {};
    auto& docs = ctx.documents();
    const auto* doc = docs.find(docs.active());
    const auto* report = doc ? docs.lastRebuild(docs.active()) : nullptr;
    auto* kernel = docs.kernel();
    const bool can = doc && report && kernel && doc->kind() == model::DocKind::Assembly;
    if (can) checks.unit = doc->lengthUnit();
    if (can && checks.overlaps) checks.found = model::checkOverlaps(*doc, *report, *kernel);
    if (can && checks.balance) checks.mass = model::assemblyMass(*doc, *report, *kernel);
    ctx.overlays().set("overlaps", overlapOverlay(checks.found, kernel));
    const auto dots = balanceOverlays(checks.mass, can && checks.balance);
    ctx.overlays().set("balance.ring", dots.first);
    ctx.overlays().set("balance.dot", dots.second);
}

// The checks' panel: what each check that's on found, in words, and a Done button.
class ChecksPanel final : public QWidget {
public:
    ChecksPanel(plugin::PluginContext& ctx, Checks& checks, std::function<void()> done)
        : ctx_(ctx), checks_(checks) {
        setObjectName(QStringLiteral("sb.measure.checks.widget"));
        setMinimumWidth(340);
        result_ = new QLabel(this);
        result_->setObjectName(QStringLiteral("sb.measure.checks.result"));
        result_->setWordWrap(true);
        result_->setTextInteractionFlags(Qt::TextSelectableByMouse);
        result_->setContentsMargins(4, 2, 4, 2);
        auto* button = new QPushButton(tr("Done"), this);
        button->setObjectName(QStringLiteral("sb.measure.checks.done"));
        button->setToolTip(tr("Stop checking, and take the red and the dot away."));
        connect(button, &QPushButton::clicked, this, std::move(done));
        auto* buttons = new QHBoxLayout;
        buttons->addStretch();
        buttons->addWidget(button);
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(4, 4, 4, 4);
        layout->addWidget(result_);
        layout->addLayout(buttons);
        subs_.push_back(ctx_.events().subscribe<ChecksDone>([this](const auto&) { fill(); }));
        fill();
    }

    // Sent after the checks are done again, to show what they found.
    struct ChecksDone {
        static constexpr std::string_view kType = "sb.measure.ChecksDone";
    };

private:
    void fill() {
        QStringList lines;
        for (const auto& line :
             checksText(checks_.found, checks_.mass, checks_.overlaps, checks_.balance, checks_.unit))
            lines << qs(line);
        // "125.00 mm³": the unit cubed, as Properties writes it.
        result_->setText(
            lines.join(QLatin1Char('\n')).replace(QStringLiteral("^3"), QStringLiteral("\u00B3")));
        updateGeometry();
    }

    plugin::PluginContext& ctx_;
    Checks& checks_;
    QLabel* result_ = nullptr;
    std::vector<plugin::Subscription> subs_;
};

void registerChecks(plugin::PluginContext& ctx, ui::UiRegistry& ui) {
    auto checks = std::make_shared<Checks>();
    // Turns a check on or off, does them again, and shows or hides the panel.
    auto update = [&ctx, &ui, checks] {
        if (!assemblyActive(ctx)) checks->overlaps = checks->balance = false;
        runChecks(ctx, *checks);
        if (checks->overlaps || checks->balance) {
            ui.showPanel(kChecksPanelId);
        } else {
            ui.hidePanel(kChecksPanelId);
        }
        ctx.events().publish(ChecksPanel::ChecksDone{});
        ctx.publish(plugin::ActionsChanged{});  // their check marks
    };
    ui::PanelSpec spec{kChecksPanelId, QObject::tr("Checks"), ui::PanelArea::Right, [&ctx, checks, update] {
                           return new ChecksPanel(ctx, *checks, [checks, update] {
                               checks->overlaps = checks->balance = false;
                               update();
                           });
                       }};
    spec.startHidden = true;
    spec.floatsOverView = true;
    ui.addPanel(std::move(spec));

    const auto inAssembly = [&ctx] { return assemblyActive(ctx); };
    ctx.addAction(plugin::ActionSpec{
        .id = "sb.measure.overlaps",
        .text = "Check for Overlaps",
        .menu = "Assemble/Inspect",
        .toolbar = true,
        .buttonText = "Overlaps",
        .icon = std::string(icons::measure::overlaps_svg),
        .tip = "Shows in red where parts overlap, so they won't go together, and lists them. It checks again "
               "as you move parts. Click it again, or Done, to stop.",
        .run =
            [checks, update] {
                checks->overlaps = !checks->overlaps;
                update();
            },
        .checked = [checks] { return checks->overlaps; },
        .enabled = inAssembly});
    ctx.addAction(plugin::ActionSpec{
        .id = "sb.measure.balance",
        .text = "Balance Point",
        .menu = "Assemble/Inspect",
        .toolbar = true,
        .buttonText = "Balance",
        .icon = std::string(icons::measure::balance_svg),
        .tip = "Shows the assembly's mass and weight, and a dot where it balances: its center of gravity. "
               "Click it again, or Done, to stop.",
        .run =
            [checks, update] {
                checks->balance = !checks->balance;
                update();
            },
        .checked = [checks] { return checks->balance; },
        .enabled = inAssembly});
    // Press Fit: the two picked components are meant to overlap, or not.
    const auto picked = [&ctx]() -> std::optional<std::pair<model::ComponentId, model::ComponentId>> {
        if (!assemblyActive(ctx)) return std::nullopt;
        const auto& components = ctx.selection().components();
        if (components.size() != 2) return std::nullopt;
        return std::pair{components[0], components[1]};
    };
    ctx.addAction(plugin::ActionSpec{
        .id = "sb.measure.press_fit",
        .text = "Press Fit",
        .menu = "Assemble/Inspect",
        .toolbar = true,
        .buttonText = "Press fit",
        .icon = std::string(icons::measure::press_fit_svg),
        .tip =
            "Pick two components that are meant to overlap, like a pin pushed into a hole a bit smaller than "
            "it, and click this: Check for Overlaps leaves them out. Click it again to unmark them.",
        .run =
            [&ctx, picked] {
                const auto pair = picked();
                if (!pair) return;
                auto& docs = ctx.documents();
                const auto* doc = docs.find(docs.active());
                if (!doc) return;
                std::unique_ptr<model::Command> change;
                if (doc->findPressFit(pair->first, pair->second))
                    change = std::make_unique<model::DeletePressFitCommand>(pair->first, pair->second);
                else
                    change = std::make_unique<model::AddPressFitCommand>(
                        model::PressFit{{}, pair->first, pair->second});
                if (auto problems = docs.execute(docs.active(), std::move(change)); !problems.empty())
                    ctx.log(problems.front().message);
            },
        .checked =
            [&ctx, picked] {
                const auto pair = picked();
                const auto* doc = ctx.documents().find(ctx.documents().active());
                return pair && doc && doc->findPressFit(pair->first, pair->second) != nullptr;
            },
        .enabled = [picked] { return picked().has_value(); }});
    ctx.listen<plugin::RebuildFinished>([checks, update](const auto&) {
        if (checks->overlaps || checks->balance) update();
    });
    ctx.listen<plugin::ActiveDocumentChanged>([checks, update](const auto&) {
        if (checks->overlaps || checks->balance) update();
    });
}

void registerMeasure(plugin::PluginContext& ctx) {
    auto* ui = ctx.service<ui::UiRegistry>();
    if (!ui) {
        ctx.log("No desktop app here, so the Measure tool isn't shown. Use sketchbillet_cli measure.");
        return;
    }
    ui::PanelSpec spec{kPanelId, QObject::tr("Measure"), ui::PanelArea::Right,
                       [&ctx] { return new MeasurePanel(ctx); }};
    spec.startHidden = true;     // Inspect > Measure opens it
    spec.floatsOverView = true;  // over the view, under the view cube
    ui->addPanel(std::move(spec));

    auto& sel = ctx.selection();
    ctx.addAction(plugin::ActionSpec{
        .id = "sb.measure.measure",
        .text = "Measure",
        .menu = "Model/Inspect;Inspect/Measure",
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
    registerChecks(ctx, *ui);
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

plugin::Overlay overlapOverlay(const model::OverlapCheck& check, const kernel::IKernel* kernel) {
    plugin::Overlay out;
    out.shape = plugin::OverlayShape::Triangles;
    out.color = kOverlapColor;
    out.overBodies = true;  // it's inside the parts
    if (!kernel) return out;
    for (const auto& o : check.overlaps) {
        for (const auto& shape : o.shapes) {
            const auto mesh = kernel->tessellate(shape, {0.1, 0.35});
            for (const auto& t : mesh.triangles) {
                for (const auto i : t) {
                    const auto& v = mesh.vertices[i];
                    out.points.push_back({v[0], v[1], v[2]});
                }
            }
        }
    }
    out.visible = !out.points.empty();
    return out;
}

std::pair<plugin::Overlay, plugin::Overlay> balanceOverlays(const model::AssemblyMass& mass, bool show) {
    plugin::Overlay ring, dot;
    ring.shape = dot.shape = plugin::OverlayShape::Points;
    ring.color = kBalanceRing;
    ring.size = 16;
    dot.color = kBalanceColor;
    dot.size = 10;
    ring.overBodies = dot.overBodies = true;  // it's usually inside a part
    ring.visible = dot.visible = show && mass.grams > 0;
    if (ring.visible) ring.points = dot.points = {mass.center};
    return {ring, dot};
}

std::vector<std::string> checksText(const model::OverlapCheck& overlaps, const model::AssemblyMass& mass,
                                    bool overlapsOn, bool balanceOn, LengthUnit unit) {
    std::vector<std::string> out;
    if (overlapsOn) {
        for (const auto& issue : overlaps.issues) out.push_back(issue.message);
        if (!overlaps.overlaps.empty()) {
            out.push_back("Where they overlap is red in the view.");
            out.push_back(
                "If two are meant to overlap, like a pin pushed into a hole a bit smaller than it, "
                "pick both and click Press fit.");
        }
    }
    if (balanceOn) {
        if (!out.empty()) out.emplace_back();
        if (mass.grams <= 0) {
            out.push_back("Nothing to weigh yet: put in a part with a body.");
        } else {
            char grams[64];
            std::snprintf(grams, sizeof grams, "Mass: %.2f g, so it weighs %.3f N.", mass.grams,
                          mass.newtons);
            out.emplace_back(grams);
            auto n = [unit](double v) {
                const auto t = lengthText(v, unit);
                return t.substr(0, t.rfind(' '));
            };
            out.push_back("Balance point (the dot): x " + n(mass.center.x) + ", y " + n(mass.center.y) +
                          ", z " + n(mass.center.z) + " " + std::string(unitSymbol(unit)) +
                          ". It balances there, like on a pin.");
            if (mass.missing > 0)
                out.push_back(std::to_string(mass.missing) +
                              (mass.missing == 1 ? " component has" : " components have") +
                              " no body yet, so it's not counted.");
        }
    }
    return out;
}

plugin::BuiltinPlugin measurePlugin() {
    return {kManifest, &registerMeasure};
}

}  // namespace sb::plugins
