#pragma once

// Boxes to type numbers in, over a view, at points of the design (ViewInput).
// Both views use it: the 3D view and the flat one.

#include <QObject>
#include <QPointF>
#include <QString>

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "sb/view/Scene.h"

class QLineEdit;
class QWidget;

namespace sb::view {

class InputBoxes final : public QObject {
public:
    // `host` is the view the boxes sit on; `place` says where a point of the
    // design is on it, in its own (logical) pixels, or nothing if it's off screen.
    InputBoxes(QWidget* host, std::function<std::optional<QPointF>(const kernel::Vec3&)> place);

    // Shows these boxes (and takes away others). A box keeps what the student
    // typed in it until it's taken away; others show their given text. With
    // `focus`, that box gets the keyboard, its words picked so typing replaces them.
    void set(const std::vector<ViewInput>& inputs, const std::string& focus);
    const std::vector<ViewInput>& inputs() const { return inputs_; }
    QLineEdit* box(const std::string& id) const;
    // Puts each box at its point again (after the view turned, panned or zoomed).
    void place();

    std::function<void(const std::string& id, const QString& text)> typed;  // each change
    std::function<void(const std::string& id)> entered;                     // Enter
    std::function<void()> cancelled;                                        // Esc

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    struct Box {
        std::string id;
        QLineEdit* edit = nullptr;
        bool typedIn = false;  // the student typed in it: keep their words
    };
    QWidget* host_;
    std::function<std::optional<QPointF>(const kernel::Vec3&)> where_;
    std::vector<ViewInput> inputs_;
    std::vector<Box> boxes_;
};

}  // namespace sb::view
