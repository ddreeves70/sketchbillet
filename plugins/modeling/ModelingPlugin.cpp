#include "sb/plugins/Modeling.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>

#include "ModelingIcons.h"
#include "sb/io/PartCommands.h"
#include "sb/kernel/Placement.h"
#include "sb/kernel/Types.h"
#include "sb/model/AssemblyCommands.h"
#include "sb/model/CommandRegistry.h"
#include "sb/plugin/Actions.h"
#include "sb/plugin/Events.h"
#include "sb/plugin/Overlays.h"

// sb.modeling adds SketchBillet's modeling commands through the same PluginContext an
// outside plugin uses. The commands themselves are still defined in sb_model
// (CommandRegistry::builtin()); they move here when feature types become an
// extension point, because saving and opening them lives in sb_io for now.

namespace sb::plugins {

namespace {

constexpr const char* kManifest = R"({
  "id": "sb.modeling",
  "name": "Modeling",
  "version": "0.1.0",
  "description": "Boxes, cylinders and Booleans, plus deleting and renaming steps.",
  "tier": "foundation"
})";

// Draws the active design's construction planes (Plane steps): a square on each,
// round the point of it nearest the middle of the design's bodies and big enough
// to show beside them, with its name; the picked ones in orange.
void drawPlanes(plugin::PluginContext& ctx) {
    plugin::Overlay plain, picked, names;
    plain.color = {183, 121, 31, 200};
    picked.color = {230, 120, 20, 255};
    picked.size = 2.5;
    names.shape = plugin::OverlayShape::Labels;
    names.color = {140, 90, 20, 255};
    names.size = 12;
    const auto& docs = ctx.documents();
    const DocId active = docs.active();
    const auto* design = docs.find(active);
    const auto* report = design ? docs.lastRebuild(active) : nullptr;
    if (design && report) {
        // The middle of the bodies, and how big they are.
        kernel::Vec3 middle{};
        double half = 30;  // mm
        if (const auto* k = docs.kernel()) {
            bool any = false;
            kernel::Vec3 lo{}, hi{};
            for (const auto& id : report->bodies()) {
                const auto* shape = report->shapeOf(id);
                if (!shape) continue;
                const auto b = k->bounds(*shape);
                if (!any) {
                    lo = b.min;
                    hi = b.max;
                    any = true;
                }
                lo = {std::min(lo.x, b.min.x), std::min(lo.y, b.min.y), std::min(lo.z, b.min.z)};
                hi = {std::max(hi.x, b.max.x), std::max(hi.y, b.max.y), std::max(hi.z, b.max.z)};
            }
            if (any) {
                middle = {(lo.x + hi.x) / 2, (lo.y + hi.y) / 2, (lo.z + hi.z) / 2};
                const double dx = hi.x - lo.x, dy = hi.y - lo.y, dz = hi.z - lo.z;
                half = std::max(half, 0.4 * std::sqrt(dx * dx + dy * dy + dz * dz));
            }
        }
        const auto& sel = ctx.selection();
        for (std::size_t i = 0; i < design->featureCount(); ++i) {
            const auto& f = design->featureAt(i);
            const auto* r = report->result(f.id());
            if (f.type() != model::FeatureType::Plane || !r || !r->plane) continue;
            const auto& a = *r->plane;
            // The point of the plane nearest the middle, in the plane's own x and y.
            const kernel::Vec3 d{middle.x - a.origin.x, middle.y - a.origin.y, middle.z - a.origin.z};
            const double cu = d.x * a.x.x + d.y * a.x.y + d.z * a.x.z;
            const double cv = d.x * a.y.x + d.y * a.y.y + d.z * a.y.z;
            auto at = [&a, cu, cv](double u, double v) {
                u += cu;
                v += cv;
                return kernel::Vec3{a.origin.x + u * a.x.x + v * a.y.x, a.origin.y + u * a.x.y + v * a.y.y,
                                    a.origin.z + u * a.x.z + v * a.y.z};
            };
            const double kHalf = half;
            const kernel::Vec3 corners[] = {at(-kHalf, -kHalf), at(kHalf, -kHalf), at(kHalf, kHalf),
                                            at(-kHalf, kHalf)};
            const bool isPicked =
                sel.doc() == active &&
                std::find(sel.features().begin(), sel.features().end(), f.id()) != sel.features().end();
            auto& out = isPicked ? picked : plain;
            for (int k = 0; k < 4; ++k) {
                out.points.push_back(corners[k]);
                out.points.push_back(corners[(k + 1) % 4]);
            }
            names.points.push_back(at(-kHalf + 2, kHalf - 2));
            names.texts.push_back(f.name());
        }
    }
    // Only what there is: a design with no planes draws nothing.
    auto put = [&ctx](const char* name, plugin::Overlay o) {
        if (o.points.empty()) {
            ctx.overlays().remove(name);
        } else {
            ctx.overlays().set(name, std::move(o));
        }
    };
    put("planes", std::move(plain));
    put("planes.picked", std::move(picked));
    put("planes.names", std::move(names));
}

// Draws the picked joints of the active assembly: where each one's two sides
// meet, and the line it turns about or slides along (its z), over the bodies.
void drawJoints(plugin::PluginContext& ctx) {
    plugin::Overlay axes, dots;
    axes.color = {20, 150, 70, 255};
    axes.size = 2.5;
    axes.overBodies = true;
    dots.shape = plugin::OverlayShape::Points;
    dots.color = {20, 150, 70, 255};
    dots.size = 9;
    dots.overBodies = true;
    const auto& docs = ctx.documents();
    const DocId active = docs.active();
    const auto* design = docs.find(active);
    const auto* report = design ? docs.lastRebuild(active) : nullptr;
    const auto& sel = ctx.selection();
    if (design && report && design->kind() == model::DocKind::Assembly && sel.doc() == active) {
        for (const auto& id : sel.joints()) {
            const auto* r = report->joint(id);
            if (!r || !r->aFrame) continue;
            const auto& f = *r->aFrame;
            constexpr double kHalf = 20;  // mm each way
            axes.points.push_back({f.origin.x - kHalf * f.zAxis.x, f.origin.y - kHalf * f.zAxis.y,
                                   f.origin.z - kHalf * f.zAxis.z});
            axes.points.push_back({f.origin.x + kHalf * f.zAxis.x, f.origin.y + kHalf * f.zAxis.y,
                                   f.origin.z + kHalf * f.zAxis.z});
            dots.points.push_back(f.origin);
            if (r->bFrame) dots.points.push_back(r->bFrame->origin);
        }
    }
    auto put = [&ctx](const char* name, plugin::Overlay o) {
        if (o.points.empty()) {
            ctx.overlays().remove(name);
        } else {
            ctx.overlays().set(name, std::move(o));
        }
    };
    put("joints.axes", std::move(axes));
    put("joints.points", std::move(dots));
}

// The toolbar mode while an assembly is the design being worked on: the
// Assemble tab shows instead of the Model tab.
constexpr const char* kAssemblyMode = "sb.modeling.assembly";

// The Assemble tab: putting parts in an assembly, its components, and its parts'
// files. Each tool is a command, like every other.
// The shown exploded view's thin lines: from where each component it moves was,
// through where each step put it, to where it's drawn. From the middle of the
// box around its bodies.
void drawExplodeLines(plugin::PluginContext& ctx) {
    plugin::Overlay lines;
    lines.color = {90, 96, 106, 255};
    lines.size = 1.2;
    lines.overBodies = false;
    auto& docs = ctx.documents();
    const auto& sel = ctx.selection();
    const auto* design = docs.find(sel.explodedDoc());
    const auto* view = design ? design->findExplodedView(sel.explodedView()) : nullptr;
    const auto* report = view ? docs.lastRebuild(sel.explodedDoc()) : nullptr;
    const auto* kernel = docs.kernel();
    if (view && report && kernel && sel.explodedDoc() == docs.active()) {
        for (const auto& c : design->components()) {
            const auto path = model::explodePath(*view, c.id);
            const auto* built = report->part(c.part);
            if (path.size() < 2 || !built) continue;
            bool any = false;
            kernel::Box3 box{};
            for (const auto& body : built->bodies()) {
                const auto* shape = built->shapeOf(body);
                if (!shape) continue;
                const auto b = kernel->bounds(*shape);
                if (!any) box = b;
                box.min = {std::min(box.min.x, b.min.x), std::min(box.min.y, b.min.y),
                           std::min(box.min.z, b.min.z)};
                box.max = {std::max(box.max.x, b.max.x), std::max(box.max.y, b.max.y),
                           std::max(box.max.z, b.max.z)};
                any = true;
            }
            if (!any) continue;
            const auto* at = report->placementOf(c.id);
            const auto middle =
                kernel::apply(at ? *at : c.placement,
                              kernel::Point3{(box.min.x + box.max.x) / 2, (box.min.y + box.max.y) / 2,
                                             (box.min.z + box.max.z) / 2});
            for (std::size_t i = 0; i + 1 < path.size(); ++i) {
                lines.points.push_back({middle.x + path[i].x, middle.y + path[i].y, middle.z + path[i].z});
                lines.points.push_back(
                    {middle.x + path[i + 1].x, middle.y + path[i + 1].y, middle.z + path[i + 1].z});
            }
        }
    }
    lines.visible = !lines.points.empty();
    ctx.overlays().set("explode.lines", lines);
}

// Explode on the Assemble tab: shows the assembly pulled apart in an exploded
// view, and adds steps that slide the picked components.
void addExplodeTools(plugin::PluginContext& ctx) {
    using plugin::ActionSpec;
    auto assembly = [&ctx]() -> const model::Document* {
        const auto* doc = ctx.documents().find(ctx.documents().active());
        return doc && doc->kind() == model::DocKind::Assembly ? doc : nullptr;
    };
    // The exploded view the active assembly is shown in, if it is.
    auto shown = [&ctx, assembly]() -> const model::ExplodedView* {
        const auto* doc = assembly();
        const auto& sel = ctx.selection();
        return doc && sel.explodedDoc() == ctx.documents().active()
                   ? doc->findExplodedView(sel.explodedView())
                   : nullptr;
    };
    // Shows the assembly in its first exploded view, making one if it has none.
    auto showOne = [&ctx, assembly]() -> const model::ExplodedView* {
        const auto* doc = assembly();
        if (!doc) return nullptr;
        const DocId id = ctx.documents().active();
        if (doc->explodedViews().empty()) {
            auto problems = ctx.documents().execute(
                id, std::make_unique<model::AddExplodedViewCommand>(model::ExplodedView{}));
            if (!problems.empty()) {
                ctx.log(problems.front().message);
                return nullptr;
            }
        }
        const auto* view = &doc->explodedViews().front();
        ctx.selection().showExploded(id, view->id);
        return view;
    };
    ctx.addAction(ActionSpec{
        .id = "sb.modeling.explode",
        .text = "Explode",
        .menu = "Assemble/Explode",
        .toolbar = true,
        .buttonText = "Explode",
        .icon = std::string(icons::modeling::explode_svg),
        .tip =
            "Pull the components apart, to show how they go together. It never moves them really. Click it "
            "again to put them back together.",
        .run =
            [&ctx, shown, showOne] {
                if (shown()) {
                    ctx.selection().showExploded({}, {});
                    return;
                }
                if (showOne()) ctx.selection().clear();  // Properties shows the view
            },
        .checked = [shown] { return shown() != nullptr; },
        .enabled = [assembly] { return assembly() != nullptr; }});
    ctx.addAction(ActionSpec{
        .id = "sb.modeling.explode_step",
        .text = "Add explode step",
        .menu = "Assemble/Explode",
        .toolbar = true,
        .buttonText = "Add step",
        .icon = std::string(icons::modeling::explode_step_svg),
        .tip = "Pick the components to slide out, then click this: they slide 20 mm up. Change which way and "
               "how far in Properties.",
        .run =
            [&ctx, shown, showOne] {
                const auto components = ctx.selection().components();
                if (components.empty()) return;
                const auto* view = shown();
                if (!view) view = showOne();
                if (!view) return;
                model::ExplodedView changed = *view;
                changed.steps.push_back({{}, components, {0, 0, 1}, 20});
                const DocId id = ctx.documents().active();
                auto problems = ctx.documents().execute(
                    id, std::make_unique<model::SetExplodedViewCommand>(std::move(changed)));
                if (!problems.empty()) {
                    ctx.log(problems.front().message);
                    return;
                }
                ctx.selection().clear();  // Properties shows the view, at its new step
            },
        .enabled = [&ctx, assembly] { return assembly() && !ctx.selection().components().empty(); }});
    ctx.listen<plugin::ExplodedViewShown>([&ctx](const auto&) {
        drawExplodeLines(ctx);
        ctx.publish(plugin::ActionsChanged{});  // Explode's check mark
    });
}

void addAssembleTools(plugin::PluginContext& ctx) {
    using plugin::ActionSpec;
    ctx.addTab({.name = "Assemble", .always = false, .mode = kAssemblyMode});
    // The Assemble tab takes the Model tab's place while an assembly is open
    // (unless another mode, like editing a sketch, is on).
    auto followDesign = [&ctx] {
        const auto* doc = ctx.documents().find(ctx.documents().active());
        const bool assembly = doc && doc->kind() == model::DocKind::Assembly;
        const auto& mode = ctx.toolbarMode();
        if (assembly && mode.empty()) ctx.setToolbarMode(kAssemblyMode);
        if (!assembly && mode == kAssemblyMode) ctx.setToolbarMode("");
        // The joint tool only works in an assembly.
        if (!assembly && ctx.selection().pickMode() == plugin::PickMode::Joint)
            ctx.selection().setPickMode(plugin::PickMode::Bodies);
    };
    ctx.listen<plugin::ActiveDocumentChanged>([followDesign](const auto&) { followDesign(); });
    ctx.listen<plugin::DocumentChanged>([followDesign](const auto&) { followDesign(); });
    ctx.onStart(followDesign);

    auto assembly = [&ctx]() -> const model::Document* {
        const auto* doc = ctx.documents().find(ctx.documents().active());
        return doc && doc->kind() == model::DocKind::Assembly ? doc : nullptr;
    };
    // The one picked component, if one is.
    auto picked = [&ctx, assembly]() -> const model::Component* {
        const auto* doc = assembly();
        const auto& sel = ctx.selection();
        if (!doc || sel.doc() != doc->id() || sel.components().size() != 1) return nullptr;
        return doc->findComponent(sel.components().front());
    };
    auto pickedLinked = [assembly, picked](bool linked) {
        return [assembly, picked, linked] {
            const auto* c = picked();
            return c && (assembly()->partLink(c->part) != nullptr) == linked;
        };
    };
    const std::string designs = "SketchBillet designs (*.sb)";

    ctx.addAction(ActionSpec{.id = "sb.modeling.insert_part",
                             .text = "Insert part",
                             .menu = "Assemble/Insert",
                             .command = "component.insert",
                             .fromFile = "file",
                             .fileTypes = designs,
                             .toolbar = true,
                             .icon = std::string(icons::modeling::insert_part_svg),
                             .tip = "Put a part from a .sb file in the assembly. It stays linked: change the "
                                    "file, and the assembly follows.",
                             .enabled = [assembly] { return assembly() != nullptr; }});
    ctx.addAction(ActionSpec{.id = "sb.modeling.insert_copy",
                             .text = "Insert copy",
                             .menu = "Assemble/Insert",
                             .command = "component.insert",
                             .args = {{"link", false}},
                             .fromFile = "file",
                             .fileTypes = designs,
                             .toolbar = true,
                             .icon = std::string(icons::modeling::insert_copy_svg),
                             .tip = "Put a copy of a part from a .sb file in the assembly. The copy doesn't "
                                    "change when the file does.",
                             .enabled = [assembly] { return assembly() != nullptr; }});
    ctx.addAction(ActionSpec{.id = "sb.modeling.new_part",
                             .text = "New part",
                             .menu = "Assemble/Insert",
                             .command = "component.new",
                             .toolbar = true,
                             .icon = std::string(icons::modeling::new_part_svg),
                             .tip = "Add a new, empty part, made inside the assembly.",
                             .enabled = [assembly] { return assembly() != nullptr; }});
    ctx.addAction(ActionSpec{
        .id = "sb.modeling.add_component",
        .text = "Add another",
        .menu = "Assemble/Insert",
        .toolbar = true,
        .icon = std::string(icons::modeling::add_component_svg),
        .tip = "Add another copy of the picked component's part, beside it.",
        .run =
            [&ctx, assembly, picked] {
                const auto* c = picked();
                const auto* doc = assembly();
                const auto* part = c ? doc->findPart(c->part) : nullptr;
                if (!part) return;
                // Beside it: along x, as far as the part is long, and 10 mm more.
                double length = 0;
                const auto* report = ctx.documents().lastRebuild(doc->id());
                const auto* built = report ? report->part(c->part) : nullptr;
                const auto* kernel = ctx.documents().kernel();
                if (built && kernel) {
                    for (const auto& body : built->bodies()) {
                        if (const auto* shape = built->shapeOf(body)) {
                            const auto b = kernel->bounds(*shape);
                            length = std::max(length, b.max.x - b.min.x);
                        }
                    }
                }
                // Facing the same way as the one it's beside, as one step.
                const auto by = kernel::applyDirection(c->placement, {length + 10, 0, 0});
                auto placement = c->placement;
                placement.origin = {placement.origin.x + by.x, placement.origin.y + by.y,
                                    placement.origin.z + by.z};
                const DocId id = doc->id();
                const auto before = doc->components().size();
                auto problems = ctx.documents().execute(id, std::make_unique<model::AddComponentsCommand>(
                                                                part->id(), 1, placement, kernel::Vec3{}));
                const auto* now = ctx.documents().find(id);
                if (problems.empty() && now && now->components().size() > before)
                    ctx.selection().setComponents(id, {now->components().back().id});
            },
        .enabled = [picked] { return picked() != nullptr; }});
    ctx.addAction(ActionSpec{.id = "sb.modeling.ground",
                             .text = "Ground",
                             .menu = "Assemble/Component",
                             .toolbar = true,
                             .icon = std::string(icons::modeling::ground_svg),
                             .tip = "Fix the picked component where it is, or let it go again.",
                             .run =
                                 [&ctx, picked] {
                                     const auto* c = picked();
                                     if (!c) return;
                                     const auto name = c->name;
                                     const bool grounded = !c->grounded;
                                     ctx.documents().run(ctx.documents().active(), "component.ground",
                                                         {{"component", name}, {"grounded", grounded}});
                                 },
                             .checked = [picked] { return picked() && picked()->grounded; },
                             .enabled = [picked] { return picked() != nullptr; }});
    ctx.addAction(ActionSpec{.id = "sb.modeling.delete_component",
                             .text = "Delete component",
                             .menu = "Assemble/Component",
                             .command = "component.delete",
                             .fromComponent = "component",
                             .toolbar = true,
                             .buttonText = "Delete",
                             .icon = std::string(icons::modeling::delete_component_svg),
                             .tip = "Take the picked component out of the assembly."});
    ctx.addAction(ActionSpec{
        .id = "sb.modeling.edit_part",
        .text = "Edit part",
        .menu = "Assemble/Edit",
        .toolbar = true,
        .icon = std::string(icons::modeling::edit_part_svg),
        .tip = "Change the picked component's part here, with the rest of the assembly faded around it. Done "
               "goes back.",
        .run =
            [&ctx, assembly, picked] {
                const auto* c = picked();
                if (!c) return;
                const DocId id = assembly()->id();
                const auto component = c->id;
                ctx.selection().clear();  // the part's steps are picked from here on
                ctx.documents().editPart(id, component);
            },
        .enabled = pickedLinked(false)});
    ctx.addAction(ActionSpec{
        .id = "sb.modeling.open_part",
        .text = "Open part",
        .menu = "Assemble/Edit",
        .toolbar = true,
        .icon = std::string(icons::modeling::open_part_svg),
        .tip = "Open the picked component's part file beside the assembly, to change it there. Save it, and "
               "the assembly uses it.",
        .openFile =
            [assembly, picked] {
                const auto* c = picked();
                const auto* link = c ? assembly()->partLink(c->part) : nullptr;
                return link ? link->path : std::string();
            },
        .enabled = pickedLinked(true)});
    // The joint tool: click a face, edge or point on the component that stays,
    // then one on the component that moves; the joint is made (rigid, to begin
    // with) and picked, so Properties can change its kind.
    ctx.addAction(ActionSpec{
        .id = "sb.modeling.joint",
        .text = "Joint",
        .menu = "Assemble/Joints",
        .toolbar = true,
        .icon = std::string(icons::modeling::joint_svg),
        .tip = "Join two components: click a face or edge on the one that stays, then one on the one that "
               "moves.",
        .run =
            [&ctx] {
                auto& sel = ctx.selection();
                sel.setPickMode(sel.pickMode() == plugin::PickMode::Joint ? plugin::PickMode::Bodies
                                                                          : plugin::PickMode::Joint);
            },
        .checked = [&ctx] { return ctx.selection().pickMode() == plugin::PickMode::Joint; },
        .enabled =
            [assembly] {
                const auto* doc = assembly();
                return doc && doc->components().size() >= 2;
            }});
    ctx.listen<plugin::SelectionChanged>([&ctx](const plugin::SelectionChanged& e) {
        auto& sel = ctx.selection();
        if (sel.pickMode() != plugin::PickMode::Joint || e.elements.size() != 2) return;
        const auto first = e.elements[0], second = e.elements[1];
        if (first.component.isNil() || second.component.isNil()) return;
        if (first.component == second.component) {
            // Both on one component: the second click starts again.
            sel.setElements(e.doc, {second});
            return;
        }
        auto side = [](const plugin::PickedElement& p) {
            model::JointOrigin o;
            o.component = p.component;
            o.body = p.body;
            o.element = p.ref;
            return o;
        };
        model::Joint joint;
        joint.a = side(first);
        joint.b = side(second);
        auto command = std::make_unique<model::AddJointCommand>(joint);
        auto* made = command.get();
        auto problems = ctx.documents().execute(e.doc, std::move(command));
        if (!problems.empty()) {
            ctx.log(problems.front().message);
            sel.setElements(e.doc, {});
            return;
        }
        const JointId id = made->joint().id;
        sel.setPickMode(plugin::PickMode::Bodies);
        sel.setJoints(e.doc, {id});
    });
    ctx.addAction(
        ActionSpec{.id = "sb.modeling.reset_joints",
                   .text = "Reset joints",
                   .menu = "Assemble/Joints",
                   .command = "joint.reset",
                   .toolbar = true,
                   .buttonText = "Reset",
                   .icon = std::string(icons::modeling::reset_joints_svg),
                   .tip = "Put every hinge and slider back where it rests: the lid shut, the arm down.",
                   .enabled = [assembly] {
                       const auto* doc = assembly();
                       return doc && std::any_of(doc->joints().begin(), doc->joints().end(),
                                                 [](const model::Joint& j) { return j.value != j.rest; });
                   }});
    ctx.addAction(ActionSpec{.id = "sb.modeling.update_links",
                             .text = "Update linked parts",
                             .menu = "Assemble/Parts",
                             .command = "part.update",
                             .toolbar = true,
                             .buttonText = "Update",
                             .icon = std::string(icons::modeling::update_links_svg),
                             .tip = "Use the new part from each linked file that changed since the assembly "
                                    "opened.",
                             .enabled = [assembly] {
                                 const auto* doc = assembly();
                                 if (!doc) return false;
                                 const auto parts = doc->parts();
                                 return std::any_of(parts.begin(), parts.end(), [doc](const auto* p) {
                                     return doc->partLink(p->id()) != nullptr;
                                 });
                             }});
    ctx.addAction(ActionSpec{.id = "sb.modeling.find_file",
                             .text = "Find linked file",
                             .menu = "Assemble/Parts",
                             .command = "part.relink",
                             .fromComponentPart = "part",
                             .fromFile = "file",
                             .fileTypes = designs,
                             .toolbar = true,
                             .buttonText = "Find file",
                             .icon = std::string(icons::modeling::find_file_svg),
                             .tip = "Link the picked component's part to its file where it is now, when "
                                    "it was moved or renamed.",
                             .enabled = pickedLinked(true)});
    ctx.addAction(ActionSpec{.id = "sb.modeling.make_inside",
                             .text = "Make part inside",
                             .menu = "Assemble/Parts",
                             .command = "part.make_inside",
                             .fromComponentPart = "part",
                             .icon = std::string(icons::modeling::make_inside_svg),
                             .tip = "Keep the picked component's part only in the assembly, so changing its "
                                    "file no longer changes it.",
                             .enabled = pickedLinked(true)});
    ctx.addAction(ActionSpec{.id = "sb.modeling.save_part",
                             .text = "Save part as file",
                             .menu = "Assemble/Parts",
                             .command = "part.save_as",
                             .fromComponentPart = "part",
                             .fromFile = "file",
                             .fileTypes = designs,
                             .saveFile = true,
                             .suggestedFile =
                                 [assembly, picked] {
                                     const auto* c = picked();
                                     const auto* part = c ? assembly()->findPart(c->part) : nullptr;
                                     return part ? part->name() + ".sb" : std::string();
                                 },
                             .icon = std::string(icons::modeling::save_part_svg),
                             .tip = "Save the picked component's part as a .sb file of its own, and link it, "
                                    "to open it on its own or use it in other assemblies.",
                             .enabled = pickedLinked(false)});
}

void registerModeling(plugin::PluginContext& ctx) {
    // Construction planes are drawn in the view after every rebuild and pick.
    ctx.listen<plugin::RebuildFinished>([&ctx](const auto&) {
        drawPlanes(ctx);
        drawJoints(ctx);
        drawExplodeLines(ctx);
    });
    ctx.listen<plugin::SelectionChanged>([&ctx](const auto&) {
        drawPlanes(ctx);
        drawJoints(ctx);
    });
    ctx.listen<plugin::ActiveDocumentChanged>([&ctx](const auto&) {
        drawPlanes(ctx);
        drawJoints(ctx);
        drawExplodeLines(ctx);
    });

    const auto& builtin = model::CommandRegistry::builtin();
    for (const auto* info : builtin.commands()) {
        ctx.addCommand(*info, *builtin.factory(info->name));
    }
    // And the ones that read parts from .sb files (sb_io's).
    model::CommandRegistry files;
    io::addPartCommands(files);
    for (const auto* info : files.commands()) ctx.addCommand(*info, *files.factory(info->name));

    // Toolbar buttons and dropdown items. New shapes start at a sensible size; the
    // student changes the numbers in the Properties panel.
    // The Model tab is the usual one; it steps aside while a sketch is being edited.
    ctx.addTab({.name = "Model", .always = false, .mode = ""});
    using plugin::ActionSpec;
    ctx.addAction(ActionSpec{.id = "sb.modeling.box",
                             .text = "Box",
                             .menu = "Model/Create",
                             .command = "feature.add_box",
                             .args = {{"size", kernel::Vec3{20, 20, 20}}},
                             .shortcut = "Ctrl+Shift+B",
                             .toolbar = true,
                             .icon = std::string(icons::modeling::box_svg),
                             .tip = "Add a 20 mm box. Change its size in Properties."});
    ctx.addAction(ActionSpec{.id = "sb.modeling.cylinder",
                             .text = "Cylinder",
                             .menu = "Model/Create",
                             .command = "feature.add_cylinder",
                             .args = {{"radius", 10.0}, {"height", 20.0}},
                             .shortcut = "Ctrl+Shift+C",
                             .icon = std::string(icons::modeling::cylinder_svg),
                             .tip = "Add a cylinder 20 mm across and 20 mm tall. Change it in Properties."});
    struct Combine {
        const char* id;
        const char* text;
        const char* op;
        const char* tip;
        std::string_view icon;
    };
    for (const auto& c :
         {Combine{"sb.modeling.union", "Union", "union", "Join the two selected bodies into one.",
                  icons::modeling::union_svg},
          Combine{"sb.modeling.cut", "Cut", "cut", "Cut the second selected body out of the first.",
                  icons::modeling::cut_svg},
          Combine{"sb.modeling.intersect", "Intersect", "intersect",
                  "Keep only where the two selected bodies overlap.", icons::modeling::intersect_svg}}) {
        ctx.addAction(ActionSpec{.id = c.id,
                                 .text = c.text,
                                 .menu = "Model/Combine",
                                 .command = "feature.add_boolean",
                                 .args = {{"operation", std::string(c.op)}},
                                 .fromSelection = {"a", "b"},
                                 .toolbar = true,
                                 .icon = std::string(c.icon),
                                 .tip = c.tip});
    }
    ctx.addAction(ActionSpec{.id = "sb.modeling.move",
                             .text = "Move",
                             .menu = "Model/Modify",
                             .command = "feature.add_move",
                             .fromSelection = {"body"},
                             .shortcut = "Ctrl+M",
                             .toolbar = true,
                             .icon = std::string(icons::modeling::move_svg),
                             .tip = "Move or turn the picked body. Then type how far in Properties."});
    // A linked copy: a Move that keeps the original, slid 20 mm along x to start.
    ctx.addAction(ActionSpec{.id = "sb.modeling.linked_copy",
                             .text = "Linked Copy",
                             .menu = "Model/Modify",
                             .command = "feature.add_move",
                             .args = {{"copy", true}, {"by", kernel::Vec3{20, 0, 0}}},
                             .fromSelection = {"body"},
                             .shortcut = "Ctrl+Shift+M",
                             .tip = "Make a copy of the picked body that changes when the original does. "
                                    "Then type where it goes in Properties."});
    // Unlink: the one linked copy picked gets steps of its own.
    ctx.addAction(ActionSpec{
        .id = "sb.modeling.unlink",
        .text = "Unlink Copy",
        .menu = "Model/Modify",
        .command = "feature.unlink",
        .fromSelection = {"feature"},
        .tip = "Make the picked linked copy a copy of its own: it stops changing when the original does, "
               "and gets its own steps to change.",
        .enabled = [&ctx] {
            const auto& sel = ctx.selection();
            const auto* doc = ctx.documents().find(sel.doc());
            const auto* f = doc && sel.features().size() == 1 ? doc->find(sel.features()[0]) : nullptr;
            return f && f->type() == model::FeatureType::Move &&
                   static_cast<const model::MoveFeature*>(f)->copy();
        }});
    // Extrude: the one sketch picked, swept 10 mm out of its plane.
    ctx.addAction(ActionSpec{
        .id = "sb.modeling.extrude",
        .text = "Extrude",
        .menu = "Model/Create",
        .command = "feature.add_extrude",
        .args = {{"distance", 10.0}},
        .fromSelection = {"sketch"},
        .shortcut = "Ctrl+Shift+E",
        .toolbar = true,
        .icon = std::string(icons::modeling::extrude_svg),
        .tip = "Make a 10 mm solid from the picked sketch's closed shapes. Change how far in "
               "Properties.",
        .enabled = [&ctx] {
            const auto& sel = ctx.selection();
            const auto* doc = ctx.documents().find(sel.doc());
            const auto* f = doc && sel.features().size() == 1 ? doc->find(sel.features()[0]) : nullptr;
            return f && f->type() == model::FeatureType::Sketch;
        }});
    // Revolve: the one sketch picked, turned all the way round its y axis (or its
    // only helper line).
    ctx.addAction(ActionSpec{
        .id = "sb.modeling.revolve",
        .text = "Revolve",
        .menu = "Model/Create",
        .command = "feature.add_revolve",
        .fromSelection = {"sketch"},
        .toolbar = true,
        .icon = std::string(icons::modeling::revolve_svg),
        .tip = "Turn the picked sketch's closed shapes all the way round a line, like a lathe: its y axis, "
               "or its helper line. Change the axis and angle in Properties.",
        .enabled = [&ctx] {
            const auto& sel = ctx.selection();
            const auto* doc = ctx.documents().find(sel.doc());
            const auto* f = doc && sel.features().size() == 1 ? doc->find(sel.features()[0]) : nullptr;
            return f && f->type() == model::FeatureType::Sketch;
        }});
    // Hole: M3 clearance holes, through, at every point of the one sketch picked
    // (drawn on a face of the body); or one hole 10 mm in from two picked edges.
    ctx.addAction(ActionSpec{
        .id = "sb.modeling.hole",
        .text = "Hole",
        .menu = "Model/Create",
        .command = "feature.add_hole",
        .args = {{"size", std::string("M3")}},
        .fromSelection = {"sketch"},
        .toolbar = true,
        .icon = std::string(icons::modeling::hole_svg),
        .tip =
            "Drill an M3 hole through the body at every point of the picked sketch. Change the size, depth "
            "and kind in Properties.",
        .enabled = [&ctx] {
            const auto& sel = ctx.selection();
            const auto* doc = ctx.documents().find(sel.doc());
            const auto* f = doc && sel.features().size() == 1 ? doc->find(sel.features()[0]) : nullptr;
            return f && f->type() == model::FeatureType::Sketch;
        }});
    ctx.addAction(ActionSpec{
        .id = "sb.modeling.hole_edges",
        .text = "Hole from two edges",
        .menu = "Model/Create",
        .command = "feature.add_hole",
        .args = {{"size", std::string("M3")}, {"distances", sketch::Point2{10, 10}}},
        .fromEdges = "edges",
        .fromEdgesBody = "body",
        .tip =
            "Drill an M3 hole 10 mm in from two picked edges of a flat face. With no edges picked, click two "
            "edges first. Change how far in from each in Properties."});
    // Patterns of the picked steps: their holes, joins and cuts again; or, for
    // one step that makes a body of its own, copies of the body.
    const auto patternable = [&ctx] {
        const auto& sel = ctx.selection();
        const auto* doc = ctx.documents().find(sel.doc());
        if (!doc || sel.features().empty()) return false;
        for (const auto& id : sel.features()) {
            const auto* f = doc->find(id);
            if (!f || f->type() == model::FeatureType::Sketch) return false;
        }
        return true;
    };
    ctx.addAction(ActionSpec{
        .id = "sb.modeling.pattern_linear",
        .text = "Linear pattern",
        .menu = "Model/Pattern",
        .command = "feature.add_pattern",
        .args = {{"kind", std::string("linear")}},
        .fromSelectionList = "steps",
        .toolbar = true,
        .buttonText = "Row",
        .icon = std::string(icons::modeling::pattern_linear_svg),
        .tip =
            "Repeat the picked hole, join or cut 3 times along x, 10 mm apart; or copy a picked body. Change "
            "how many, which way and how far in Properties.",
        .enabled = patternable});
    ctx.addAction(ActionSpec{
        .id = "sb.modeling.pattern_circular",
        .text = "Circular pattern",
        .menu = "Model/Pattern",
        .command = "feature.add_pattern",
        .args = {{"kind", std::string("circular")}},
        .fromSelectionList = "steps",
        .toolbar = true,
        .buttonText = "Around",
        .icon = std::string(icons::modeling::pattern_circular_svg),
        .tip =
            "Repeat the picked hole, join or cut 6 times round the z axis; or copy a picked body. Change how "
            "many and the axis in Properties.",
        .enabled = patternable});
    // Mirror: the picked steps' holes, joins and cuts, or a picked body, across
    // the YZ plane (left and right swap).
    ctx.addAction(ActionSpec{
        .id = "sb.modeling.mirror",
        .text = "Mirror",
        .menu = "Model/Pattern",
        .command = "feature.add_mirror",
        .fromSelectionList = "steps",
        .toolbar = true,
        .icon = std::string(icons::modeling::mirror_svg),
        .tip =
            "Mirror the picked hole, join or cut, or a picked body, across the YZ plane, so left and right "
            "match. Change the plane in Properties.",
        .enabled = patternable});
    // Shell: the picked faces of one body left open, 2 mm walls.
    ctx.addAction(ActionSpec{
        .id = "sb.modeling.shell",
        .text = "Shell",
        .menu = "Model/Modify",
        .command = "feature.add_shell",
        .args = {{"thickness", 2.0}},
        .fromFaces = "faces",
        .fromFacesBody = "body",
        .toolbar = true,
        .icon = std::string(icons::modeling::shell_svg),
        .tip =
            "Hollow a body out to 2 mm walls, leaving the picked faces open, like a box with no lid. With no "
            "faces picked, click the faces first. Change the thickness in Properties."});
    // Fillet and Chamfer: the picked edges of one body. With none picked, the
    // first click starts picking edges.
    ctx.addAction(ActionSpec{
        .id = "sb.modeling.fillet",
        .text = "Fillet",
        .menu = "Model/Modify",
        .command = "feature.add_fillet",
        .args = {{"radius", 1.0}},
        .fromEdges = "edges",
        .fromEdgesBody = "body",
        .toolbar = true,
        .icon = std::string(icons::modeling::fillet_svg),
        .tip = "Round the picked edges by 1 mm. Change the radius in Properties. With no edges picked, "
               "click the edges first."});
    ctx.addAction(ActionSpec{
        .id = "sb.modeling.chamfer",
        .text = "Chamfer",
        .menu = "Model/Modify",
        .command = "feature.add_chamfer",
        .args = {{"distance", 1.0}},
        .fromEdges = "edges",
        .fromEdgesBody = "body",
        .toolbar = true,
        .icon = std::string(icons::modeling::chamfer_svg),
        .tip = "Bevel the picked edges by 1 mm. Change the distance in Properties. With no edges picked, "
               "click the edges first."});
    // What clicking a body picks: the body (the usual) or one of its edges.
    ctx.addAction(ActionSpec{
        .id = "sb.modeling.pick_edges",
        .text = "Pick edges",
        .menu = "Model/Select",
        .toolbar = true,
        .icon = std::string(icons::modeling::pick_edges_svg),
        .tip = "Click edges of bodies instead of whole bodies, for a fillet or chamfer.",
        .run =
            [&ctx] {
                auto& sel = ctx.selection();
                sel.setPickMode(sel.pickMode() == plugin::PickMode::Edges ? plugin::PickMode::Bodies
                                                                          : plugin::PickMode::Edges);
            },
        .checked = [&ctx] { return ctx.selection().pickMode() == plugin::PickMode::Edges; }});
    ctx.addAction(ActionSpec{.id = "sb.modeling.duplicate",
                             .text = "Duplicate step",
                             .menu = "Model/Steps",
                             .command = "feature.duplicate",
                             .fromSelection = {"feature"},
                             .shortcut = "Ctrl+D",
                             .tip = "Copy the selected step, and the steps it uses, as new steps."});
    ctx.addAction(ActionSpec{.id = "sb.modeling.turn_off",
                             .text = "Turn off",
                             .menu = "Model/Steps",
                             .command = "feature.suppress",
                             .args = {{"off", true}},
                             .fromSelectionList = "steps",
                             .tip = "Turn the picked steps off, so they aren't built. Nothing is deleted."});
    ctx.addAction(ActionSpec{.id = "sb.modeling.turn_on",
                             .text = "Turn on",
                             .menu = "Model/Steps",
                             .command = "feature.suppress",
                             .args = {{"off", false}},
                             .fromSelectionList = "steps",
                             .tip = "Turn the picked steps back on."});
    ctx.addAction(ActionSpec{.id = "sb.modeling.new_folder",
                             .text = "New folder",
                             .menu = "Model/Steps",
                             .command = "folder.add",
                             .fromSelectionList = "steps",
                             .tip = "Put the picked steps in a folder, to keep the feature tree tidy."});
    ctx.addAction(ActionSpec{.id = "sb.modeling.delete",
                             .text = "Delete step",
                             .menu = "Model/Steps",
                             .command = "feature.delete",
                             .fromSelection = {"feature"},
                             .shortcut = "Delete",
                             .tip = "Delete the selected step."});
    addAssembleTools(ctx);
    addExplodeTools(ctx);
}

}  // namespace

plugin::BuiltinPlugin modelingPlugin() {
    return {kManifest, &registerModeling};
}

}  // namespace sb::plugins
