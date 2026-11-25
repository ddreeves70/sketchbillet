#pragma once

/// @file
/// The shared selection: which steps, edges (or which design's settings) the student has picked.

#include <vector>

#include "sb/core/Id.h"
#include "sb/plugin/DocumentList.h"
#include "sb/plugin/EventBus.h"
#include "sb/plugin/Picks.h"

namespace sb::plugin {

/// What the student has picked, shared by every panel and view.
///
/// Picking a step in the feature tree selects it here, and every other panel
/// and view hears SelectionChanged and highlights it too. A selection is a list of
/// steps in one design, or, while viewers pick edges, faces or things to measure
/// (see PickMode), a list of edges, faces or points of its bodies, or, in an
/// assembly, a list of its components.
///
/// The selection keeps itself valid: steps that are deleted or undone away drop
/// out of it, and closing the design clears it.
///
/// @code
/// ctx.selection().set(doc, {boxId});
/// if (ctx.selection().contains(boxId)) { ... }
/// @endcode
class Selection {
public:
    /// Makes an empty selection.
    /// @param bus Where SelectionChanged is sent. Must outlive the selection.
    /// @param docs The open designs, to drop steps that no longer exist. Must outlive the selection.
    Selection(EventBus& bus, const DocumentList& docs);

    Selection(const Selection&) = delete;
    Selection& operator=(const Selection&) = delete;

    /// Gives the design the selection is in.
    /// @return Its id, or a nil id when nothing is selected.
    DocId doc() const { return doc_; }

    /// Gives the selected steps.
    /// @return Their ids, in the order they were picked.
    const std::vector<FeatureId>& features() const { return features_; }

    /// Checks whether a step is selected.
    /// @param id The step.
    /// @return True if it is selected.
    bool contains(const FeatureId& id) const;

    /// Replaces the selection. Sends SelectionChanged if it differs. Ids that
    /// aren't steps of @p doc, and repeats, are left out.
    /// @param doc The design.
    /// @param features The steps to select, in order.
    void set(const DocId& doc, std::vector<FeatureId> features);

    /// Selects nothing. Sends SelectionChanged if something was selected.
    void clear();

    /// Gives the picked components, while the selection is an assembly's components.
    /// @return Them, in the order they were picked; empty when steps are selected.
    const std::vector<model::ComponentId>& components() const { return components_; }

    /// Checks whether a component is picked.
    /// @param id The component.
    /// @return True if it is.
    bool containsComponent(const model::ComponentId& id) const;

    /// Picks components of an assembly instead of steps, as clicking one in a
    /// viewer or the feature tree does. Sends SelectionChanged if it differs. Ids
    /// that aren't components of @p doc, and repeats, are left out.
    /// @code
    /// ctx.selection().setComponents(car, {wheel1});
    /// @endcode
    /// @param doc The assembly.
    /// @param components The components, in the order they were picked.
    void setComponents(const DocId& doc, std::vector<model::ComponentId> components);

    /// Gives the picked joints of an assembly (in the feature tree), in the order
    /// they were picked; empty when steps or components are picked instead.
    /// @return The joints.
    const std::vector<JointId>& joints() const { return joints_; }

    /// Says whether a joint is picked.
    /// @param id The joint.
    /// @return True if it's picked.
    bool containsJoint(const JointId& id) const;

    /// Picks joints of an assembly in place of anything else. Joints it doesn't
    /// have are left out; none left clears the selection. Sends SelectionChanged.
    /// @param doc The assembly.
    /// @param joints The joints, in the order they were picked.
    void setJoints(const DocId& doc, std::vector<JointId> joints);

    /// Gives the picked edges, faces or points, while the selection is those instead of steps.
    /// @return Them, in the order they were picked; empty when steps are selected.
    const std::vector<PickedElement>& elements() const { return elements_; }

    /// Selects edges, faces or points instead of steps. Sends SelectionChanged if
    /// it differs. Ones on steps that aren't in @p doc, and repeats, are left out.
    /// @param doc The design.
    /// @param elements The edges, faces or points, in order.
    void setElements(const DocId& doc, std::vector<PickedElement> elements);

    /// Gives what clicking a body in a viewer picks.
    /// @return Bodies (steps), edges, faces, or things to measure.
    PickMode pickMode() const { return mode_; }

    /// Changes what clicking a body picks. Sends PickModeChanged if it differs,
    /// and clears picked edges or faces when it does.
    /// @param mode Bodies or edges.
    void setPickMode(PickMode mode);

    /// Gives the one body viewers show while picking its edges or faces again
    /// (for a step that uses a body it replaced, like a Fillet), or nil.
    /// @return The step that made that body, or nil for every body as usual.
    const FeatureId& pickOnly() const { return only_; }

    /// Picks edges or faces of one body only: viewers show just that body, as it
    /// was before the steps that used it. Sends PickModeChanged. Going back to
    /// PickMode::Bodies forgets it.
    /// @param body The step that made the body, or nil for every body.
    void setPickOnly(const FeatureId& body);

    /// Picks the design itself instead of any of its steps, as the feature
    /// tree's **Document settings** row does: Properties then shows the design's
    /// settings. Sends SelectionChanged if it differs. Picking steps with set()
    /// (or clear()) un-picks it.
    /// @code
    /// ctx.selection().pickDesign(ctx.documents().active());
    /// @endcode
    /// @param doc The design. An id that isn't an open design selects nothing.
    void pickDesign(const DocId& doc);

    /// Checks whether the design itself is picked (see pickDesign()).
    /// @return True if it is; doc() says which design, and features() is empty.
    bool designPicked() const { return design_; }

    /// Gives the exploded view the viewers show an assembly in, if any: its
    /// components drawn pulled apart. It stays while picks change, so the
    /// student can pick components to add a step.
    /// @return The view, or nil when the assembly is drawn as it is.
    const ExplodedViewId& explodedView() const { return exploded_; }

    /// Gives the assembly the shown exploded view is of.
    /// @return It, or nil when no exploded view is shown.
    const DocId& explodedDoc() const { return explodedDoc_; }

    /// Shows an assembly in one of its exploded views, or as it is again. Sends
    /// ExplodedViewShown if it changes. It stops by itself when the view is
    /// deleted or the assembly is closed.
    /// @code
    /// ctx.selection().showExploded(ctx.documents().active(), view.id);
    /// @endcode
    /// @param doc The assembly.
    /// @param view The view, or nil to show the assembly as it is. One the
    ///        assembly doesn't have shows it as it is.
    void showExploded(const DocId& doc, const ExplodedViewId& view);

private:
    void prune();
    void pruneExploded();

    EventBus& bus_;
    const DocumentList& docs_;
    DocId doc_;
    std::vector<FeatureId> features_;
    std::vector<PickedElement> elements_;
    std::vector<model::ComponentId> components_;
    std::vector<JointId> joints_;
    PickMode mode_ = PickMode::Bodies;
    FeatureId only_{};
    bool design_ = false;
    DocId explodedDoc_;        // the assembly an exploded view is shown of
    ExplodedViewId exploded_;  // and the view
    std::vector<Subscription> subscriptions_;
};

}  // namespace sb::plugin
