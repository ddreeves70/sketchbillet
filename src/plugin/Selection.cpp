#include "sb/plugin/Selection.h"

#include <algorithm>
#include <utility>

#include "sb/plugin/Events.h"

namespace sb::plugin {

Selection::Selection(EventBus& bus, const DocumentList& docs) : bus_(bus), docs_(docs) {
    subscriptions_.push_back(bus_.subscribe<DocumentChanged>([this](const DocumentChanged& e) {
        if (e.doc == doc_) prune();
    }));
    subscriptions_.push_back(bus_.subscribe<DocumentClosed>([this](const DocumentClosed& e) {
        if (e.doc == doc_) clear();
    }));
}

bool Selection::contains(const FeatureId& id) const {
    return std::find(features_.begin(), features_.end(), id) != features_.end();
}

void Selection::set(const DocId& doc, std::vector<FeatureId> features) {
    const auto* design = docs_.find(doc);
    std::vector<FeatureId> kept;
    if (design) {
        for (const auto& id : features) {
            if (design->find(id) && std::find(kept.begin(), kept.end(), id) == kept.end()) kept.push_back(id);
        }
    }
    const DocId newDoc = kept.empty() ? DocId{} : doc;
    if (newDoc == doc_ && kept == features_ && !design_ && elements_.empty() && components_.empty() &&
        joints_.empty())
        return;
    doc_ = newDoc;
    features_ = std::move(kept);
    elements_.clear();
    components_.clear();
    joints_.clear();
    design_ = false;
    bus_.publish(SelectionChanged{doc_, features_, false, {}, {}, {}});
}

void Selection::setElements(const DocId& doc, std::vector<PickedElement> elements) {
    const auto* design = docs_.find(doc);
    std::vector<PickedElement> kept;
    if (design) {
        for (auto& e : elements) {
            if (design->find(e.body) && !e.ref.name.empty() &&
                std::find(kept.begin(), kept.end(), e) == kept.end()) {
                kept.push_back(std::move(e));
            }
        }
    }
    if (kept.empty()) {
        if (elements_.empty()) {
            set(doc_, features_);
            return;
        }
        elements_.clear();
        set(DocId{}, {});
        return;
    }
    if (doc == doc_ && kept == elements_ && features_.empty() && !design_) return;
    doc_ = doc;
    features_.clear();
    components_.clear();
    joints_.clear();
    elements_ = std::move(kept);
    design_ = false;
    bus_.publish(SelectionChanged{doc_, features_, false, elements_, {}, {}});
}

void Selection::setPickOnly(const FeatureId& body) {
    if (body == only_) return;
    only_ = body;
    bus_.publish(PickModeChanged{mode_});
}

void Selection::setPickMode(PickMode mode) {
    if (mode == mode_) return;
    mode_ = mode;
    if (mode == PickMode::Bodies) only_ = FeatureId{};
    if (!elements_.empty()) clear();  // picked edges aren't faces, and the other way round
    bus_.publish(PickModeChanged{mode_});
}

void Selection::pickDesign(const DocId& doc) {
    if (!docs_.find(doc)) {
        clear();
        return;
    }
    if (design_ && doc_ == doc) return;
    doc_ = doc;
    features_.clear();
    elements_.clear();
    components_.clear();
    joints_.clear();
    design_ = true;
    bus_.publish(SelectionChanged{doc_, features_, true, {}, {}, {}});
}

bool Selection::containsComponent(const model::ComponentId& id) const {
    return std::find(components_.begin(), components_.end(), id) != components_.end();
}

void Selection::setComponents(const DocId& doc, std::vector<model::ComponentId> components) {
    const auto* design = docs_.find(doc);
    std::vector<model::ComponentId> kept;
    if (design) {
        for (const auto& id : components) {
            if (design->findComponent(id) && std::find(kept.begin(), kept.end(), id) == kept.end())
                kept.push_back(id);
        }
    }
    if (kept.empty()) {
        if (components_.empty()) return;
        components_.clear();
        set(DocId{}, {});
        return;
    }
    if (doc == doc_ && kept == components_) return;
    doc_ = doc;
    features_.clear();
    elements_.clear();
    joints_.clear();
    design_ = false;
    components_ = std::move(kept);
    bus_.publish(SelectionChanged{doc_, features_, false, {}, components_, {}});
}

bool Selection::containsJoint(const JointId& id) const {
    return std::find(joints_.begin(), joints_.end(), id) != joints_.end();
}

void Selection::setJoints(const DocId& doc, std::vector<JointId> joints) {
    const auto* design = docs_.find(doc);
    std::vector<JointId> kept;
    if (design) {
        for (const auto& id : joints) {
            if (design->findJoint(id) && std::find(kept.begin(), kept.end(), id) == kept.end())
                kept.push_back(id);
        }
    }
    if (kept.empty()) {
        if (joints_.empty()) return;
        joints_.clear();
        set(DocId{}, {});
        return;
    }
    if (doc == doc_ && kept == joints_) return;
    doc_ = doc;
    features_.clear();
    elements_.clear();
    components_.clear();
    design_ = false;
    joints_ = std::move(kept);
    bus_.publish(SelectionChanged{doc_, features_, false, {}, {}, joints_});
}

void Selection::clear() {
    set(DocId{}, {});
}

void Selection::prune() {
    if (design_) return;  // the design is still there
    if (!components_.empty()) {
        setComponents(doc_, components_);  // drops components that are gone
        return;
    }
    if (!joints_.empty()) {
        setJoints(doc_, joints_);  // drops joints that are gone
        return;
    }
    if (!elements_.empty()) {
        setElements(doc_, elements_);  // drops edges on steps that are gone
        return;
    }
    set(doc_, features_);  // drops steps that are gone
}

}  // namespace sb::plugin
