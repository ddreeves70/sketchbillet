#include "InputBoxes.h"

#include <QKeyEvent>
#include <QLineEdit>
#include <QWidget>

#include <algorithm>

namespace sb::view {

InputBoxes::InputBoxes(QWidget* host, std::function<std::optional<QPointF>(const kernel::Vec3&)> place)
    : QObject(host), host_(host), where_(std::move(place)) {}

QLineEdit* InputBoxes::box(const std::string& id) const {
    for (const auto& b : boxes_) {
        if (b.id == id) return b.edit;
    }
    return nullptr;
}

void InputBoxes::set(const std::vector<ViewInput>& inputs, const std::string& focus) {
    inputs_ = inputs;
    // Boxes no longer asked for go.
    std::erase_if(boxes_, [&](Box& b) {
        const bool kept = std::ranges::any_of(inputs, [&](const ViewInput& i) { return i.id == b.id; });
        if (!kept) b.edit->deleteLater();
        return !kept;
    });
    std::vector<Box> ordered;
    for (const auto& input : inputs) {
        auto it = std::ranges::find_if(boxes_, [&](const Box& b) { return b.id == input.id; });
        if (it == boxes_.end()) {
            auto* edit = new QLineEdit(host_);
            edit->setObjectName(QString::fromStdString("sb.view.input." + input.id));
            edit->setAlignment(Qt::AlignCenter);
            edit->setFixedWidth(96);
            edit->setStyleSheet(
                QStringLiteral("QLineEdit { background: white; color: #1f2328; border: 1px solid "
                               "#3d7ad6; border-radius: 3px; padding: 1px 3px; }"));
            edit->installEventFilter(this);
            const std::string id = input.id;
            connect(edit, &QLineEdit::textEdited, this, [this, id](const QString& text) {
                for (auto& b : boxes_) {
                    if (b.id == id) b.typedIn = true;
                }
                if (typed) typed(id, text);
            });
            connect(edit, &QLineEdit::returnPressed, this, [this, id] {
                if (entered) entered(id);
            });
            edit->setText(QString::fromStdString(input.text));
            edit->show();
            ordered.push_back({input.id, edit, false});
        } else {
            // What the student typed stays; the rest follows what's asked.
            if (!it->typedIn && it->edit->text() != QString::fromStdString(input.text)) {
                const bool all = it->edit->hasFocus() && it->edit->selectedText() == it->edit->text();
                it->edit->setText(QString::fromStdString(input.text));
                if (all) it->edit->selectAll();
            }
            ordered.push_back(*it);
        }
    }
    boxes_ = std::move(ordered);
    place();
    if (QLineEdit* f = box(focus)) {
        f->setFocus(Qt::OtherFocusReason);
        f->selectAll();
    }
}

void InputBoxes::place() {
    for (const auto& b : boxes_) {
        auto it = std::ranges::find_if(inputs_, [&](const ViewInput& i) { return i.id == b.id; });
        if (it == inputs_.end()) continue;
        const auto at = where_(it->at);
        b.edit->setVisible(at.has_value());
        if (!at) continue;
        const QSize size = b.edit->sizeHint().expandedTo({b.edit->width(), 0});
        b.edit->move(static_cast<int>(at->x() - b.edit->width() / 2.0),
                     static_cast<int>(at->y() - size.height() / 2.0));
        b.edit->raise();
    }
}

bool InputBoxes::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() != QEvent::KeyPress || boxes_.empty()) return QObject::eventFilter(watched, event);
    const auto key = static_cast<QKeyEvent*>(event)->key();
    if (key == Qt::Key_Escape) {
        if (cancelled) cancelled();
        return true;
    }
    if (key == Qt::Key_Tab || key == Qt::Key_Backtab) {
        // Tab goes to the next box, round from the last to the first; Shift+Tab back.
        auto it = std::ranges::find_if(boxes_, [&](const Box& b) { return b.edit == watched; });
        if (it == boxes_.end()) return false;
        const auto n = static_cast<long>(boxes_.size());
        const long at = (it - boxes_.begin() + (key == Qt::Key_Tab ? 1 : -1) + n) % n;
        QLineEdit* next = boxes_[static_cast<std::size_t>(at)].edit;
        next->setFocus(Qt::TabFocusReason);
        next->selectAll();
        return true;
    }
    return QObject::eventFilter(watched, event);
}

}  // namespace sb::view
