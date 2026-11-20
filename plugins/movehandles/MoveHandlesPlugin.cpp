#include "sb/plugins/MoveHandles.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>

#include "sb/model/Commands.h"
#include "sb/plugin/Events.h"

namespace sb::plugins {

namespace {

constexpr const char* kManifest = R"({
  "id": "sb.movehandles",
  "name": "Move handles",
  "version": "0.1.0",
  "description": "Arrows on the picked body in the 3D view: drag one to slide the body along x, y or z, or the square to slide it on the ground.",
  "requires": ["sb.modeling", "sb.viewer"],
  "tier": "ui"
})";

// Puts the handles on the picked body. Dragging them shows the body moved (a
// preview: instant, however big the body is); dropping slides it, as one step.
class Handles final : public viewer::IHandleUser {
public:
    Handles(plugin::PluginContext& ctx, viewer::IViewer& viewer) : ctx_(ctx), viewer_(viewer) {}

    // No clearHandles() when it goes: that's only when SketchBillet closes, and the
    // viewer may already be gone then.

    // Shows handles on the picked body (or the body being dragged, where it's
    // drawn), or none.
    void refresh() {
        const DocId docId = ctx_.documents().active();
        const auto* doc = ctx_.documents().find(docId);
        const auto* report = ctx_.documents().lastRebuild(docId);
        const auto* kernel = ctx_.documents().kernel();
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
        auto box = kernel->bounds(*shape);
        for (auto* p : {&box.min, &box.max}) {  // where the preview draws it
            p->x += shown_.x;
            p->y += shown_.y;
            p->z += shown_.z;
        }
        viewer_.setHandles(*this, moveHandles(box));
    }

    void dragged(const std::string&, const kernel::Vec3& by) override {
        const auto* doc = ctx_.documents().find(ctx_.documents().active());
        if (!doc) return;
        if (!dragging_) {
            // The drag starts: it slides the picked body.
            if (ctx_.selection().features().size() != 1) return;
            dragging_ = true;
            target_ = ctx_.selection().features().front();
            shown_ = {};
        }
        const auto snapped = snapSlide(by, doc->lengthUnit());
        if (snapped.x == shown_.x && snapped.y == shown_.y && snapped.z == shown_.z) return;
        // Only the drawing moves while dragging; the design changes once, on dropping.
        shown_ = snapped;
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
        if (ctx_.documents().find(docId) && (slide.x != 0 || slide.y != 0 || slide.z != 0)) {
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
        viewer_.clearPreview();
        refresh();
    }

private:
    plugin::PluginContext& ctx_;
    viewer::IViewer& viewer_;
    bool dragging_ = false;  // a drag has started
    FeatureId target_;       // the step being slid
    kernel::Vec3 shown_{};   // how far the preview draws it, snapped (mm)
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
