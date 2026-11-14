#include "sb/plugins/Properties.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QEvent>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <memory>
#include <numbers>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

#include "sb/model/Commands.h"
#include "sb/model/ConstructionPlane.h"
#include "sb/model/Document.h"
#include "sb/model/Expression.h"
#include "sb/model/Formulas.h"
#include "sb/model/Holes.h"
#include "sb/model/Import.h"
#include "sb/model/Material.h"
#include "sb/model/Mirror.h"
#include "sb/model/Pattern.h"
#include "sb/model/Shell.h"
#include "sb/plugin/Events.h"
#include "sb/ui/FormulaBox.h"
#include "sb/ui/UiRegistry.h"

namespace sb::plugins {

namespace {

constexpr const char* kManifest = R"({
  "id": "sb.properties",
  "name": "Properties",
  "version": "0.1.0",
  "description": "The panel for changing the selected step's name and numbers.",
  "requires": ["sb.modeling"],
  "tier": "ui"
})";

constexpr qint64 kMergeMs = 800;                 // edits to one number this close together are one undo step
constexpr const char* kNewMaterial = "\x01new";  // the combo item that opens the New material dialog

QString qs(const std::string& s) {
    return QString::fromStdString(s);
}

// "0.18", "12.50", "1,234.00": money with two decimals.
QString money(double v) {
    return QStringLiteral("$") + QString::number(v, 'f', 2);
}

// Grams below 1 kg, kilograms above.
QString massText(double grams) {
    return grams < 1000 ? QObject::tr("%1 g").arg(grams, 0, 'f', 2)
                        : QObject::tr("%1 kg").arg(grams / 1000, 0, 'f', 3);
}

// Asks for a new material's numbers. Returns false if cancelled.
bool askNewMaterial(QWidget* parent, model::Material& m) {
    QDialog dialog(parent);
    dialog.setWindowTitle(QObject::tr("New material"));
    auto* form = new QFormLayout(&dialog);
    auto* name = new QLineEdit(&dialog);
    name->setPlaceholderText(QObject::tr("For example: Blue PETG"));
    auto number = [&dialog](double max, int decimals, const QString& suffix) {
        auto* s = new QDoubleSpinBox(&dialog);
        s->setRange(0, max);
        s->setDecimals(decimals);
        s->setSuffix(suffix);
        s->setSpecialValueText(QObject::tr("unknown"));  // shown for 0
        return s;
    };
    auto* density = number(25, 3, QObject::tr(" g/cm³"));
    density->setValue(1.24);
    auto* youngs = number(1000, 2, QObject::tr(" GPa"));
    auto* poisson = number(0.5, 3, QString());
    auto* yield = number(5000, 0, QObject::tr(" MPa"));
    auto* cost = number(100000, 2, QObject::tr(" per kg"));
    cost->setPrefix(QStringLiteral("$"));
    QColor color(120, 170, 220);
    auto* colorButton = new QPushButton(&dialog);
    auto showColor = [colorButton, &color] {
        colorButton->setText(color.name());
        colorButton->setStyleSheet(QStringLiteral("background: %1").arg(color.name()));
    };
    showColor();
    QObject::connect(colorButton, &QPushButton::clicked, &dialog, [&] {
        const QColor picked = QColorDialog::getColor(color, &dialog, QObject::tr("Material color"));
        if (picked.isValid()) color = picked;
        showColor();
    });
    form->addRow(QObject::tr("Name"), name);
    form->addRow(QObject::tr("Density"), density);
    form->addRow(QObject::tr("Color"), colorButton);
    form->addRow(QObject::tr("Stiffness (Young's modulus)"), youngs);
    form->addRow(QObject::tr("Poisson's ratio"), poisson);
    form->addRow(QObject::tr("Yield strength"), yield);
    form->addRow(QObject::tr("Price"), cost);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    form->addRow(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return false;
    m.name = name->text().trimmed().toStdString();
    m.density = density->value();
    m.color = {static_cast<std::uint8_t>(color.red()), static_cast<std::uint8_t>(color.green()),
               static_cast<std::uint8_t>(color.blue())};
    m.youngsModulus = youngs->value();
    m.poissonsRatio = poisson->value();
    m.yieldStrength = yield->value();
    m.costPerKg = cost->value();
    return true;
}

// A copy of the step, changed, ready for EditFeatureCommand.
using Change = std::function<void(model::Feature&)>;

class Properties final : public QWidget {
public:
    Properties(plugin::PluginContext& ctx, ui::UiRegistry& ui) : ctx_(ctx), ui_(ui) {
        setObjectName(QStringLiteral("sb.properties.widget"));
        setMinimumWidth(340);
        auto* outer = new QVBoxLayout(this);
        title_ = new QLabel(this);
        title_->setObjectName(QStringLiteral("sb.properties.title"));
        title_->setWordWrap(true);
        QFont bold = title_->font();
        bold.setBold(true);
        title_->setFont(bold);
        outer->addWidget(title_);
        form_ = new QWidget(this);
        outer->addWidget(form_);
        // While picking a Fillet's or Chamfer's edges again: Done and Cancel.
        changing_ = new QWidget(this);
        changing_->setObjectName(QStringLiteral("sb.properties.changing"));
        auto* buttons = new QHBoxLayout(changing_);
        buttons->setContentsMargins(0, 0, 0, 0);
        done_ = new QPushButton(tr("Done"), changing_);
        done_->setObjectName(QStringLiteral("edgesdone"));
        auto* cancel = new QPushButton(tr("Cancel"), changing_);
        cancel->setObjectName(QStringLiteral("edgescancel"));
        buttons->addWidget(done_);
        buttons->addWidget(cancel);
        buttons->addStretch(1);
        connect(done_, &QPushButton::clicked, this, [this] { finishChanging(true); });
        connect(cancel, &QPushButton::clicked, this, [this] { finishChanging(false); });
        changing_->hide();
        outer->addWidget(changing_);
        status_ = new QLabel(this);
        status_->setObjectName(QStringLiteral("sb.properties.status"));
        status_->setWordWrap(true);
        status_->setStyleSheet(QStringLiteral("color: #c42b1c"));
        outer->addWidget(status_);
        outer->addStretch(1);

        using namespace plugin;
        subs_.push_back(ctx_.events().subscribe<SelectionChanged>([this](const auto&) { schedule(); }));
        subs_.push_back(ctx_.events().subscribe<ActiveDocumentChanged>([this](const auto&) { schedule(); }));
        subs_.push_back(ctx_.events().subscribe<DocumentChanged>([this](const auto&) { schedule(); }));
        subs_.push_back(ctx_.events().subscribe<RebuildFinished>([this](const auto&) { schedule(); }));
        subs_.push_back(ctx_.events().subscribe<PickModeChanged>([this](const auto& e) {
            // Leaving edge picking some other way stops changing edges.
            if (e.mode != plugin::PickMode::Edges) changingStep_ = FeatureId{};
            schedule();
        }));
        refresh();
    }

    // Starts picking a Fillet's or Chamfer's edges again: the view shows only its
    // body (as it was before the step) and picks edges, until Done or Cancel.
    void startChanging(const FeatureId& step) {
        const auto* design = ctx_.documents().find(ctx_.selection().doc());
        const auto* e = design ? dynamic_cast<const model::EdgeFeature*>(design->find(step)) : nullptr;
        if (!e) return;
        changingDoc_ = ctx_.selection().doc();
        auto& sel = ctx_.selection();
        sel.setPickMode(plugin::PickMode::Edges);  // clears the selection's edges, if any
        changingStep_ = step;
        sel.setPickOnly(e->body());
        ui_.showMessage(tr("Click the edges for %1, then click Done in Properties. Ctrl-click picks more.")
                            .arg(QString::fromStdString(e->name())));
        schedule();
    }

    void finishChanging(bool keep) {
        const FeatureId step = changingStep_;
        const DocId doc = changingDoc_;
        if (step.isNil()) return;
        std::vector<model::GeometryRef> edges;
        for (const auto& p : ctx_.selection().elements()) edges.push_back(p.ref);
        changingStep_ = FeatureId{};
        ctx_.selection().setPickMode(plugin::PickMode::Bodies);
        if (keep && !edges.empty()) {
            const auto problems =
                ctx_.documents().run(doc, "feature.set_edges", {{"feature", step}, {"edges", edges}});
            if (!problems.empty()) ui_.showProblems(problems);
        }
        ctx_.selection().set(doc, {step});
    }

public:
    // Modify > Change edges: for the picked Fillet or Chamfer.
    bool canChange() const {
        const auto* f = selected();
        return f && (f->type() == model::FeatureType::Fillet || f->type() == model::FeatureType::Chamfer);
    }
    void changePicked() {
        if (const auto* f = selected(); f && canChange()) startChanging(f->id());
    }

private:
    const model::Feature* selected() const {
        const auto& sel = ctx_.selection();
        if (sel.features().size() != 1) return nullptr;
        const auto* design = ctx_.documents().find(sel.doc());
        return design ? design->find(sel.features()[0]) : nullptr;
    }

    void schedule() {
        if (pending_) return;
        pending_ = true;
        QMetaObject::invokeMethod(this, [this] { refresh(); }, Qt::QueuedConnection);
    }

    // Shows the selected step. Rebuilds the form only when a different step is
    // picked, so the box being typed in keeps its focus.
    void refresh() {
        pending_ = false;
        const auto* f = selected();
        const auto count = ctx_.selection().features().size();
        if (ctx_.selection().designPicked()) {
            showDesign();
            return;
        }
        changing_->setVisible(!changingStep_.isNil());
        if (!changingStep_.isNil()) {
            // Picking a Fillet's or Chamfer's edges again.
            shown_ = FeatureId{};
            clearForm();
            const auto* design = ctx_.documents().find(changingDoc_);
            const auto* step = design ? design->find(changingStep_) : nullptr;
            const auto n = static_cast<int>(ctx_.selection().elements().size());
            const QString name = step ? QString::fromStdString(step->name()) : QString();
            title_->setText(tr("Changing the edges of %1").arg(name));
            status_->setStyleSheet(QStringLiteral("color: #57606a"));
            status_->setText(n == 0   ? tr("Click the edges to use.")
                             : n == 1 ? tr("1 edge picked.")
                                      : tr("%1 edges picked.").arg(n));
            done_->setEnabled(n > 0);
            return;
        }
        status_->setStyleSheet(QStringLiteral("color: #c42b1c"));
        if (!f) {
            shown_ = FeatureId{};
            clearForm();
            const auto& edges = ctx_.selection().elements();
            const auto* design = ctx_.documents().find(ctx_.selection().doc());
            const auto* body = design && !edges.empty() ? design->find(edges.front().body) : nullptr;
            if (body && edges.front().ref.kind == kernel::TopoKind::Face) {
                // A face picked: say which, and what it's for.
                title_->setText(tr("Face picked: %1")
                                    .arg(QString::fromStdString(model::describeElement(
                                        *design, kernel::TopoKind::Face, edges.front().ref.name))));
                status_->setText(
                    edges.front().ref.signature.geom == kernel::GeomKind::Plane
                        ? QString()
                        : tr("This face is curved, so it can't hold a sketch. Pick a flat face."));
                return;
            }
            if (body) {
                // Edges picked: say how many, and what to do with them.
                const bool oneBody =
                    std::all_of(edges.begin(), edges.end(),
                                [&](const plugin::PickedElement& e) { return e.body == edges.front().body; });
                const auto n = static_cast<int>(edges.size());
                const QString name = QString::fromStdString(body->name());
                if (oneBody) {
                    title_->setText(n == 1 ? tr("1 edge of %1 picked").arg(name)
                                           : tr("%1 edges of %2 picked").arg(n).arg(name));
                    status_->clear();  // the status bar says what to click next
                } else {
                    title_->setText(tr("%1 edges picked").arg(n));
                    status_->setText(
                        tr("Fillet and Chamfer change one body at a time: pick edges of just one body."));
                }
                return;
            }
            title_->setText(count > 1 ? tr("%1 steps selected").arg(count)
                                      : tr("Pick a step in the feature tree to see its numbers."));
            status_->clear();
            return;
        }
        if (f->id() != shown_ || unitNow() != unit_) {
            shown_ = f->id();
            unit_ = unitNow();  // the numbers are shown in the design's unit
            buildForm(*f);
        }
        filling_ = true;
        for (auto& u : updaters_) u(*f);
        showFormulas(*f);
        filling_ = false;
        title_->setText(QString::fromStdString(f->name()));

        QString problems;
        if (const auto* report = ctx_.documents().lastRebuild(ctx_.selection().doc())) {
            if (const auto* r = report->result(f->id())) {
                for (const auto& d : r->issues)
                    problems += QString::fromStdString(d.message) + QLatin1Char('\n');
            }
        }
        status_->setText(problems.trimmed());
    }

    // The design's own settings (the feature tree's Document settings row).
    void showDesign() {
        const auto* design = ctx_.documents().find(ctx_.selection().doc());
        shown_ = FeatureId{};
        clearForm();
        status_->clear();
        title_->setText(tr("Document settings"));
        if (!design) return;
        auto* form = new QFormLayout(form_);
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        auto* name = new QLabel(QString::fromStdString(design->name()), form_);
        name->setObjectName(QStringLiteral("design"));
        form->addRow(tr("Design"), name);
        const auto count = static_cast<int>(design->featureCount());
        auto* steps = new QLabel(count == 1 ? tr("1 step") : tr("%1 steps").arg(count), form_);
        steps->setObjectName(QStringLiteral("steps"));
        form->addRow(tr("Steps"), steps);
        auto* units = new QComboBox(form_);
        units->setObjectName(QStringLiteral("units"));
        for (auto u : allLengthUnits()) {
            units->addItem(tr("%1 (%2)").arg(qs(std::string(unitName(u))), qs(std::string(unitSymbol(u)))),
                           qs(std::string(unitSymbol(u))));
        }
        units->setCurrentIndex(units->findData(qs(std::string(unitSymbol(design->lengthUnit())))));
        units->setToolTip(
            tr("The unit for every length you see and type in this design. Changing it "
               "doesn't change anything's size."));
        form->addRow(tr("Lengths"), units);
        connect(units, &QComboBox::activated, this, [this, units](int index) {
            const auto symbol = units->itemData(index).toString().toStdString();
            const auto doc = ctx_.selection().doc();
            const auto* d = ctx_.documents().find(doc);
            if (!d || symbol == unitSymbol(d->lengthUnit())) return;
            auto problems = ctx_.documents().run(doc, "document.set_units", {{"unit", symbol}});
            if (!problems.empty()) ui_.showProblems(problems);
        });
        auto* angles = new QLabel(tr("Degrees"), form_);
        angles->setObjectName(QStringLiteral("angles"));
        form->addRow(tr("Angles"), angles);
    }

    bool eventFilter(QObject* watched, QEvent* event) override {
        if (watched == wrapped_ && event->type() == QEvent::Resize) {
            const int needs = wrapped_->heightForWidth(wrapped_->width());
            if (needs > 0 && needs != wrapped_->minimumHeight()) wrapped_->setMinimumHeight(needs);
        }
        return QWidget::eventFilter(watched, event);
    }

    void clearForm() {
        wrapped_ = nullptr;
        updaters_.clear();
        delete form_->layout();
        for (auto* child : form_->findChildren<QWidget*>(Qt::FindDirectChildrenOnly)) delete child;
    }

    void buildForm(const model::Feature& f) {
        clearForm();
        auto* form = new QFormLayout(form_);
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

        auto* name = new QLineEdit(form_);
        name->setObjectName(QStringLiteral("name"));
        form->addRow(tr("Name"), name);
        connect(name, &QLineEdit::editingFinished, this, [this, name] { rename(name->text()); });
        updaters_.push_back([name](const model::Feature& g) {
            if (!name->hasFocus()) name->setText(QString::fromStdString(g.name()));
        });

        switch (f.type()) {
            case model::FeatureType::Box:
                addVec3(
                    form, QStringLiteral("corner"), tr("Corner"), false,
                    [](const model::Feature& g) { return static_cast<const model::BoxFeature&>(g).corner(); },
                    [](model::Feature& g, kernel::Vec3 v) {
                        static_cast<model::BoxFeature&>(g).setCorner(v);
                    });
                addVec3(
                    form, QStringLiteral("size"), tr("Size"), true,
                    [](const model::Feature& g) { return static_cast<const model::BoxFeature&>(g).size(); },
                    [](model::Feature& g, kernel::Vec3 v) { static_cast<model::BoxFeature&>(g).setSize(v); });
                break;
            case model::FeatureType::Cylinder:
                addNumber(
                    form, QStringLiteral("radius"), tr("Radius"),
                    [](const model::Feature& g) {
                        return static_cast<const model::CylinderFeature&>(g).radius();
                    },
                    [](model::Feature& g, double v) {
                        static_cast<model::CylinderFeature&>(g).setRadius(v);
                    });
                addNumber(
                    form, QStringLiteral("height"), tr("Height"),
                    [](const model::Feature& g) {
                        return static_cast<const model::CylinderFeature&>(g).height();
                    },
                    [](model::Feature& g, double v) {
                        static_cast<model::CylinderFeature&>(g).setHeight(v);
                    });
                addVec3(
                    form, QStringLiteral("base"), tr("Base"), false,
                    [](const model::Feature& g) {
                        return static_cast<const model::CylinderFeature&>(g).base();
                    },
                    [](model::Feature& g, kernel::Vec3 v) {
                        static_cast<model::CylinderFeature&>(g).setBase(v);
                    });
                addVec3(
                    form, QStringLiteral("axis"), tr("Axis"), false,
                    [](const model::Feature& g) {
                        return static_cast<const model::CylinderFeature&>(g).axis();
                    },
                    [](model::Feature& g, kernel::Vec3 v) {
                        static_cast<model::CylinderFeature&>(g).setAxis(v);
                    },
                    false);
                break;
            case model::FeatureType::Boolean: {
                auto* op = new QComboBox(form_);
                op->setObjectName(QStringLiteral("operation"));
                op->addItem(tr("Union: join them"), static_cast<int>(kernel::BoolOp::Union));
                op->addItem(tr("Cut: A minus B"), static_cast<int>(kernel::BoolOp::Cut));
                op->addItem(tr("Intersect: keep the overlap"), static_cast<int>(kernel::BoolOp::Intersect));
                form->addRow(tr("Operation"), op);
                connect(op, &QComboBox::currentIndexChanged, this, [this, op](int) {
                    const auto value = static_cast<kernel::BoolOp>(op->currentData().toInt());
                    edit(QStringLiteral("operation"),
                         [value](model::Feature& g) { static_cast<model::BooleanFeature&>(g).setOp(value); });
                });
                updaters_.push_back([op](const model::Feature& g) {
                    op->setCurrentIndex(
                        op->findData(static_cast<int>(static_cast<const model::BooleanFeature&>(g).op())));
                });
                auto* inputs = new QLabel(form_);
                inputs->setObjectName(QStringLiteral("inputs"));
                form->addRow(tr("Bodies"), inputs);
                updaters_.push_back([this, inputs](const model::Feature& g) {
                    const auto& b = static_cast<const model::BooleanFeature&>(g);
                    const auto* design = ctx_.documents().find(ctx_.selection().doc());
                    auto nameOf = [design](const FeatureId& id) {
                        const auto* s = design ? design->find(id) : nullptr;
                        return s ? QString::fromStdString(s->name()) : tr("(missing)");
                    };
                    inputs->setText(tr("A: %1, B: %2").arg(nameOf(b.a()), nameOf(b.b())));
                });
                break;
            }
            case model::FeatureType::Sketch: {
                auto* plane = new QLabel(form_);
                plane->setObjectName(QStringLiteral("plane"));
                form->addRow(tr("Plane"), plane);
                auto* drawn = new QLabel(form_);
                drawn->setObjectName(QStringLiteral("drawn"));
                drawn->setWordWrap(true);
                form->addRow(tr("Drawn"), drawn);
                auto* constraints = new QLabel(form_);
                constraints->setObjectName(QStringLiteral("constraints"));
                form->addRow(tr("Constraints"), constraints);
                auto* freedom = new QLabel(form_);
                freedom->setObjectName(QStringLiteral("freedom"));
                freedom->setWordWrap(true);
                freedom->setToolTip(
                    tr("How many ways the drawing can still move or change size. Add "
                       "constraints and dimensions until it's 0: fully constrained."));
                form->addRow(tr("Free to move"), freedom);
                updaters_.push_back([this, plane, drawn, constraints, freedom](const model::Feature& g) {
                    const auto& sk = static_cast<const model::SketchFeature&>(g);
                    const auto* design = ctx_.documents().find(ctx_.selection().doc());
                    if (sk.face() && design) {
                        plane->setText(tr("On the %1")
                                           .arg(QString::fromStdString(model::describeElement(
                                               *design, kernel::TopoKind::Face, sk.face()->face.name))));
                    } else {
                        switch (sk.plane()) {
                            case model::SketchPlane::XY:
                                plane->setText(tr("XY (the ground, seen from the top)"));
                                break;
                            case model::SketchPlane::XZ:
                                plane->setText(tr("XZ (standing, seen from the front)"));
                                break;
                            case model::SketchPlane::YZ:
                                plane->setText(tr("YZ (standing, seen from the right)"));
                                break;
                        }
                    }
                    int points = 0, lines = 0, circles = 0, arcs = 0, splines = 0;
                    for (const auto& e : sk.sketch().entities) {
                        points += std::holds_alternative<sketch::PointShape>(e.shape) ? 1 : 0;
                        lines += std::holds_alternative<sketch::LineShape>(e.shape) ? 1 : 0;
                        circles += std::holds_alternative<sketch::CircleShape>(e.shape) ? 1 : 0;
                        arcs += std::holds_alternative<sketch::ArcShape>(e.shape) ? 1 : 0;
                        splines += std::holds_alternative<sketch::SplineShape>(e.shape) ? 1 : 0;
                    }
                    QStringList parts;
                    if (lines) parts << tr("%n line(s)", nullptr, lines);
                    if (circles) parts << tr("%n circle(s)", nullptr, circles);
                    if (arcs) parts << tr("%n arc(s)", nullptr, arcs);
                    if (splines) parts << tr("%n spline(s)", nullptr, splines);
                    if (points) parts << tr("%n point(s)", nullptr, points);
                    drawn->setText(parts.isEmpty() ? tr("nothing yet") : parts.join(QStringLiteral(", ")));
                    constraints->setText(QString::number(sk.sketch().constraints.size()));
                    const auto* report = ctx_.documents().lastRebuild(ctx_.selection().doc());
                    const auto* r = report ? report->result(g.id()) : nullptr;
                    if (!r || !r->sketch || r->sketch->freedom < 0) {
                        freedom->setText(tr("unknown: the sketch didn't solve"));
                    } else if (r->sketch->freedom == 0) {
                        freedom->setText(tr("0: fully constrained"));
                    } else {
                        freedom->setText(QString::number(r->sketch->freedom));
                    }
                });
                break;
            }
            case model::FeatureType::Extrude:  // see addExtrude()
            case model::FeatureType::Revolve:  // see addRevolve()
            case model::FeatureType::Hole:     // see addHole()
            case model::FeatureType::Pattern:  // see addPattern()
            case model::FeatureType::Mirror:   // see addMirror()
            case model::FeatureType::Plane:    // see addPlane()
                break;
            case model::FeatureType::Fillet:
            case model::FeatureType::Chamfer: {
                const bool fillet = f.type() == model::FeatureType::Fillet;
                auto* body = new QLabel(form_);
                body->setObjectName(QStringLiteral("body"));
                form->addRow(tr("Body"), body);
                auto* edges = new QLabel(form_);
                edges->setObjectName(QStringLiteral("edges"));
                auto* change = new QPushButton(tr("Change edges"), form_);
                change->setObjectName(QStringLiteral("changeedges"));
                change->setToolTip(
                    tr("Pick this step's edges again, on its body as it was before this step."));
                connect(change, &QPushButton::clicked, this, [this, id = f.id()] { startChanging(id); });
                auto* row = new QHBoxLayout;
                row->addWidget(edges, 1);
                row->addWidget(change);
                form->addRow(tr("Edges"), row);
                updaters_.push_back([this, body, edges](const model::Feature& g) {
                    const auto& e = static_cast<const model::EdgeFeature&>(g);
                    const auto* design = ctx_.documents().find(ctx_.selection().doc());
                    const auto* s = design ? design->find(e.body()) : nullptr;
                    body->setText(s ? QString::fromStdString(s->name()) : tr("(missing)"));
                    edges->setText(QString::number(e.edges().size()));
                });
                addNumber(
                    form, QString::fromLatin1(fillet ? "radius" : "distance"),
                    fillet ? tr("Radius") : tr("Distance"),
                    [](const model::Feature& g) { return static_cast<const model::EdgeFeature&>(g).size(); },
                    [](model::Feature& g, double v) { static_cast<model::EdgeFeature&>(g).setSize(v); });
                break;
            }
            case model::FeatureType::Import: {
                // The file it came from, and a button to read it again after it changes.
                auto* file = new QLabel(form_);
                file->setObjectName(QStringLiteral("file"));
                file->setWordWrap(true);
                form->addRow(tr("File"), file);
                auto* reload = new QPushButton(tr("Reload from file"), form_);
                reload->setObjectName(QStringLiteral("reload"));
                reload->setToolTip(
                    tr("Read the file again, after it was changed in the other program. The design keeps "
                       "its own copy until then."));
                form->addRow(QString(), reload);
                const FeatureId step = f.id();
                connect(reload, &QPushButton::clicked, this, [this, step] {
                    auto problems = ctx_.documents().run(ctx_.selection().doc(), "feature.reload_import",
                                                         {{"feature", step}});
                    if (!problems.empty()) ui_.showProblems(problems);
                });
                updaters_.push_back([file](const model::Feature& g) {
                    const auto& im = static_cast<const model::ImportFeature&>(g);
                    file->setText(QString::fromStdString(im.fileName()));
                    file->setToolTip(QString::fromStdString(im.source()));
                });
                if (static_cast<const model::ImportFeature&>(f).format() == model::ImportFormat::Stl) {
                    // STL files don't say their unit: a part made in inches comes in 25.4 times too small.
                    auto* unit = new QComboBox(form_);
                    unit->setObjectName(QStringLiteral("unit"));
                    unit->setToolTip(
                        tr("The unit the STL file's numbers are in. STL files don't say, so pick "
                           "inches if the part came in 25.4 times too small."));
                    for (const auto u : allLengthUnits())
                        unit->addItem(QString::fromStdString(std::string(unitName(u))), static_cast<int>(u));
                    form->addRow(tr("File unit"), unit);
                    connect(unit, &QComboBox::activated, this, [this, unit](int index) {
                        const auto u = static_cast<LengthUnit>(unit->itemData(index).toInt());
                        edit(QStringLiteral("unit"),
                             [u](model::Feature& g) { static_cast<model::ImportFeature&>(g).setUnit(u); });
                    });
                    updaters_.push_back([unit](const model::Feature& g) {
                        const auto u = static_cast<const model::ImportFeature&>(g).unit();
                        unit->setCurrentIndex(unit->findData(static_cast<int>(u)));
                    });
                }
                break;
            }
            case model::FeatureType::Shell: {
                auto* body = new QLabel(form_);
                body->setObjectName(QStringLiteral("body"));
                form->addRow(tr("Body"), body);
                auto* open = new QLabel(form_);
                open->setObjectName(QStringLiteral("faces"));
                open->setToolTip(
                    tr("How many faces are left open. To change them, delete this step and add "
                       "it again with the faces picked."));
                form->addRow(tr("Open faces"), open);
                updaters_.push_back([this, body, open](const model::Feature& g) {
                    const auto& sh = static_cast<const model::ShellFeature&>(g);
                    const auto* design = ctx_.documents().find(ctx_.selection().doc());
                    const auto* s = design ? design->find(sh.body()) : nullptr;
                    body->setText(s ? QString::fromStdString(s->name()) : tr("(missing)"));
                    open->setText(QString::number(sh.faces().size()));
                });
                addNumber(
                    form, QStringLiteral("thickness"), tr("Thickness"),
                    [](const model::Feature& g) {
                        return static_cast<const model::ShellFeature&>(g).thickness();
                    },
                    [](model::Feature& g, double v) {
                        static_cast<model::ShellFeature&>(g).setThickness(v);
                    });
                break;
            }
            case model::FeatureType::Move: {
                auto* body = new QLabel(form_);
                body->setObjectName(QStringLiteral("body"));
                form->addRow(tr("Body"), body);
                updaters_.push_back([this, body](const model::Feature& g) {
                    const auto* design = ctx_.documents().find(ctx_.selection().doc());
                    const auto* s =
                        design ? design->find(static_cast<const model::MoveFeature&>(g).body()) : nullptr;
                    body->setText(s ? QString::fromStdString(s->name()) : tr("(missing)"));
                });
                auto motion = [](const model::Feature& g) {
                    return static_cast<const model::MoveFeature&>(g).motion();
                };
                auto setMotion = [](model::Feature& g, auto change) {
                    auto& m = static_cast<model::MoveFeature&>(g);
                    auto value = m.motion();
                    change(value);
                    m.setMotion(value);
                };
                addVec3(
                    form, QStringLiteral("offset"), tr("Move"), false,
                    [motion](const model::Feature& g) { return motion(g).offset; },
                    [setMotion](model::Feature& g, kernel::Vec3 v) {
                        setMotion(g, [v](kernel::Motion& m) { m.offset = v; });
                    });
                // Turn: degrees for the student, radians inside.
                auto* turn = new ui::FormulaBox(form_);
                turn->setObjectName(QStringLiteral("turn"));
                turn->setDecimals(2);
                turn->setRange(-360.0, 360.0);
                turn->setSuffix(QStringLiteral("°"));
                turn->setKeyboardTracking(false);
                turn->setToolTip(
                    tr("How far to turn the body before moving it, in degrees. Positive turns "
                       "counterclockwise, looking down the axis."));
                form->addRow(tr("Turn"), turn);
                connect(turn, &QDoubleSpinBox::valueChanged, this, [this, setMotion](double degrees) {
                    edit(QStringLiteral("turn"), [setMotion, degrees](model::Feature& g) {
                        setMotion(g, [degrees](kernel::Motion& m) {
                            m.angle = degrees * std::numbers::pi / 180.0;
                        });
                    });
                });
                updaters_.push_back([turn, motion](const model::Feature& g) {
                    turn->setValue(motion(g).angle * 180.0 / std::numbers::pi);
                });
                addVec3(
                    form, QStringLiteral("axis"), tr("Axis"), false,
                    [motion](const model::Feature& g) { return motion(g).axis; },
                    [setMotion](model::Feature& g, kernel::Vec3 v) {
                        setMotion(g, [v](kernel::Motion& m) { m.axis = v; });
                    },
                    false);
                addVec3(
                    form, QStringLiteral("center"), tr("Center"), false,
                    [motion](const model::Feature& g) { return motion(g).center; },
                    [setMotion](model::Feature& g, kernel::Vec3 v) {
                        setMotion(g, [v](kernel::Motion& m) { m.center = v; });
                    });
                // A linked copy keeps the body where it is and adds a moved copy.
                auto* copy = new QCheckBox(tr("Keep the original (linked copy)"), form_);
                copy->setObjectName(QStringLiteral("copy"));
                copy->setToolTip(
                    tr("Keep the body where it is, and make a moved copy of it. Change the original, and "
                       "the copy changes too."));
                form->addRow(tr("Copy"), copy);
                connect(copy, &QCheckBox::clicked, this, [this](bool on) {
                    edit(QStringLiteral("copy"),
                         [on](model::Feature& g) { static_cast<model::MoveFeature&>(g).setCopy(on); });
                });
                updaters_.push_back([copy](const model::Feature& g) {
                    copy->setChecked(static_cast<const model::MoveFeature&>(g).copy());
                });
                break;
            }
        }
        if (f.type() == model::FeatureType::Extrude) addExtrude(form);
        if (f.type() == model::FeatureType::Revolve) addRevolve(form);
        if (f.type() == model::FeatureType::Hole) addHole(form, static_cast<const model::HoleFeature&>(f));
        if (f.type() == model::FeatureType::Pattern) addPattern(form);
        if (f.type() == model::FeatureType::Mirror) addMirror(form);
        if (f.type() == model::FeatureType::Plane) addPlane(form);
        // Sketches and planes aren't bodies: no material, volume or mass.
        if (f.type() != model::FeatureType::Sketch && f.type() != model::FeatureType::Plane)
            addMaterial(form);
    }

    const model::Document* design() const { return ctx_.documents().find(ctx_.selection().doc()); }

    // An Extrude: which sketch, how far, and which way.
    void addExtrude(QFormLayout* form) {
        addSketchName<model::ExtrudeFeature>(form);
        addNumber(
            form, QStringLiteral("distance"), tr("Distance"),
            [](const model::Feature& g) { return static_cast<const model::ExtrudeFeature&>(g).distance(); },
            [](model::Feature& g, double v) { static_cast<model::ExtrudeFeature&>(g).setDistance(v); });
        addSide<model::ExtrudeFeature>(form,
                                       {tr("One side (out of the sketch's front)"),
                                        tr("Flipped (the other way)"), tr("Both sides (half each way)")},
                                       tr("Which way the shape goes from its sketch."));
        addOperation<model::ExtrudeFeature>(form, tr("(a pocket, a hole)"));
    }

    void addRevolve(QFormLayout* form) {
        addSketchName<model::RevolveFeature>(form);
        // The axis: the sketch's x or y axis, or one of its lines.
        auto* axis = new QComboBox(form_);
        axis->setObjectName(QStringLiteral("axis"));
        axis->setToolTip(
            tr("The line the shape turns about: the sketch's x or y axis, or a line of the sketch. The shape "
               "may touch it but not cross it."));
        form->addRow(tr("Axis"), axis);
        connect(axis, &QComboBox::activated, this, [this, axis](int index) {
            const QString key = axis->itemData(index).toString();
            model::RevolveAxis a;
            if (key == QLatin1String("x")) {
                a.kind = model::RevolveAxis::Kind::X;
            } else if (key != QLatin1String("y")) {
                const auto line = EntityId::parse(key.toStdString());
                if (!line) return;
                a = {model::RevolveAxis::Kind::Line, *line};
            }
            edit(QStringLiteral("axis"),
                 [a](model::Feature& g) { static_cast<model::RevolveFeature&>(g).setAxis(a); });
        });
        updaters_.push_back([this, axis](const model::Feature& g) {
            const auto& r = static_cast<const model::RevolveFeature&>(g);
            axis->clear();
            axis->addItem(tr("Sketch's y axis"), QStringLiteral("y"));
            axis->addItem(tr("Sketch's x axis"), QStringLiteral("x"));
            const auto* d = design();
            if (const auto* sk =
                    d ? dynamic_cast<const model::SketchFeature*>(d->find(r.sketch())) : nullptr) {
                for (const auto& e : sk->sketch().entities) {
                    if (!std::holds_alternative<sketch::LineShape>(e.shape)) continue;
                    axis->addItem(e.construction ? tr("%1 (helper line)").arg(qs(e.name)) : qs(e.name),
                                  qs(e.id.toString()));
                }
            }
            const QString want = r.axis().kind == model::RevolveAxis::Kind::X ? QStringLiteral("x")
                                 : r.axis().kind == model::RevolveAxis::Kind::Y
                                     ? QStringLiteral("y")
                                     : qs(r.axis().line.toString());
            if (axis->findData(want) < 0) axis->addItem(tr("(deleted line)"), want);
            axis->setCurrentIndex(axis->findData(want));
        });
        // Angle: degrees for the student, radians inside.
        auto* angle = new ui::FormulaBox(form_);
        angle->setObjectName(QStringLiteral("angle"));
        angle->setDecimals(2);
        angle->setRange(0.01, 360.0);
        angle->setSuffix(QStringLiteral("°"));
        angle->setKeyboardTracking(false);
        angle->setAccelerated(true);
        form->addRow(tr("Angle"), angle);
        connect(angle, &QDoubleSpinBox::valueChanged, this, [this](double degrees) {
            edit(QStringLiteral("angle"), [degrees](model::Feature& g) {
                static_cast<model::RevolveFeature&>(g).setAngle(degrees * std::numbers::pi / 180.0);
            });
        });
        updaters_.push_back([this, angle](const model::Feature& g) {
            angle->setValue(static_cast<const model::RevolveFeature&>(g).angle() * 180.0 / std::numbers::pi);
            const auto tip = formulaTip(g, QStringLiteral("angle"));
            angle->setToolTip(tip.isEmpty() ? tr("How far to turn, in degrees: 360 for all the way round.")
                                            : tip);
        });
        addSide<model::RevolveFeature>(
            form, {tr("One way"), tr("Flipped (the other way)"), tr("Both ways (half each way)")},
            tr("Which way the shape turns from its sketch, for less than a full turn."));
        addOperation<model::RevolveFeature>(form, tr("(a groove)"));
    }

    void addHole(QFormLayout* form, const model::HoleFeature& first) {
        using model::HoleFeature;
        auto hole = [](const model::Feature& g) -> const HoleFeature& {
            return static_cast<const HoleFeature&>(g);
        };
        auto* body = new QLabel(form_);
        body->setObjectName(QStringLiteral("body"));
        form->addRow(tr("Body"), body);
        auto* where = new QLabel(form_);
        where->setObjectName(QStringLiteral("placement"));
        where->setWordWrap(true);
        form->addRow(tr("Where"), where);
        updaters_.push_back([this, body, where, hole](const model::Feature& g) {
            const auto& h = hole(g);
            const auto* d = design();
            const auto* b = d ? d->find(h.body()) : nullptr;
            body->setText(b ? qs(b->name()) : tr("(missing)"));
            if (h.placement().kind == model::HolePlacement::Kind::FromEdges) {
                where->setText(tr("In from two edges"));
                return;
            }
            const auto* sk =
                d ? dynamic_cast<const model::SketchFeature*>(d->find(h.placement().sketch)) : nullptr;
            int points = 0;
            if (sk) {
                for (const auto& e : sk->sketch().entities)
                    points += std::holds_alternative<sketch::PointShape>(e.shape) && !e.construction;
            }
            where->setText(sk ? tr("At the points of %1 (%n holes)", "", points).arg(qs(sk->name()))
                              : tr("(missing sketch)"));
        });
        if (first.placement().kind == model::HolePlacement::Kind::FromEdges) {
            addNumber(
                form, QStringLiteral("distance_a"), tr("From edge 1"),
                [hole](const model::Feature& g) { return hole(g).placement().distanceA; },
                [](model::Feature& g, double v) { static_cast<HoleFeature&>(g).setNumber("distance_a", v); });
            addNumber(
                form, QStringLiteral("distance_b"), tr("From edge 2"),
                [hole](const model::Feature& g) { return hole(g).placement().distanceB; },
                [](model::Feature& g, double v) { static_cast<HoleFeature&>(g).setNumber("distance_b", v); });
        }

        // Kind, and the screw it's sized for.
        auto* kind = new QComboBox(form_);
        kind->setObjectName(QStringLiteral("kind"));
        kind->addItem(tr("Simple"), static_cast<int>(model::HoleKind::Simple));
        kind->addItem(tr("Counterbore (screw head below the face)"),
                      static_cast<int>(model::HoleKind::Counterbore));
        kind->addItem(tr("Countersink (flat head, flush)"), static_cast<int>(model::HoleKind::Countersink));
        form->addRow(tr("Kind"), kind);
        connect(kind, &QComboBox::activated, this, [this, kind](int index) {
            const auto k = static_cast<model::HoleKind>(kind->itemData(index).toInt());
            edit(QStringLiteral("kind"), [k](model::Feature& g) { static_cast<HoleFeature&>(g).setKind(k); });
        });
        auto* size = new QComboBox(form_);
        size->setObjectName(QStringLiteral("size"));
        size->addItem(tr("Its own size"), QString());
        for (const auto& s : model::screwSizes()) size->addItem(qs(s.name), qs(s.name));
        size->setToolTip(tr("A metric screw: sets the hole, counterbore and countersink to fit it."));
        form->addRow(tr("Screw"), size);
        auto* fit = new QComboBox(form_);
        fit->setObjectName(QStringLiteral("fit"));
        fit->addItem(tr("Clearance (the screw slides through)"), static_cast<int>(model::HoleFit::Clearance));
        fit->addItem(tr("Tap (the screw's thread grips)"), static_cast<int>(model::HoleFit::Tap));
        form->addRow(tr("Fit"), fit);
        auto resize = [this, size, fit] {
            const auto name = size->currentData().toString().toStdString();
            const auto f = static_cast<model::HoleFit>(fit->currentData().toInt());
            if (name.empty()) return;
            edit(QStringLiteral("size"),
                 [name, f](model::Feature& g) { static_cast<HoleFeature&>(g).setSize(name, f); });
        };
        connect(size, &QComboBox::activated, this, resize);
        connect(fit, &QComboBox::activated, this, resize);

        addNumber(
            form, QStringLiteral("diameter"), tr("Diameter"),
            [hole](const model::Feature& g) { return hole(g).diameter(); },
            [](model::Feature& g, double v) { static_cast<HoleFeature&>(g).setDiameter(v); });
        auto* through = new QCheckBox(tr("All the way through"), form_);
        through->setObjectName(QStringLiteral("through"));
        form->addRow(tr("Depth"), through);
        connect(through, &QCheckBox::clicked, this, [this](bool on) {
            edit(QStringLiteral("through"), [on](model::Feature& g) {
                auto& h = static_cast<HoleFeature&>(g);
                h.setDepth(on, h.depth());
            });
        });
        addNumber(
            form, QStringLiteral("depth"), QString(),
            [hole](const model::Feature& g) { return hole(g).depth(); },
            [](model::Feature& g, double v) {
                auto& h = static_cast<HoleFeature&>(g);
                h.setDepth(false, v);
            });
        addNumber(
            form, QStringLiteral("counterbore_diameter"), tr("Counterbore"),
            [hole](const model::Feature& g) { return hole(g).counterboreDiameter(); },
            [](model::Feature& g, double v) {
                auto& h = static_cast<HoleFeature&>(g);
                h.setCounterbore(v, h.counterboreDepth());
            });
        addNumber(
            form, QStringLiteral("counterbore_depth"), tr("Counterbore depth"),
            [hole](const model::Feature& g) { return hole(g).counterboreDepth(); },
            [](model::Feature& g, double v) {
                auto& h = static_cast<HoleFeature&>(g);
                h.setCounterbore(h.counterboreDiameter(), v);
            });
        addNumber(
            form, QStringLiteral("countersink_diameter"), tr("Countersink"),
            [hole](const model::Feature& g) { return hole(g).countersinkDiameter(); },
            [](model::Feature& g, double v) {
                auto& h = static_cast<HoleFeature&>(g);
                h.setCountersink(v, h.countersinkAngle());
            });
        auto* angle = new ui::FormulaBox(form_);
        angle->setObjectName(QStringLiteral("countersink_angle"));
        angle->setDecimals(1);
        angle->setRange(1.0, 179.0);
        angle->setSuffix(QStringLiteral("°"));
        angle->setKeyboardTracking(false);
        angle->setToolTip(
            tr("How wide the countersink's cone is, across: 90 degrees for metric flat head screws."));
        form->addRow(tr("Countersink angle"), angle);
        connect(angle, &QDoubleSpinBox::valueChanged, this, [this](double degrees) {
            edit(QStringLiteral("countersink_angle"), [degrees](model::Feature& g) {
                auto& h = static_cast<HoleFeature&>(g);
                h.setCountersink(h.countersinkDiameter(), degrees * std::numbers::pi / 180.0);
            });
        });
        auto* flip = new QCheckBox(tr("Drill the other way"), form_);
        flip->setObjectName(QStringLiteral("flip"));
        form->addRow(tr("Flip"), flip);
        connect(flip, &QCheckBox::clicked, this, [this](bool on) {
            edit(QStringLiteral("flip"),
                 [on](model::Feature& g) { static_cast<HoleFeature&>(g).setFlipped(on); });
        });

        // Show what fits this hole: the depth when it isn't through, and the top's sizes.
        updaters_.push_back(
            [this, form, hole, kind, size, fit, through, angle, flip](const model::Feature& g) {
                const auto& h = hole(g);
                kind->setCurrentIndex(kind->findData(static_cast<int>(h.kind())));
                size->setCurrentIndex(std::max(0, size->findData(qs(h.size()))));
                fit->setCurrentIndex(fit->findData(static_cast<int>(h.fit())));
                fit->setEnabled(!h.size().empty());
                through->setChecked(h.through());
                angle->setValue(h.countersinkAngle() * 180.0 / std::numbers::pi);
                flip->setChecked(h.flipped());
                auto show = [&](const char* key, bool on) {
                    if (auto* w = form_->findChild<QWidget*>(QLatin1String(key))) form->setRowVisible(w, on);
                };
                show("depth", !h.through());
                const bool cb = h.kind() == model::HoleKind::Counterbore,
                           cs = h.kind() == model::HoleKind::Countersink;
                show("counterbore_diameter", cb);
                show("counterbore_depth", cb);
                show("countersink_diameter", cs);
                show("countersink_angle", cs);
            });
    }

    void addPattern(QFormLayout* form) {
        using model::PatternFeature;
        using model::PatternLayout;
        auto pattern = [](const model::Feature& g) -> const PatternFeature& {
            return static_cast<const PatternFeature&>(g);
        };
        // Changes the layout of the step being edited.
        auto relayout = [this](const QString& field, std::function<void(PatternLayout&)> change) {
            edit(field, [change](model::Feature& g) {
                auto& p = static_cast<PatternFeature&>(g);
                auto l = p.layout();
                change(l);
                p.setLayout(l);
            });
        };
        auto* what = new QLabel(form_);
        what->setObjectName(QStringLiteral("repeats"));
        what->setWordWrap(true);
        form->addRow(tr("Repeats"), what);
        updaters_.push_back([this, what, pattern](const model::Feature& g) {
            const auto& p = pattern(g);
            what->setText(repeatsText(p.steps(), p.body(), tr("Copies of %1")));
        });

        auto* kind = new QComboBox(form_);
        kind->setObjectName(QStringLiteral("kind"));
        kind->addItem(tr("Row (or grid)"), static_cast<int>(PatternLayout::Kind::Linear));
        kind->addItem(tr("Circle"), static_cast<int>(PatternLayout::Kind::Circular));
        form->addRow(tr("Kind"), kind);
        connect(kind, &QComboBox::activated, this, [relayout, kind](int index) {
            const auto k = static_cast<PatternLayout::Kind>(kind->itemData(index).toInt());
            relayout(QStringLiteral("kind"), [k](PatternLayout& l) {
                if (l.kind != k && k == PatternLayout::Kind::Circular && l.count < 2) l.count = 6;
                l.kind = k;
            });
        });
        auto addCount = [this, form, relayout](const char* key, const QString& label, const QString& tip,
                                               int PatternLayout::*member) {
            auto* box = new QSpinBox(form_);
            box->setObjectName(QLatin1String(key));
            box->setRange(1, 200);
            box->setKeyboardTracking(false);
            box->setToolTip(tip);
            form->addRow(label, box);
            connect(box, &QSpinBox::valueChanged, this, [relayout, member, key](int n) {
                relayout(QLatin1String(key), [member, n](PatternLayout& l) { l.*member = n; });
            });
            updaters_.push_back([box, member](const model::Feature& g) {
                box->setValue(static_cast<const PatternFeature&>(g).layout().*member);
            });
        };
        addCount("count", tr("How many"),
                 tr("How many in the row, or round the circle, the first one included."),
                 &PatternLayout::count);
        addVec3(
            form, QStringLiteral("direction"), tr("Direction"), false,
            [pattern](const model::Feature& g) { return pattern(g).layout().direction; },
            [](model::Feature& g, kernel::Vec3 v) {
                auto& p = static_cast<PatternFeature&>(g);
                auto l = p.layout();
                l.direction = v;
                p.setLayout(l);
            },
            false);
        addNumber(
            form, QStringLiteral("spacing"), tr("Spacing"),
            [pattern](const model::Feature& g) { return pattern(g).layout().spacing; },
            [](model::Feature& g, double v) { static_cast<PatternFeature&>(g).setNumber("spacing", v); });
        addCount("count2", tr("Rows"), tr("How many rows, for a grid: 1 for one row."),
                 &PatternLayout::count2);
        addVec3(
            form, QStringLiteral("direction2"), tr("Rows go"), false,
            [pattern](const model::Feature& g) { return pattern(g).layout().direction2; },
            [](model::Feature& g, kernel::Vec3 v) {
                auto& p = static_cast<PatternFeature&>(g);
                auto l = p.layout();
                l.direction2 = v;
                p.setLayout(l);
            },
            false);
        addNumber(
            form, QStringLiteral("spacing2"), tr("Row spacing"),
            [pattern](const model::Feature& g) { return pattern(g).layout().spacing2; },
            [](model::Feature& g, double v) { static_cast<PatternFeature&>(g).setNumber("spacing2", v); });
        addVec3(
            form, QStringLiteral("axis"), tr("Axis"), false,
            [pattern](const model::Feature& g) { return pattern(g).layout().axis; },
            [](model::Feature& g, kernel::Vec3 v) {
                auto& p = static_cast<PatternFeature&>(g);
                auto l = p.layout();
                l.axis = v;
                p.setLayout(l);
            },
            false);
        addVec3(
            form, QStringLiteral("center"), tr("Axis through"), false,
            [pattern](const model::Feature& g) { return pattern(g).layout().center; },
            [](model::Feature& g, kernel::Vec3 v) {
                auto& p = static_cast<PatternFeature&>(g);
                auto l = p.layout();
                l.center = v;
                p.setLayout(l);
            });
        auto* angle = new ui::FormulaBox(form_);
        angle->setObjectName(QStringLiteral("angle"));
        angle->setDecimals(1);
        angle->setRange(0.1, 360.0);
        angle->setSuffix(QStringLiteral("°"));
        angle->setKeyboardTracking(false);
        angle->setToolTip(tr("The angle they spread over: 360 for evenly all the way round."));
        form->addRow(tr("Angle"), angle);
        connect(angle, &QDoubleSpinBox::valueChanged, this, [relayout](double degrees) {
            relayout(QStringLiteral("angle"),
                     [degrees](PatternLayout& l) { l.angle = degrees * std::numbers::pi / 180.0; });
        });

        // Show the rows that fit a row or a circle.
        updaters_.push_back([this, form, pattern, kind, angle](const model::Feature& g) {
            const auto& l = pattern(g).layout();
            kind->setCurrentIndex(kind->findData(static_cast<int>(l.kind)));
            angle->setValue(l.angle * 180.0 / std::numbers::pi);
            const bool row = l.kind == PatternLayout::Kind::Linear;
            auto show = [&](const char* key, bool on) {
                // A box of its own, or the first of an x, y, z row's boxes.
                auto* w = form_->findChild<QWidget*>(QLatin1String(key));
                if (!w) {
                    if (auto* x = form_->findChild<QWidget*>(QLatin1String(key) + QStringLiteral(".x")))
                        w = x->parentWidget();
                }
                if (w) form->setRowVisible(w, on);
            };
            for (const char* key : {"direction", "spacing", "count2", "direction2", "spacing2"})
                show(key, row);
            show("direction2", row && l.count2 > 1);
            show("spacing2", row && l.count2 > 1);
            for (const char* key : {"axis", "center", "angle"}) show(key, !row);
        });
    }

    void addMirror(QFormLayout* form) {
        using model::MirrorFeature;
        using model::MirrorPlane;
        auto mirror = [](const model::Feature& g) -> const MirrorFeature& {
            return static_cast<const MirrorFeature&>(g);
        };
        auto* what = new QLabel(form_);
        what->setObjectName(QStringLiteral("mirrors"));
        what->setWordWrap(true);
        form->addRow(tr("Mirrors"), what);
        updaters_.push_back([this, what, mirror](const model::Feature& g) {
            const auto& m = mirror(g);
            what->setText(repeatsText(m.steps(), m.body(), tr("The whole of %1")));
        });
        auto* plane = new QComboBox(form_);
        plane->setObjectName(QStringLiteral("plane"));
        plane->addItem(tr("YZ (left and right swap)"), static_cast<int>(MirrorPlane::Kind::YZ));
        plane->addItem(tr("XZ (front and back swap)"), static_cast<int>(MirrorPlane::Kind::XZ));
        plane->addItem(tr("XY (top and bottom swap)"), static_cast<int>(MirrorPlane::Kind::XY));
        plane->setToolTip(tr("The plane it mirrors across, through 0 unless it's moved by Offset."));
        form->addRow(tr("Plane"), plane);
        connect(plane, &QComboBox::activated, this, [this, plane](int index) {
            const auto k = static_cast<MirrorPlane::Kind>(plane->itemData(index).toInt());
            edit(QStringLiteral("plane"), [k](model::Feature& g) {
                auto& m = static_cast<MirrorFeature&>(g);
                auto p = m.plane();
                p.kind = k;
                m.setPlane(p);
            });
        });
        addNumber(
            form, QStringLiteral("offset"), tr("Offset"),
            [mirror](const model::Feature& g) { return mirror(g).plane().offset; },
            [](model::Feature& g, double v) { static_cast<MirrorFeature&>(g).setNumber("offset", v); },
            false);
        updaters_.push_back([this, form, mirror, plane](const model::Feature& g) {
            const auto& p = mirror(g).plane();
            // A face, from the command line, shows as one more choice.
            const int faceAt = plane->findData(static_cast<int>(MirrorPlane::Kind::Face));
            if (p.kind == MirrorPlane::Kind::Face && faceAt < 0) {
                plane->addItem(tr("A face of the body"), static_cast<int>(MirrorPlane::Kind::Face));
            }
            plane->setCurrentIndex(plane->findData(static_cast<int>(p.kind)));
            if (auto* w = form_->findChild<QWidget*>(QStringLiteral("offset")))
                form->setRowVisible(w, p.kind != MirrorPlane::Kind::Face);
        });
    }

    void addPlane(QFormLayout* form) {
        using model::PlaneFeature;
        using model::PlaneFrom;
        using model::PlaneHinge;
        auto plane = [](const model::Feature& g) -> const PlaneFeature& {
            return static_cast<const PlaneFeature&>(g);
        };
        // Where it starts: a base plane can be changed here; a face or another plane is shown.
        auto* from = new QComboBox(form_);
        from->setObjectName(QStringLiteral("from"));
        from->setToolTip(tr("What the plane starts from, before it's turned and moved."));
        form->addRow(tr("Starts from"), from);
        connect(from, &QComboBox::activated, this, [this, from](int index) {
            const QString key = from->itemData(index).toString();
            if (key != QLatin1String("xy") && key != QLatin1String("xz") && key != QLatin1String("yz"))
                return;
            const auto base = key == QLatin1String("xz")   ? model::SketchPlane::XZ
                              : key == QLatin1String("yz") ? model::SketchPlane::YZ
                                                           : model::SketchPlane::XY;
            edit(QStringLiteral("from"), [base](model::Feature& g) {
                auto& p = static_cast<PlaneFeature&>(g);
                PlaneFrom f;
                f.base = base;
                p.setFrom(f);
                if (p.hinge().kind == PlaneHinge::Kind::Edge) p.setHinge({});  // an edge needs a face
            });
        });
        addNumber(
            form, QStringLiteral("offset"), tr("Offset"),
            [plane](const model::Feature& g) { return plane(g).offset(); },
            [](model::Feature& g, double v) { static_cast<PlaneFeature&>(g).setOffset(v); }, false);
        auto* angle = new ui::FormulaBox(form_);
        angle->setObjectName(QStringLiteral("angle"));
        angle->setDecimals(1);
        angle->setRange(-360.0, 360.0);
        angle->setSuffix(QStringLiteral("°"));
        angle->setKeyboardTracking(false);
        angle->setToolTip(tr("How far it's tilted about its hinge. Below 0 tilts it the other way."));
        form->addRow(tr("Angle"), angle);
        connect(angle, &QDoubleSpinBox::valueChanged, this, [this](double degrees) {
            edit(QStringLiteral("angle"), [degrees](model::Feature& g) {
                static_cast<PlaneFeature&>(g).setAngle(degrees * std::numbers::pi / 180.0);
            });
        });
        auto* hinge = new QComboBox(form_);
        hinge->setObjectName(QStringLiteral("hinge"));
        hinge->addItem(tr("Its x direction"), static_cast<int>(PlaneHinge::Kind::X));
        hinge->addItem(tr("Its y direction"), static_cast<int>(PlaneHinge::Kind::Y));
        hinge->setToolTip(tr("The line it tilts about, through the starting plane's 0, 0."));
        form->addRow(tr("Tilts about"), hinge);
        connect(hinge, &QComboBox::activated, this, [this, hinge](int index) {
            const auto k = static_cast<PlaneHinge::Kind>(hinge->itemData(index).toInt());
            if (k == PlaneHinge::Kind::Edge) return;
            edit(QStringLiteral("hinge"),
                 [k](model::Feature& g) { static_cast<PlaneFeature&>(g).setHinge(PlaneHinge{k, {}}); });
        });
        updaters_.push_back([this, plane, from, angle, hinge](const model::Feature& g) {
            const auto& p = plane(g);
            from->clear();
            from->addItem(tr("XY (the ground)"), QStringLiteral("xy"));
            from->addItem(tr("XZ (facing the front)"), QStringLiteral("xz"));
            from->addItem(tr("YZ (facing the right)"), QStringLiteral("yz"));
            const auto* d = design();
            auto nameOf = [d](const FeatureId& id) {
                const auto* f = d ? d->find(id) : nullptr;
                return f ? qs(f->name()) : QObject::tr("(missing)");
            };
            const auto& f = p.from();
            if (f.kind == PlaneFrom::Kind::Face) {
                from->addItem(tr("A face of %1").arg(nameOf(f.face.body)), QStringLiteral("face"));
                from->setCurrentIndex(3);
            } else if (f.kind == PlaneFrom::Kind::Plane) {
                from->addItem(nameOf(f.plane), QStringLiteral("plane"));
                from->setCurrentIndex(3);
            } else {
                from->setCurrentIndex(f.base == model::SketchPlane::XZ   ? 1
                                      : f.base == model::SketchPlane::YZ ? 2
                                                                         : 0);
            }
            angle->setValue(p.angle() * 180.0 / std::numbers::pi);
            const int edgeAt = hinge->findData(static_cast<int>(PlaneHinge::Kind::Edge));
            if (p.hinge().kind == PlaneHinge::Kind::Edge && edgeAt < 0) {
                hinge->addItem(tr("An edge of the face"), static_cast<int>(PlaneHinge::Kind::Edge));
            }
            hinge->setCurrentIndex(hinge->findData(static_cast<int>(p.hinge().kind)));
        });
    }

    // What a Pattern or Mirror repeats, in words: its steps (and the body, when
    // it's a later one), or `whole` with the body's name.
    QString repeatsText(const std::vector<FeatureId>& steps, const FeatureId& body,
                        const QString& whole) const {
        const auto* d = design();
        auto nameOf = [d](const FeatureId& id) {
            const auto* f = d ? d->find(id) : nullptr;
            return f ? qs(f->name()) : QObject::tr("(missing)");
        };
        if (steps.empty()) return whole.arg(nameOf(body));
        QStringList names;
        for (const auto& s : steps) names << nameOf(s);
        const bool own = std::find(steps.begin(), steps.end(), body) != steps.end();
        return own ? names.join(QStringLiteral(", "))
                   : tr("%1, on %2").arg(names.join(QStringLiteral(", ")), nameOf(body));
    }

    // A sketch step's sketch, by name.
    template <class Step>
    void addSketchName(QFormLayout* form) {
        auto* sketch = new QLabel(form_);
        sketch->setObjectName(QStringLiteral("sketch"));
        form->addRow(tr("Sketch"), sketch);
        updaters_.push_back([this, sketch](const model::Feature& g) {
            const auto* d = design();
            const auto* s = d ? d->find(static_cast<const Step&>(g).sketch()) : nullptr;
            sketch->setText(s ? QString::fromStdString(s->name()) : tr("(missing)"));
        });
    }

    // Which way an Extrude or Revolve goes: one, flipped or both.
    template <class Step>
    void addSide(QFormLayout* form, const std::array<QString, 3>& labels, const QString& tip) {
        auto* side = new QComboBox(form_);
        side->setObjectName(QStringLiteral("side"));
        side->addItem(labels[0], static_cast<int>(model::ExtrudeSide::One));
        side->addItem(labels[1], static_cast<int>(model::ExtrudeSide::Flip));
        side->addItem(labels[2], static_cast<int>(model::ExtrudeSide::Both));
        side->setToolTip(tip);
        form->addRow(tr("Side"), side);
        connect(side, &QComboBox::activated, this, [this, side](int index) {
            const auto s = static_cast<model::ExtrudeSide>(side->itemData(index).toInt());
            edit(QStringLiteral("side"), [s](model::Feature& g) { static_cast<Step&>(g).setSide(s); });
        });
        updaters_.push_back([side](const model::Feature& g) {
            const int want = static_cast<int>(static_cast<const Step&>(g).side());
            side->setCurrentIndex(side->findData(want));
        });
    }

    // What an Extrude or Revolve does with its solid, and to which body.
    template <class Step>
    void addOperation(QFormLayout* form, const QString& cutExample) {
        auto* op = new QComboBox(form_);
        op->setObjectName(QStringLiteral("operation"));
        op->addItem(tr("New body"), static_cast<int>(model::ExtrudeOp::NewBody));
        op->addItem(tr("Join (add to a body)"), static_cast<int>(model::ExtrudeOp::Join));
        op->addItem(tr("Cut (take out of a body)"), static_cast<int>(model::ExtrudeOp::Cut));
        op->setToolTip(
            tr("Make a body of its own, add the shape to a body (a peg, a tab), or cut it out of one "
               "%1.")
                .arg(cutExample));
        form->addRow(tr("Operation"), op);
        auto* body = new QComboBox(form_);
        body->setObjectName(QStringLiteral("body"));
        body->setToolTip(tr("The body it joins onto or cuts from: one made before this step."));
        form->addRow(tr("Body"), body);
        connect(op, &QComboBox::activated, this, [this, op](int index) {
            const auto want = static_cast<model::ExtrudeOp>(op->itemData(index).toInt());
            const auto* d = design();
            edit(QStringLiteral("operation"), [want, d](model::Feature& g) {
                auto& e = static_cast<Step&>(g);
                // Keep its body; or the body its sketch is on; or the last body before it.
                FeatureId target = e.body();
                const auto* sk = d ? dynamic_cast<const model::SketchFeature*>(d->find(e.sketch())) : nullptr;
                if (target.isNil() && sk && sk->face()) target = sk->face()->body;
                if (target.isNil() && d) {
                    for (const auto& id : bodiesBefore(*d, e.id())) target = id;
                }
                // An extruded cut from a face goes into the body.
                if constexpr (std::is_same_v<Step, model::ExtrudeFeature>) {
                    if (want == model::ExtrudeOp::Cut && sk && sk->face() &&
                        e.side() == model::ExtrudeSide::One) {
                        e.setSide(model::ExtrudeSide::Flip);
                    }
                }
                e.setOperation(want, target);
            });
        });
        connect(body, &QComboBox::activated, this, [this, body](int index) {
            const auto id = FeatureId::parse(body->itemData(index).toString().toStdString());
            if (!id) return;
            edit(QStringLiteral("body"), [id](model::Feature& g) {
                auto& e = static_cast<Step&>(g);
                e.setOperation(e.operation(), *id);
            });
        });
        updaters_.push_back([this, op, body](const model::Feature& g) {
            const auto& e = static_cast<const Step&>(g);
            op->setCurrentIndex(op->findData(static_cast<int>(e.operation())));
            body->clear();
            const auto* d = design();
            if (d) {
                for (const auto& id : bodiesBefore(*d, e.id())) {
                    body->addItem(QString::fromStdString(d->find(id)->name()),
                                  QString::fromStdString(id.toString()));
                }
            }
            body->setCurrentIndex(body->findData(QString::fromStdString(e.body().toString())));
            body->setEnabled(e.operation() != model::ExtrudeOp::NewBody);
        });
    }

    // The steps before `step` that make a body (any step but a sketch or a plane), in order.
    static std::vector<FeatureId> bodiesBefore(const model::Document& d, const FeatureId& step) {
        std::vector<FeatureId> out;
        for (std::size_t i = 0; i < d.featureCount(); ++i) {
            const auto& f = d.featureAt(i);
            if (f.id() == step) break;
            if (f.type() != model::FeatureType::Sketch && f.type() != model::FeatureType::Plane)
                out.push_back(f.id());
        }
        return out;
    }

    // The material, and what it makes of the body: mass, weight and cost.
    void addMaterial(QFormLayout* form) {
        auto* line = new QFrame(form_);
        line->setFrameShape(QFrame::HLine);
        line->setFrameShadow(QFrame::Sunken);
        form->addRow(line);

        auto* material = new QComboBox(form_);
        material->setObjectName(QStringLiteral("material"));
        material->setToolTip(
            tr("What this step's body is made of. It sets the mass, weight, cost and color."));
        form->addRow(tr("Material"), material);
        auto* info = new QLabel(form_);
        info->setObjectName(QStringLiteral("materialinfo"));
        info->setWordWrap(true);
        info->setStyleSheet(QStringLiteral("color: #57606a"));
        // A form lays out wrapped text at the height of its size hint, which
        // cuts off lines when it's narrower than that (floating over the view,
        // say): keep it as tall as its lines need at the width it has.
        info->installEventFilter(this);
        wrapped_ = info;
        form->addRow(QString(), info);
        auto* volume = new QLabel(form_);
        volume->setObjectName(QStringLiteral("volume"));
        form->addRow(tr("Volume"), volume);
        auto* mass = new QLabel(form_);
        mass->setObjectName(QStringLiteral("mass"));
        form->addRow(tr("Mass"), mass);
        auto* cost = new QLabel(form_);
        cost->setObjectName(QStringLiteral("cost"));
        form->addRow(tr("Material cost"), cost);

        connect(material, &QComboBox::activated, this, [this, material](int index) {
            const auto key = material->itemData(index).toString().toStdString();
            if (key == kNewMaterial) {
                newMaterial();
            } else {
                setMaterial(key);
            }
        });

        updaters_.push_back([this, material, info, volume, mass, cost](const model::Feature& g) {
            const auto* doc = design();
            if (!doc) return;
            // The first item: what the body gets when nothing is picked for it.
            QString inherited = tr("Default: %1").arg(qs(model::defaultMaterial().name));
            if (const auto inputs = g.dependencies(); !inputs.empty()) {
                const auto& a = inputs.front();  // a Boolean's A, a Move's body
                if (const auto* first = doc->find(a)) {
                    inherited = tr("Same as %1: %2").arg(qs(first->name()), qs(doc->materialOf(a).name));
                }
            }
            const QSignalBlocker block(material);
            material->clear();
            material->addItem(inherited, QString());
            material->insertSeparator(material->count());
            for (const auto& m : model::builtinMaterials()) material->addItem(qs(m.name), qs(m.key));
            if (!doc->customMaterials().empty()) {
                material->insertSeparator(material->count());
                for (const auto& m : doc->customMaterials()) material->addItem(qs(m.name), qs(m.key));
            }
            material->insertSeparator(material->count());
            material->addItem(tr("New material..."), QString::fromLatin1(kNewMaterial));
            const int at = material->findData(
                doc->findMaterial(g.material()) ? qs(doc->findMaterial(g.material())->key) : QString());
            material->setCurrentIndex(at < 0 ? 0 : at);

            const auto& m = doc->materialOf(g.id());
            auto known = [](double v, const QString& text) { return v > 0 ? text : tr("unknown"); };
            info->setText(
                tr("%1: density %2 g/cm³, stiffness %3, Poisson's ratio %4, yield strength %5, price %6")
                    .arg(qs(m.name), QString::number(m.density, 'g', 4),
                         known(m.youngsModulus, tr("%1 GPa").arg(m.youngsModulus)),
                         known(m.poissonsRatio, QString::number(m.poissonsRatio)),
                         known(m.yieldStrength, tr("%1 MPa").arg(m.yieldStrength)),
                         known(m.costPerKg, tr("%1 per kg").arg(money(m.costPerKg)))));

            const auto* report = ctx_.documents().lastRebuild(ctx_.selection().doc());
            const auto* shape = report ? report->shapeOf(g.id()) : nullptr;
            const auto* kernel = ctx_.documents().kernel();
            if (!shape || !kernel) {
                const QString none = tr("none: this step didn't build");
                volume->setText(none);
                mass->setText(none);
                cost->setText(none);
                return;
            }
            const auto b = model::bodyMass(*kernel, *shape, m);
            // "3748.67 mm³": the design's unit, cubed.
            QString vol = qs(volumeText(b.volume, unit_));
            vol.replace(QStringLiteral("^3"), QStringLiteral("³"));
            volume->setText(vol);
            mass->setText(tr("%1 (weighs %2 N)").arg(massText(b.grams)).arg(b.newtons, 0, 'f', 3));
            cost->setText(m.costPerKg <= 0 ? tr("unknown: the material has no price")
                          : b.cost < 0.005 ? tr("less than $0.01")
                                           : tr("about %1").arg(money(b.cost)));
        });
    }

    void setMaterial(const std::string& key) {
        if (filling_) return;
        const auto* f = selected();
        if (!f || key == f->material()) return;
        auto problems = ctx_.documents().run(ctx_.selection().doc(), "feature.set_material",
                                             {{"feature", f->id()}, {"material", key}});
        if (!problems.empty()) ui_.showProblems(problems);
        schedule();
    }

    void newMaterial() {
        model::Material m;
        if (!askNewMaterial(this, m)) {
            schedule();  // put the combo back
            return;
        }
        auto problems = ctx_.documents().execute(ctx_.selection().doc(),
                                                 std::make_unique<model::DefineMaterialCommand>(m));
        if (!problems.empty()) {
            ui_.showProblems(problems);
            schedule();
            return;
        }
        setMaterial(m.name);
    }

    // The unit the shown design's lengths are in (its Document settings).
    LengthUnit unitNow() const {
        const auto* d = design();
        return d ? d->lengthUnit() : LengthUnit::Millimeter;
    }

    // A box for a length in the design's unit (or, with `length` false, for a
    // plain number, like an axis direction).
    ui::FormulaBox* spin(const QString& name, bool positive, const QString& suffix, bool length = true) {
        auto* s = new ui::FormulaBox(form_);
        s->setObjectName(name);
        const double most = length ? toUnit(10000.0, unit_) : 10000.0;  // 10 m
        s->setDecimals(length ? unitDecimals(unit_) : 2);
        s->setRange(positive ? std::pow(10.0, -s->decimals()) : -most, most);
        s->setSuffix(suffix);
        s->setKeyboardTracking(false);  // edit when Enter is pressed or focus leaves, not per key
        s->setAccelerated(true);
        return s;
    }

    // A length in the design's unit. `positive`: more than 0; otherwise 0 and
    // less are fine too (like how far a plane is moved).
    void addNumber(QFormLayout* form, const QString& key, const QString& label,
                   std::function<double(const model::Feature&)> get,
                   std::function<void(model::Feature&, double)> set, bool positive = true) {
        auto* s = spin(key, positive, QLatin1Char(' ') + qs(std::string(unitSymbol(unit_))));
        form->addRow(label, s);
        connect(s, &QDoubleSpinBox::valueChanged, this, [this, key, set](double v) {
            const double mm = fromUnit(v, unit_);
            edit(key, [set, mm](model::Feature& g) { set(g, mm); });
        });
        updaters_.push_back([this, s, get, key](const model::Feature& g) {
            s->setValue(toUnit(get(g), unit_));
            s->setToolTip(formulaTip(g, key));
        });
    }

    // The key of the step's number a box shows, like "size_x" for the box "size.x".
    static std::string numberKey(const QObject& box) {
        QString name = box.objectName();
        if (name == QStringLiteral("turn")) return "angle";  // a Move's angle
        return name.replace(QLatin1Char('.'), QLatin1Char('_')).toStdString();
    }

    // Lets every box that shows one of the step's numbers take a formula, and
    // marks the ones that have one.
    void showFormulas(const model::Feature& f) {
        const auto numbers = f.numbers();
        for (auto* spinBox : form_->findChildren<QDoubleSpinBox*>()) {
            auto* box = dynamic_cast<ui::FormulaBox*>(spinBox);
            if (!box) continue;
            const std::string key = numberKey(*box);
            const bool number = std::any_of(numbers.begin(), numbers.end(),
                                            [&](const model::StepNumber& n) { return n.key == key; });
            if (!number) {
                box->setFormulaHandler(nullptr);
                box->setFormula(QString());
                continue;
            }
            box->setFormula(qs(f.formula(key)));
            if (!box->takesFormulas())
                box->setFormulaHandler([this, key](const QString& text) { typedFormula(key, text); });
        }
    }

    // A formula typed into a number's box. One with no parameters in it, like
    // "1 in" or "25 mm", is just a number in another unit; any other becomes the
    // number's formula.
    void typedFormula(const std::string& key, const QString& text) {
        const auto* f = selected();
        const auto* d = design();
        if (!f || !d) return;
        const std::string formula = text.toStdString();
        model::Args args{{"feature", f->id()}, {"setting", key}};
        std::optional<double> typed;
        if (model::Expression::parse(formula).isConstant()) {
            for (const auto& n : f->numbers()) {
                if (n.key != key) continue;
                const auto r = model::evalNumber(n.dim, n.positive, formula, d->parameters().evaluate(),
                                                 key + " in " + f->name());
                // feature.set_value takes angles in degrees.
                if (r.ok())
                    typed =
                        n.dim == model::kAngle ? r.value->value * 180.0 / std::numbers::pi : r.value->value;
            }
        }
        if (typed) {
            args["value"] = *typed;
        } else {
            args["formula"] = formula;
        }
        lastField_.clear();  // not part of a run of arrow-key changes
        const auto problems = ctx_.documents().run(ctx_.selection().doc(), "feature.set_value", args);
        if (!problems.empty()) ui_.showProblems(problems);
        schedule();
    }

    // Says where a number comes from, if it's worked out from a formula.
    // @param key The box's name, like "distance" or "size.x" (the number's key is "size_x").
    QString formulaTip(const model::Feature& g, QString key) const {
        const auto text = g.formula(key.replace(QLatin1Char('.'), QLatin1Char('_')).toStdString());
        if (text.empty()) return {};
        return tr("Worked out from %1. Type a new formula to change it, or a number to use that instead.")
            .arg(qs(text));
    }

    void addVec3(QFormLayout* form, const QString& key, const QString& label, bool positive,
                 std::function<kernel::Vec3(const model::Feature&)> get,
                 std::function<void(model::Feature&, kernel::Vec3)> set, bool length = true) {
        auto* row = new QWidget(form_);
        auto* h = new QHBoxLayout(row);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(4);
        QDoubleSpinBox* xyz[3];
        const char* axes[3] = {"x", "y", "z"};
        for (int i = 0; i < 3; ++i) {
            // Compact: "x 20.00", no arrows (type, or scroll the mouse wheel); the
            // unit is in the row's label.
            xyz[i] = spin(key + QLatin1Char('.') + QLatin1String(axes[i]), positive, QString(), length);
            xyz[i]->setPrefix(QLatin1String(axes[i]) + QLatin1Char(' '));
            xyz[i]->setButtonSymbols(QAbstractSpinBox::NoButtons);
            xyz[i]->setMinimumWidth(72);
            h->addWidget(xyz[i]);
        }
        form->addRow(length ? tr("%1 (%2)").arg(label, qs(std::string(unitSymbol(unit_)))) : label, row);
        for (int i = 0; i < 3; ++i) {
            connect(xyz[i], &QDoubleSpinBox::valueChanged, this,
                    [this, key, i, get, set, length](double shown) {
                        const QString part = key + QLatin1Char('.') + QString::number(i);
                        const double v = length ? fromUnit(shown, unit_) : shown;
                        edit(part, [i, v, get, set](model::Feature& g) {
                            kernel::Vec3 p = get(g);
                            (i == 0 ? p.x : i == 1 ? p.y : p.z) = v;
                            set(g, p);
                        });
                    });
        }
        updaters_.push_back([this, xyz, get, length](const model::Feature& g) {
            const auto p = get(g);
            auto shown = [&](double v) { return length ? toUnit(v, unit_) : v; };
            xyz[0]->setValue(shown(p.x));
            xyz[1]->setValue(shown(p.y));
            xyz[2]->setValue(shown(p.z));
            for (auto* box : xyz) box->setToolTip(formulaTip(g, box->objectName()));
        });
    }

    // Edits the shown step through a command. Quick repeated changes to the same
    // number (holding an arrow key) become one undo step.
    void edit(const QString& field, const Change& change) {
        if (filling_) return;
        const auto* f = selected();
        if (!f) return;
        auto copy = f->clone();
        change(*copy);
        const bool merge = field == lastField_ && lastEdit_.isValid() && lastEdit_.elapsed() < kMergeMs;
        lastField_ = field;
        lastEdit_.start();
        auto problems = ctx_.documents().execute(
            ctx_.selection().doc(), std::make_unique<model::EditFeatureCommand>(std::move(copy), merge));
        if (!problems.empty()) {
            ui_.showProblems(problems);
            schedule();  // show the value that is really in the design
        }
    }

    void rename(const QString& text) {
        if (filling_) return;
        const auto* f = selected();
        const auto name = text.trimmed().toStdString();
        if (!f || name == f->name()) return;
        auto problems = ctx_.documents().run(ctx_.selection().doc(), "feature.rename",
                                             {{"feature", f->id()}, {"name", name}});
        if (!problems.empty()) {
            ui_.showProblems(problems);
            schedule();
        }
    }

    QLabel* wrapped_ = nullptr;  // the material's description, kept as tall as its lines
    plugin::PluginContext& ctx_;
    ui::UiRegistry& ui_;
    QLabel* title_ = nullptr;
    QWidget* changing_ = nullptr;
    QPushButton* done_ = nullptr;
    FeatureId changingStep_{};
    DocId changingDoc_{};
    QWidget* form_ = nullptr;
    QLabel* status_ = nullptr;
    FeatureId shown_;
    LengthUnit unit_ = LengthUnit::Millimeter;  // the unit the form was built for
    std::vector<std::function<void(const model::Feature&)>> updaters_;
    bool filling_ = false;
    bool pending_ = false;
    QString lastField_;
    QElapsedTimer lastEdit_;
    std::vector<plugin::Subscription> subs_;
};

void registerProperties(plugin::PluginContext& ctx) {
    auto* ui = ctx.service<ui::UiRegistry>();
    if (!ui) {
        ctx.log("No desktop app here, so the Properties panel isn't shown.");
        return;
    }
    // The panel, once made, for Modify > Change edges.
    auto panel = std::make_shared<QPointer<Properties>>();
    ui::PanelSpec spec{"sb.properties.panel", QObject::tr("Properties"), ui::PanelArea::Right,
                       [&ctx, ui, panel] {
                           auto* p = new Properties(ctx, *ui);
                           *panel = p;
                           return p;
                       }};
    spec.floatsOverView = true;  // at the view's top right
    ui->addPanel(std::move(spec));
    ctx.addAction(plugin::ActionSpec{.id = "sb.properties.change_edges",
                                     .text = "Change edges",
                                     .menu = "Modify",
                                     .tip = "Pick the edges of the picked Fillet or Chamfer again, for when "
                                            "an earlier change took them away.",
                                     .run =
                                         [panel] {
                                             if (*panel) (*panel)->changePicked();
                                         },
                                     .enabled = [panel] { return *panel && (*panel)->canChange(); }});
}

}  // namespace

plugin::BuiltinPlugin propertiesPlugin() {
    return {kManifest, &registerProperties};
}

}  // namespace sb::plugins
