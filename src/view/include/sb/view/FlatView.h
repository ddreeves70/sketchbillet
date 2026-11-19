#pragma once

/// @file
/// A flat, drawn-with-Qt stand-in for the 3D view, for computers without 3D graphics.

#include <QPointF>
#include <QString>
#include <QWidget>

#include <vector>

#include "sb/view/Choices.h"
#include "sb/view/Scene.h"

class QLineEdit;

namespace sb::view {

class InputBoxes;

/// Draws the overlays (such as the grid) seen from above, with a text card in the top-left corner.
///
/// The viewer uses it when 3D graphics can't start (see graphicsProblem()), so
/// the student still sees the grid and a list of the finished bodies.
class FlatView : public QWidget {
    Q_OBJECT
public:
    /// Makes an empty view. Its object name is "sb.view.flat".
    /// @param parent The widget it sits in.
    explicit FlatView(QWidget* parent = nullptr);

    /// Changes the text on the card. Empty hides the card.
    /// @param text The text, with line breaks.
    void setText(const QString& text);

    /// Changes what it draws behind the text, seen from above (x to the right,
    /// y up, the origin in the middle). Lattices are drawn first, underneath
    /// the others.
    /// @param overlays The overlays, drawn in this order.
    void setOverlays(const std::vector<ViewOverlay>& overlays);

    /// Gives the text on the card.
    /// @return The text.
    const QString& text() const { return text_; }

    /// Gives the overlays it draws.
    /// @return The overlays.
    const std::vector<ViewOverlay>& overlays() const { return overlays_; }

    /// Gives the distance between the drawn lines or dots of a lattice, in
    /// screen pixels. A lattice too fine to see draws every 2nd, 5th, 10th, ...
    /// line instead, so it doesn't turn into a grey smear.
    /// @param lattice A Lattice overlay.
    /// @return The drawn distance, in pixels.
    static double pixelsPerSpace(const ViewOverlay& lattice);

    /// Starts or stops drawing mode, as ViewWidget::setDrawingPlane() does. The
    /// flat view is seen from above, so it only draws on a flat plane (one whose
    /// normal is up or down); on others, it reports nothing.
    /// @param plane The plane to draw on, or nothing to stop drawing.
    void setDrawingPlane(std::optional<DrawingPlane> plane);

    /// Shows flat squares to click, as ViewWidget::setChoices() does. Seen from
    /// above, a square standing up is a line: clicking near it chooses it.
    /// @param choices The squares; empty to show none.
    void setChoices(const std::vector<ViewChoice>& choices);

    /// Gives the squares shown to click.
    /// @return The choices last given to setChoices().
    const std::vector<ViewChoice>& choices() const { return choices_; }

    /// Shows boxes to type numbers in, at points of the design, in place of the
    /// ones shown, like a rectangle's width and height while it's drawn. Typing
    /// sends inputTyped(), Enter inputEntered() and Escape inputsCancelled(); Tab
    /// and Shift+Tab go from box to box. A box keeps what the student typed in it
    /// until it's taken away; the others show the text given each time.
    /// @param inputs The boxes, in Tab order; empty to show none.
    /// @param focus A box to give the keyboard to, its words picked so typing
    ///        replaces them; empty to leave the keyboard where it is.
    void setInputs(const std::vector<ViewInput>& inputs, const std::string& focus = {});

    /// Gives the boxes shown to type in.
    /// @return The inputs last given to setInputs().
    const std::vector<ViewInput>& inputs() const;

    /// Finds one of the boxes, for tests that type in it.
    /// @param id The box's id.
    /// @return The box, or nullptr if it isn't shown.
    QLineEdit* input(const std::string& id) const;

    /// Gives the screen position of a point, for tests that click on it.
    /// @param point A point in mm (z is ignored: the view is seen from above).
    /// @return Where it is drawn, in widget coordinates.
    QPointF toScreen(const kernel::Vec3& point) const;

signals:
    /// While drawing: the mouse moved over the drawing plane.
    /// @param point Where, on the plane.
    void planeMoved(const sb::view::PlanePoint& point);

    /// While drawing: the student clicked on the drawing plane.
    /// @param point Where, on the plane.
    void planeClicked(const sb::view::PlanePoint& point);

    /// While drawing: the student is dragging with the left button (pressed, then
    /// moved more than a few pixels). Sent on every move until planeDropped().
    /// @param from Where the button went down, on the plane.
    /// @param to Where the mouse is now, on the plane.
    void planeDragged(const sb::view::PlanePoint& from, const sb::view::PlanePoint& to);

    /// While drawing: a drag ended (the left button came up).
    /// @param from Where the button went down, on the plane.
    /// @param to Where it came up, on the plane.
    void planeDropped(const sb::view::PlanePoint& from, const sb::view::PlanePoint& to);

    /// While drawing: the student pressed Escape.
    void planeCancelled();

    /// The student clicked one of the squares to choose (see setChoices()).
    /// @param id The choice's id.
    void choiceClicked(const std::string& id);

    /// The student pressed Escape while squares to choose were shown.
    void choicesCancelled();

    /// The student typed in one of the boxes (see setInputs()).
    /// @param id The box's id.
    /// @param text What the box says now.
    void inputTyped(const std::string& id, const QString& text);

    /// The student pressed Enter in one of the boxes.
    /// @param id The box's id.
    void inputEntered(const std::string& id);

    /// The student pressed Escape in one of the boxes.
    void inputsCancelled();

protected:
    /// Draws the overlays and the card.
    /// @param event Which part to draw (it draws everything).
    void paintEvent(QPaintEvent* event) override;
    /// Reports a click while drawing.
    /// @param event The mouse.
    void mousePressEvent(QMouseEvent* event) override;
    /// Reports the mouse while drawing.
    /// @param event The mouse.
    void mouseMoveEvent(QMouseEvent* event) override;
    /// Reports a click or the end of a drag while drawing.
    /// @param event The mouse.
    void mouseReleaseEvent(QMouseEvent* event) override;
    /// Sends Escape to the drawing tool while drawing.
    /// @param event The key.
    void keyPressEvent(QKeyEvent* event) override;

private:
    std::optional<PlanePoint> planePoint(QPointF at) const;
    std::vector<ViewChoice> shownChoices() const;  // with sizes in mm
    std::optional<std::size_t> choiceUnder(QPointF at) const;
    void updateTracking();

    QString text_;
    std::optional<DrawingPlane> drawing_;
    // Where the left button went down, to tell a click from a drag.
    QPoint leftPress_;
    std::optional<PlanePoint> pressPoint_;
    bool leftDown_ = false;
    bool dragging_ = false;
    std::vector<ViewOverlay> overlays_;
    std::vector<ViewChoice> choices_;
    std::optional<std::size_t> hoveredChoice_;
    std::optional<std::size_t> pressedChoice_;
    InputBoxes* inputs_ = nullptr;  // boxes to type numbers in (setInputs())
};

}  // namespace sb::view
