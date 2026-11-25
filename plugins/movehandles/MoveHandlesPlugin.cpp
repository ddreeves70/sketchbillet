#include "sb/plugins/MoveHandles.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <numbers>
#include <optional>
#include <string>

#include "sb/kernel/Placement.h"
#include "sb/model/AssemblyCommands.h"
#include "sb/model/Commands.h"
#include "sb/model/Document.h"
#include "sb/model/JointDrag.h"
#include "sb/plugin/Events.h"

namespace sb::plugins {

namespace {

constexpr const char* kManifest = R"({
  "id": "sb.movehandles",
  "name": "Move handles",
  "version": "0.1.0",
  "description": "Arrows on the picked body in the 3D view: drag one to slide the body along x, y or z, or the square to slide it on the ground. A component on a hinge gets a ring to turn it, and one on a slider an arrow to slide it.",
  "requires": ["sb.modeling", "sb.viewer"],
  "tier": "ui"
})";

// Puts the handles on the picked body or component. Dragging them shows it moved
// (a preview: instant, however big it is); dropping moves it, as one step. A
// component a revolute or slider joint places gets one handle, on the joint: a
// ring that turns it, or an arrow that slides it; dropping sets the joint's value.
class Handles final : public viewer::IHandleUser {
public:
    Handles(plugin::PluginContext& ctx, viewer::IViewer& viewer) : ctx_(ctx), viewer_(viewer) {}

    // No clearHandles() when it goes: that's only when SketchBillet closes, and the
    // viewer may already be gone then.

    // Shows handles on the picked body or free component, or on the joint that
    // turns or slides the picked component (or the one being dragged, where it's
    // drawn), or none.
    void refresh() {
        const DocId docId = ctx_.documents().active();
        const auto* doc = ctx_.documents().find(docId);
        const auto* report = ctx_.documents().lastRebuild(docId);
        const auto* kernel = ctx_.documents().kernel();
        // Shown exploded, the components aren't drawn where they are: no handles.
        if (doc && ctx_.selection().explodedDoc() == docId) {
            viewer_.clearHandles(*this);
            return;
        }
        if (doc && kernel && report && doc->kind() == model::DocKind::Assembly) {
            // A component: a grounded one stays where it is, and a joint places a
            // joined one, so only a revolute or slider joint's handle moves it.
            const model::Component* c = nullptr;
            if (dragging_ && !component_.isNil()) {
                c = doc->findComponent(component_);
            } else if (!dragging_ && ctx_.selection().doc() == docId &&
                       ctx_.selection().components().size() == 1) {
                c = doc->findComponent(ctx_.selection().components().front());
                if (c && c->grounded) c = nullptr;
            }
            // On a revolute or slider joint: its one handle.
            if (!dragging_) {
                const JointId* by = c ? report->placingJoint(c->id) : nullptr;
                joint_ = by && model::dragJoint(*doc, *report, *by, 0) ? *by : JointId{};
            }
            if (c && !dragging_ && report->placedByJoint(c->id)) c = nullptr;
            if (const auto along = jointHandle(*doc, *report, joint_)) {
                viewer_.setHandles(*this, {*along});
                return;
            }
            const auto box = c ? componentBounds(*report, *kernel, *c) : std::nullopt;
            if (!box) {
                viewer_.clearHandles(*this);
                return;
            }
            viewer_.setHandles(*this, moveHandles(shifted(*box)));
            return;
        }
        if (!dragging_) joint_ = {};
        std::optional<FeatureId> body;
        if (dragging_) {
            body = target_;
        } else if (doc && ctx_.selection().doc() == docId && ctx_.selection().features().size() == 1) {
            body = ctx_.selection().features().front();
        }
        const kernel::ShapeHandle* shape = body && report ? report->shapeOf(*body) : nullptr;
        if (!doc || !kernel || !shape ||
            (!dragging_ && !model::SlideBodyCommand::check(*doc, *body).empty())) {
            viewer_.clearHandles(*this);
            return;
        }
        viewer_.setHandles(*this, moveHandles(shifted(kernel->bounds(*shape))));
    }

    void dragged(const std::string&, const kernel::Vec3& by) override {
        const auto* doc = ctx_.documents().find(ctx_.documents().active());
        if (!doc) return;
        if (!dragging_) {
            // The drag starts: it slides the picked component, or body.
            const auto& sel = ctx_.selection();
            component_ = {};
            if (doc->kind() == model::DocKind::Assembly) {
                if (sel.components().size() != 1) return;
                component_ = sel.components().front();
                if (!joint_.isNil()) {
                    const auto* j = doc->findJoint(joint_);
                    if (!j) return;
                    startValue_ = shownValue_ = j->value;
                }
            } else {
                if (sel.features().size() != 1) return;
                target_ = sel.features().front();
            }
            dragging_ = true;
            shown_ = {};
        }
        if (!joint_.isNil()) {
            dragAlongJoint(*doc, by);
            return;
        }
        const auto snapped = snapSlide(by, doc->lengthUnit());
        if (snapped.x == shown_.x && snapped.y == shown_.y && snapped.z == shown_.z) return;
        // Only the drawing moves while dragging; the design changes once, on dropping.
        shown_ = snapped;
        if (!component_.isNil())
            viewer_.previewComponent(component_, kernel::Motion{shown_});
        else
            viewer_.previewMove(target_, kernel::Motion{shown_});
        refresh();  // the handles follow the body
    }

    void dropped(const std::string&, const kernel::Vec3&) override {
        if (!dragging_) return;
        const auto slide = shown_;
        dragging_ = false;
        shown_ = {};
        viewer_.clearPreview();  // before the design moves it, so it isn't moved twice
        const DocId docId = ctx_.documents().active();
        if (!joint_.isNil()) {
            // Along a joint: its value changes, as one step.
            const auto* doc = ctx_.documents().find(docId);
            const auto* j = doc ? doc->findJoint(joint_) : nullptr;
            if (j && shownValue_ != startValue_) {
                model::Joint changed = *j;
                changed.value = shownValue_;
                auto problems =
                    ctx_.documents().execute(docId, std::make_unique<model::SetJointCommand>(changed));
                if (!problems.empty()) ctx_.log(problems.front().message);
            }
            moved_ = {};
            component_ = {};
            refresh();
            return;
        }
        const bool moved = slide.x != 0 || slide.y != 0 || slide.z != 0;
        if (!component_.isNil()) {
            // A component: placed further along, as one step.
            const auto* doc = ctx_.documents().find(docId);
            const auto* c = doc ? doc->findComponent(component_) : nullptr;
            if (c && moved) {
                auto placement = c->placement;
                placement.origin = {placement.origin.x + slide.x, placement.origin.y + slide.y,
                                    placement.origin.z + slide.z};
                auto problems = ctx_.documents().execute(
                    docId, std::make_unique<model::PlaceComponentCommand>(component_, placement));
                if (!problems.empty()) ctx_.log(problems.front().message);
            }
            component_ = {};
            refresh();
            return;
        }
        if (ctx_.documents().find(docId) && moved) {
            const FeatureId moveId = FeatureId::generate();
            auto problems = ctx_.documents().execute(
                docId, std::make_unique<model::SlideBodyCommand>(target_, slide, false, moveId));
            if (!problems.empty()) {
                ctx_.log(problems.front().message);
            } else if (const auto* doc = ctx_.documents().find(docId); doc && doc->find(moveId)) {
                // It added a Move step: that's the body now, so it's what's picked.
                ctx_.selection().set(docId, {moveId});
            }
        }
        refresh();
    }

    void cancelled() override {
        // Escape: the design never changed, so only the drawing goes back.
        dragging_ = false;
        shown_ = {};
        moved_ = {};
        component_ = {};
        viewer_.clearPreview();
        refresh();
    }

private:
    // A drag of the joint's handle: the joint's value changes by how far it was
    // turned (radians) or slid (mm), in whole degrees or steps, within its limits,
    // and what it carries is drawn moved.
    void dragAlongJoint(const model::Document& doc, const kernel::Vec3& by) {
        const auto* report = ctx_.documents().lastRebuild(ctx_.documents().active());
        const auto* j = doc.findJoint(joint_);
        const auto* r = report ? report->joint(joint_) : nullptr;
        if (!j || !r || !r->aFrame) return;
        const double sign = movesB(*report) ? 1 : -1;
        double value = startValue_;
        if (j->kind == model::JointKind::Revolute) {
            constexpr double degree = std::numbers::pi / 180;
            value = std::round((startValue_ + sign * by.x) / degree) * degree;
        } else {
            const auto& z = r->aFrame->zAxis;
            const double along = by.x * z.x + by.y * z.y + by.z * z.z;
            value = snapSlide({startValue_ + sign * along, 0, 0}, doc.lengthUnit()).x;
        }
        const auto drag = model::dragJoint(doc, *report, joint_, value);
        if (!drag || drag->value == shownValue_) return;
        shownValue_ = drag->value;
        moved_ = drag->motion;
        viewer_.previewComponents(drag->components, drag->motion);
        refresh();  // the handle follows
    }

    // Whether the joint placed its b side (which moves with its value), or its a side.
    bool movesB(const model::RebuildReport& report) const {
        const auto* doc = ctx_.documents().find(ctx_.documents().active());
        const auto* j = doc ? doc->findJoint(joint_) : nullptr;
        const JointId* by = j ? report.placingJoint(j->b.component) : nullptr;
        return by && *by == joint_;
    }

    // The one handle of a component on a revolute or slider joint: on the origin
    // of the side that moves (moved as the preview draws it), a ring about the
    // joint's line, or an arrow along it.
    std::optional<viewer::Handle> jointHandle(const model::Document& doc, const model::RebuildReport& report,
                                              const JointId& id) const {
        const auto* j = id.isNil() ? nullptr : doc.findJoint(id);
        const auto* r = j ? report.joint(id) : nullptr;
        if (!r || !r->aFrame || !r->bFrame) return std::nullopt;
        const auto& origin = movesB(report) ? r->bFrame->origin : r->aFrame->origin;
        const auto at = kernel::apply(kernel::fromMotion(moved_), origin);
        const auto& z = r->aFrame->zAxis;
        if (j->kind == model::JointKind::Revolute)
            return viewer::Handle{"turn", viewer::HandleKind::Ring, at, z, kRingPixels, 150, 60, 220};
        return viewer::Handle{"slide", viewer::HandleKind::Arrow, at, z, kArrowPixels, 150, 60, 220};
    }

    // A box moved to where the preview draws it.
    kernel::Box3 shifted(kernel::Box3 box) const {
        for (auto* p : {&box.min, &box.max}) {
            p->x += shown_.x;
            p->y += shown_.y;
            p->z += shown_.z;
        }
        return box;
    }

    // Around a component's bodies, where it's placed: the box around each body's
    // own box, turned and moved by the placement.
    static std::optional<kernel::Box3> componentBounds(const model::RebuildReport& report,
                                                       const kernel::IKernel& kernel,
                                                       const model::Component& c) {
        const auto* built = report.part(c.part);
        if (!built) return std::nullopt;
        std::optional<kernel::Box3> out;
        for (const auto& body : built->bodies()) {
            const auto* shape = built->shapeOf(body);
            if (!shape) continue;
            const auto b = kernel.bounds(*shape);
            const auto* placed = report.placementOf(c.id);
            for (int corner = 0; corner < 8; ++corner) {
                const auto p = kernel::apply(
                    placed ? *placed : c.placement,
                    kernel::Point3{corner & 1 ? b.max.x : b.min.x, corner & 2 ? b.max.y : b.min.y,
                                   corner & 4 ? b.max.z : b.min.z});
                if (!out) out = kernel::Box3{p, p};
                out->min = {std::min(out->min.x, p.x), std::min(out->min.y, p.y), std::min(out->min.z, p.z)};
                out->max = {std::max(out->max.x, p.x), std::max(out->max.y, p.y), std::max(out->max.z, p.z)};
            }
        }
        return out;
    }

    plugin::PluginContext& ctx_;
    viewer::IViewer& viewer_;
    bool dragging_ = false;   // a drag has started
    FeatureId target_;        // the step being slid
    InstanceId component_;    // or, in an assembly, the component
    kernel::Vec3 shown_{};    // how far the preview draws it, snapped (mm)
    JointId joint_;           // the joint the picked component turns or slides on, if any
    double startValue_ = 0;   // its value when the drag started (radians or mm)
    double shownValue_ = 0;   // and as the preview draws it
    kernel::Motion moved_{};  // how the preview moves what it carries
};

void registerMoveHandles(plugin::PluginContext& ctx) {
    auto* viewer = ctx.service<viewer::IViewer>();
    if (!viewer) {
        ctx.fail("Move handles need the 3D viewer (sb.viewer). Reinstall SketchBillet to get it back.");
        return;
    }
    // Lives as long as the plugin: the listeners below hold it.
    auto handles = std::make_shared<Handles>(ctx, *viewer);
    auto refresh = [handles](const auto&) { handles->refresh(); };
    ctx.listen<plugin::SelectionChanged>(refresh);
    ctx.listen<plugin::RebuildFinished>(refresh);
    ctx.listen<plugin::ActiveDocumentChanged>(refresh);
    ctx.listen<plugin::DocumentClosed>(refresh);
    ctx.listen<plugin::ExplodedViewShown>(refresh);
}

}  // namespace

std::vector<viewer::Handle> moveHandles(const kernel::Box3& b) {
    const kernel::Vec3 middle{(b.min.x + b.max.x) / 2, (b.min.y + b.max.y) / 2, (b.min.z + b.max.z) / 2};
    std::vector<viewer::Handle> out;
    out.push_back({"x", viewer::HandleKind::Arrow, middle, {1, 0, 0}, kArrowPixels, 220, 50, 50});
    out.push_back({"y", viewer::HandleKind::Arrow, middle, {0, 1, 0}, kArrowPixels, 40, 170, 60});
    out.push_back({"z", viewer::HandleKind::Arrow, middle, {0, 0, 1}, kArrowPixels, 40, 100, 230});
    out.push_back({"xy", viewer::HandleKind::Square, middle, {0, 0, 1}, kSquarePixels, 190, 110, 220});
    return out;
}

kernel::Vec3 snapSlide(const kernel::Vec3& by, LengthUnit unit) {
    const double step = (unit == LengthUnit::Inch || unit == LengthUnit::Foot) ? 2.54 : 1.0;
    auto snap = [step](double v) {
        const double s = std::round(v / step) * step;
        return s == 0 ? 0.0 : s;  // never -0
    };
    return {snap(by.x), snap(by.y), snap(by.z)};
}

plugin::BuiltinPlugin moveHandlesPlugin() {
    return {kManifest, &registerMoveHandles};
}

}  // namespace sb::plugins
