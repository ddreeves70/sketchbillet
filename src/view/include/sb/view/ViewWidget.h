#pragma once

/// @file
/// One 3D view of a design: OCCT draws it inside a Qt widget.

#include <QOpenGLWidget>
#include <QPoint>
#include <QString>

#include <memory>
#include <vector>

#include "sb/core/Id.h"
#include "sb/view/Choices.h"
#include "sb/view/Handles.h"
#include "sb/view/Layout.h"
#include "sb/view/Scene.h"

class QLineEdit;

namespace sb::view {

/// A 3D view with its own camera, a view cube and the grid.
///
/// Mouse: drag with the left button to orbit, with the right or middle button
/// (or Shift and the left button) to pan, and turn the wheel to zoom. Click a
/// body to select it; Ctrl-click or Shift-click adds or removes one. Click a
/// face, edge or corner of the view cube to turn the camera to it.
///
/// The view never changes the design or the selection itself: it shows the
/// SceneContent it is given, and reports clicks through picked().
///
/// @code
/// auto* view = new sb::view::ViewWidget;
/// view->setContent({{{boxId, boxShape, false}}, {}});
/// view->setStandardView(sb::view::StandardView::Iso);
/// QObject::connect(view, &sb::view::ViewWidget::picked,
///                  [&](const std::vector<sb::FeatureId>& ids) { selection.set(doc, ids); });
/// @endcode
class ViewWidget : public QOpenGLWidget {
    Q_OBJECT
public:
    /// Makes a view looking from Iso. Call prepareGraphics() before the QApplication
    /// is made, and check graphicsProblem() first.
    /// @param parent The widget it sits in.
    explicit ViewWidget(QWidget* parent = nullptr);
    /// Frees the view's OCCT objects.
    ~ViewWidget() override;

    /// Changes what the view shows. Bodies that didn't change are kept, so this is
    /// quick to call after every rebuild or selection change. The first time
    /// there is something to show, the camera zooms to fit it.
    /// @param content The bodies and the selection.
    void setContent(const SceneContent& content);

    /// Changes what is drawn over the design (the grid, guide lines, ...).
    /// @param overlays The overlays, drawn in this order.
    void setOverlays(const std::vector<ViewOverlay>& overlays);

    /// Changes the handles drawn on top of everything, like the arrows on a picked
    /// body. Pressing the left button on one and dragging reports how far it was
    /// dragged (handleDragged(), then handleDropped()) instead of orbiting or
    /// selecting. While drawing (see setDrawingPlane()) no handles are shown.
    /// @param handles The handles.
    void setHandles(const std::vector<ViewHandle>& handles);

    /// Gives the handles the view draws.
    /// @return The handles last given to setHandles().
    const std::vector<ViewHandle>& handles() const;

    /// Draws one body moved, as a preview while it's dragged, without making its
    /// drawing again: instant, however many faces it has. Picking follows it. It
    /// stays until clearPreview(), even when setContent() remakes the body's drawing.
    /// @param body The step whose body is drawn moved.
    /// @param motion How: turned about a line, then slid (mm, radians).
    void setPreview(const FeatureId& body, const kernel::Motion& motion);

    /// Draws one component of an assembly moved (all its bodies), as a preview while
    /// it's dragged, like setPreview(). It takes the place of a body's preview.
    /// @param component The component drawn moved.
    /// @param motion How: turned about a line, then slid (mm, radians), after its placement.
    void setComponentPreview(const InstanceId& component, const kernel::Motion& motion);

    /// Draws several components of an assembly moved together, the same way, like
    /// setComponentPreview(): a hinged lid and what's on it.
    /// @param components The components drawn moved.
    /// @param motion How: turned about a line, then slid (mm, radians), after their placements.
    void setComponentPreview(const std::vector<InstanceId>& components, const kernel::Motion& motion);

    /// Draws every body where setContent() put it again.
    void clearPreview();

    /// Shows flat squares to click, like the three base planes offered when
    /// starting a sketch, in place of the ones shown. The one under the mouse
    /// lights up; clicking it sends choiceClicked(), Escape sends
    /// choicesCancelled(). A left drag that starts on one still orbits. Clicks
    /// elsewhere pick as usual.
    /// @param choices The squares; empty to show none.
    void setChoices(const std::vector<ViewChoice>& choices);

    /// Gives the squares shown to click.
    /// @return The choices last given to setChoices().
    const std::vector<ViewChoice>& choices() const;

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

    /// Gives where a point is drawn in the view, for tests that drag a handle.
    /// @param point A point, in mm.
    /// @return Where, in widget coordinates, or nothing before the view has drawn.
    std::optional<QPoint> toScreen(const kernel::Vec3& point) const;

    /// Gives how many overlays the view draws.
    /// @return The number of overlays.
    int overlayCount() const;

    /// Turns the camera to look from a standard direction, and zooms to fit.
    /// @param view The direction.
    void setStandardView(StandardView view);

    /// Zooms and pans so every body fits in the view.
    void fitAll();

    /// Gives the camera's current direction.
    /// @return Unit vectors, from the point looked at towards the camera, and up.
    ViewDirection camera() const;

    /// Gives where the camera is and how far it's zoomed, to put back later with
    /// setCameraPlace().
    /// @return The camera's place, or one with a scale of 0 before the view has drawn.
    CameraPlace cameraPlace() const;

    /// Puts the camera back where cameraPlace() said it was. The view doesn't zoom
    /// to fit afterwards, even the first time it has something to show.
    /// @param place The place; one with a scale of 0 is ignored.
    void setCameraPlace(const CameraPlace& place);

    /// Zooms to fit the next time setContent() has something to show, as a new
    /// view does: call it before showing another design whose camera isn't known yet.
    void fitWhenShown();

    /// Gives what the view shows.
    /// @return The content last given to setContent().
    const SceneContent& content() const;

    /// Gives how many bodies the view draws (bodies whose shape it could use).
    /// @return The number of bodies.
    int shownCount() const;

    /// Checks whether OpenGL started and the view has drawn.
    /// @return True after the first successful draw.
    bool ready() const;

    /// Gives why the view can't draw, if it can't.
    /// @return Empty, or a plain sentence saying what went wrong and what to do.
    QString problem() const;

    /// Starts or stops drawing mode. While drawing, a left click reports the
    /// point on the plane (planeClicked()) instead of orbiting or selecting,
    /// moving the mouse reports it too (planeMoved()), and a left drag reports
    /// where it started and where it is (planeDragged(), then planeDropped()).
    /// Panning and zooming still work, and Escape sends planeCancelled().
    /// @param plane The plane to draw on, or nothing to stop drawing.
    void setDrawingPlane(std::optional<DrawingPlane> plane);

    /// Turns the camera to look along a direction, and zooms to fit.
    /// @param direction From the point looked at towards the camera, and up.
    void lookAlong(const ViewDirection& direction);

    /// Changes what a left drag does: orbit (the usual), pan or zoom. Right and
    /// middle drags still pan, Shift+left drag pans, and the wheel zooms. A left
    /// click still picks.
    /// @param drag What it does.
    void setLeftDrag(LeftDrag drag);

    /// Gives what a left drag does.
    /// @return The choice last given to setLeftDrag(); Orbit at first.
    LeftDrag leftDrag() const;

    /// Changes how bodies are drawn: shaded with edges (the usual), shaded, or
    /// wireframe. Ghosts of used-up steps stay see-through either way.
    /// @param style The style.
    void setDisplayStyle(DisplayStyle style);

    /// Gives how bodies are drawn.
    /// @return The style last given to setDisplayStyle(); ShadedWithEdges at first.
    DisplayStyle displayStyle() const;

    /// Selects as if the student clicked at a point (for tests). It runs on the
    /// next draw, like a real click.
    /// @param point Where, in widget coordinates.
    /// @param add True for a Ctrl-click (add or remove), false for a plain click.
    void clickAt(QPoint point, bool add = false);

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

    /// The student is dragging a handle (see setHandles()). Sent on every move
    /// until handleDropped() or handleCancelled().
    /// @param id The handle's id.
    /// @param by How far it has been dragged since the button went down, along x, y and z (mm).
    ///        For a ring, x is how far it has turned (radians), carrying on past
    ///        half a turn as it goes round.
    void handleDragged(const std::string& id, const kernel::Vec3& by);

    /// A handle drag ended: the left button came up.
    /// @param id The handle's id.
    /// @param by How far it was dragged, along x, y and z (mm); for a ring, x is
    ///        how far it turned (radians).
    void handleDropped(const std::string& id, const kernel::Vec3& by);

    /// The student pressed Escape while dragging a handle: put back what the drag changed.
    void handleCancelled();

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

    /// The student clicked in the view and the selection should change.
    /// @param selection The steps that should be selected, in picking order.
    void picked(const std::vector<sb::FeatureId>& selection);

    /// Components of an assembly were picked by clicking (Ctrl or Shift adds or
    /// takes away), in picking order: the selection the view wants. Like picked(),
    /// the view doesn't change its own highlight until setContent().
    /// @param components The components, in picking order; empty after a click on empty space.
    void componentsPicked(const std::vector<sb::InstanceId>& components);

    /// Edges were picked by clicking, while SceneContent::pickEdges is on: the
    /// edge clicked, or with Ctrl or Shift held, the edges picked before with the
    /// one clicked added or taken away.
    /// @param picks The picked edges, in order; empty when empty space was clicked.
    void edgesPicked(const std::vector<sb::view::ScenePick>& picks);

    /// A face was picked by clicking, while SceneContent::pickFaces is on: the
    /// face clicked (one face is picked at a time).
    /// @param picks The picked face, or empty when empty space was clicked.
    void facesPicked(const std::vector<sb::view::ScenePick>& picks);

    /// A face, edge or point was clicked while SceneContent::pickMeasure is on.
    /// Up to two are picked: the first click picks one, the second adds another,
    /// and the third starts again with just the one clicked. Clicking a picked
    /// one again takes it away.
    /// @param picks The picked faces, edges or points, in order; empty when empty
    ///        space was clicked.
    void measurePicked(const std::vector<sb::view::ScenePick>& picks);

    /// The student clicked or focused this view (so it becomes the active pane).
    void activated();

    /// The student clicked the right mouse button without dragging (a drag pans),
    /// asking for the pane's menu.
    /// @param global Where, on the screen.
    void menuRequested(QPoint global);

    /// OpenGL couldn't start, so the view can't draw.
    /// @param why A plain sentence saying what went wrong and what to do.
    void failed(const QString& why);

protected:
    /// Wraps Qt's OpenGL context for OCCT.
    void initializeGL() override;
    /// Lets OCCT handle the queued mouse input and draw.
    void paintGL() override;
    /// Passes a mouse button press to OCCT's view controller.
    /// @param event The press.
    void mousePressEvent(QMouseEvent* event) override;
    /// Passes a mouse button release to OCCT's view controller (a click selects).
    /// @param event The release.
    void mouseReleaseEvent(QMouseEvent* event) override;
    /// Passes mouse movement to OCCT's view controller (drags turn the camera).
    /// @param event The movement.
    void mouseMoveEvent(QMouseEvent* event) override;
    /// Zooms towards the mouse.
    /// @param event The wheel turn.
    void wheelEvent(QWheelEvent* event) override;
    /// Clears the hover highlight when the mouse leaves.
    /// @param event The leave event.
    void leaveEvent(QEvent* event) override;

    /// Sends Escape to the drawing tool while drawing.
    /// @param event The key.
    void keyPressEvent(QKeyEvent* event) override;
    /// Gives the view a sensible starting size.
    /// @return 640 by 480 pixels.
    QSize sizeHint() const override;

private:
    friend class ViewController;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace sb::view
