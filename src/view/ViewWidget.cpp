#include "sb/view/ViewWidget.h"

#include <QCoreApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QPointer>
#include <QWheelEvent>

#include <AIS_AnimationCamera.hxx>
#include <AIS_InteractiveContext.hxx>
#include <AIS_Shape.hxx>
#include <AIS_ViewController.hxx>
#include <AIS_ViewCube.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_Copy.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRep_Builder.hxx>
#include <Bnd_Box.hxx>
#include <Font_NameOfFont.hxx>
#include <Graphic3d_ArrayOfPoints.hxx>
#include <Graphic3d_ArrayOfSegments.hxx>
#include <Graphic3d_AspectLine3d.hxx>
#include <Graphic3d_AspectMarker3d.hxx>
#include <Graphic3d_AspectText3d.hxx>
#include <Graphic3d_Camera.hxx>
#include <Graphic3d_Group.hxx>
#include <Graphic3d_Text.hxx>
#include <Graphic3d_TransformPers.hxx>
#include <Prs3d_Drawer.hxx>
#include <Prs3d_LineAspect.hxx>
#include <Prs3d_PointAspect.hxx>
#include <Prs3d_ShadingAspect.hxx>
#include <Quantity_Color.hxx>
#include <Standard_Failure.hxx>
#include <Standard_Version.hxx>
#include <StdSelect_BRepOwner.hxx>
#include <TopExp.hxx>
#include <TopLoc_Location.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Shape.hxx>
#include <V3d_AmbientLight.hxx>
#include <V3d_DirectionalLight.hxx>
#include <V3d_View.hxx>
#include <V3d_Viewer.hxx>
#include <gp_Ax1.hxx>
#include <gp_Ax3.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>

#include <algorithm>
#include <cmath>
#include <map>
#include <numbers>
#include <type_traits>

#include "InputBoxes.h"
#include "OcctGl.h"
#include "sb/kernel_occt/OcctShape.h"
#include "sb/view/Choices.h"
#include "sb/view/Dimensions.h"
#include "sb/view/Graphics.h"

namespace sb::view {

namespace {

Quantity_Color rgb(int r, int g, int b) {
    return Quantity_Color(r / 255.0, g / 255.0, b / 255.0, Quantity_TOC_sRGB);
}

// Colours, matching the flat view and the docs pictures.
const Quantity_Color kEdges = rgb(44, 52, 64);
const Quantity_Color kSelected = rgb(255, 140, 26);
const Quantity_Color kHover = rgb(90, 170, 255);
const Quantity_Color kSkyTop = rgb(214, 222, 233);
const Quantity_Color kSkyBottom = rgb(250, 251, 253);

// An overlay, drawn from plain lines or dots. It has no selection, so clicks go
// through it, and it isn't a shape, so Fit all ignores it.
class OverlayObject final : public AIS_InteractiveObject {
public:
    explicit OverlayObject(const ViewOverlay& o) : overlay_(o) {}

    void Compute(const Handle(PrsMgr_PresentationManager) &, const Handle(Prs3d_Presentation) & prs,
                 const Standard_Integer) override {
        const auto& o = overlay_;
        const Quantity_ColorRGBA color(rgb(o.color.r, o.color.g, o.color.b),
                                       static_cast<float>(std::clamp(o.alpha, 0, 255) / 255.0));
        Handle(Graphic3d_Group) group = prs->NewGroup();
        const bool dots = o.shape == OverlayShape::Points || (o.shape == OverlayShape::Lattice && o.dots);
        if (o.shape == OverlayShape::Labels) {
            // Words that always face the screen and keep their size in pixels.
            Handle(Graphic3d_AspectText3d) aspect = new Graphic3d_AspectText3d();
            aspect->SetColor(color.GetRGB());
            aspect->SetFont(Font_NOF_SANS_SERIF);
            aspect->SetTextFontAspect(Font_FA_Bold);
            if (o.boxed) {
                // On a box of the background's color, so a dimension's line doesn't run through the words.
                aspect->SetDisplayType(Aspect_TODT_SUBTITLE);
                aspect->SetColorSubTitle(rgb(240, 243, 247));
            }
            group->SetGroupPrimitivesAspect(aspect);
            const std::size_t n = std::min(o.points.size(), o.texts.size());
            for (std::size_t i = 0; i < n; ++i) {
                Handle(Graphic3d_Text) text =
                    new Graphic3d_Text(static_cast<Standard_ShortReal>(std::max(6.0, o.size)));
                text->SetText(o.texts[i].c_str());
                text->SetPosition(gp_Pnt(o.points[i].x, o.points[i].y, o.points[i].z));
                text->SetHorizontalAlignment(Graphic3d_HTA_CENTER);
                text->SetVerticalAlignment(Graphic3d_VTA_CENTER);
                group->AddText(text);
            }
        } else if (dots) {
            const auto points = o.shape == OverlayShape::Points ? o.points : latticePoints(o);
            if (points.empty()) return;
            Handle(Graphic3d_ArrayOfPoints) array =
                new Graphic3d_ArrayOfPoints(static_cast<int>(points.size()));
            for (const auto& p : points) array->AddVertex(p.x, p.y, p.z);
            Handle(Graphic3d_AspectMarker3d) aspect =
                new Graphic3d_AspectMarker3d(Aspect_TOM_POINT, color.GetRGB(), std::max(1.0, o.size));
            aspect->SetInteriorColor(color);  // with its alpha
            if (o.alpha < 255) aspect->SetAlphaMode(Graphic3d_AlphaMode_Blend);
            group->SetGroupPrimitivesAspect(aspect);
            group->AddPrimitiveArray(array);
        } else {
            const auto points = o.shape == OverlayShape::Lines ? o.points : latticeLines(o);
            if (points.size() < 2) return;
            const int count = static_cast<int>(points.size() / 2 * 2);
            Handle(Graphic3d_ArrayOfSegments) array = new Graphic3d_ArrayOfSegments(count);
            for (int i = 0; i < count; ++i) {
                const auto& p = points[static_cast<std::size_t>(i)];
                array->AddVertex(p.x, p.y, p.z);
            }
            Handle(Graphic3d_AspectLine3d) aspect =
                new Graphic3d_AspectLine3d(color.GetRGB(), Aspect_TOL_SOLID, std::max(1.0, o.size));
            aspect->SetInteriorColor(color);  // with its alpha
            if (o.alpha < 255) aspect->SetAlphaMode(Graphic3d_AlphaMode_Blend);
            group->SetGroupPrimitivesAspect(aspect);
            group->AddPrimitiveArray(array);
        }
    }
    void ComputeSelection(const Handle(SelectMgr_Selection) &, const Standard_Integer) override {}

private:
    ViewOverlay overlay_;
};

// Pixels, before scaling for high-DPI screens. OCCT puts the cube's sides 15% of
// this further out than half of it (20 + 6 = kViewCubeHalfSize), and bevels the
// edges back by that 15% (kViewCubeBevel).
constexpr double kCubeSize = 40.0;
static_assert(kCubeSize * 13 / 20 == kViewCubeHalfSize);
static_assert(kCubeSize * 3 / 20 == kViewCubeBevel);

// Places something in the view's top-right corner, turning with the view but not
// panning or zooming, where the view cube is.
Handle(Graphic3d_TransformPers) atTheCube(double ratio) {
    const int offset = static_cast<int>(kViewCubeOffset * ratio);
    return new Graphic3d_TransformPers(Graphic3d_TMF_TriedronPers, Aspect_TOTP_RIGHT_UPPER,
                                       Graphic3d_Vec2i(offset, offset));
}

Aspect_VKeyMouse toButtons(Qt::MouseButtons b) {
    Aspect_VKeyMouse out = Aspect_VKeyMouse_NONE;
    if (b & Qt::LeftButton) out |= Aspect_VKeyMouse_LeftButton;
    if (b & Qt::MiddleButton) out |= Aspect_VKeyMouse_MiddleButton;
    if (b & Qt::RightButton) out |= Aspect_VKeyMouse_RightButton;
    return out;
}

Aspect_VKeyFlags toFlags(Qt::KeyboardModifiers m) {
    Aspect_VKeyFlags out = Aspect_VKeyFlags_NONE;
    if (m & Qt::ShiftModifier) out |= Aspect_VKeyFlags_SHIFT;
    if (m & Qt::ControlModifier) out |= Aspect_VKeyFlags_CTRL;
    if (m & Qt::AltModifier) out |= Aspect_VKeyFlags_ALT;
    if (m & Qt::MetaModifier) out |= Aspect_VKeyFlags_META;
    return out;
}

// The message for the student, plus what went wrong where, for SketchBillet's
// developers (shown in small print on the flat view's card).
QString drawFailed(const QString& detail) {
    return QCoreApplication::translate("sb::view",
                                       "The 3D view couldn't start OpenGL on this computer. Update the "
                                       "graphics driver (in a virtual machine, "
                                       "turn on 3D acceleration), then restart SketchBillet.") +
           QStringLiteral("\n\n") +
           QCoreApplication::translate("sb::view", "Details for SketchBillet's developers: %1").arg(detail);
}

// A Qt window id as OCCT's window handle: a pointer (HWND) on Windows, a
// number on macOS and Linux.
template <typename Drawable = Aspect_Drawable>
Drawable toDrawable(WId id) {
    // A template, so the branch that doesn't apply here isn't compiled.
    if constexpr (std::is_pointer_v<Drawable>) {
        return reinterpret_cast<Drawable>(id);
    } else {
        return static_cast<Drawable>(id);
    }
}

// "Standard_ProgramError: <message>", from an OCCT exception.
QString failureText(const Standard_Failure& e) {
    const QString type = QString::fromLatin1(e.DynamicType()->Name());
    const QString message = QString::fromUtf8(e.GetMessageString());
    return message.isEmpty() ? type : type + QStringLiteral(": ") + message;
}

}  // namespace

// A mouse button plus keys held, as OCCT's gesture maps want them.
static unsigned int with(Aspect_VKeyMouse button, Aspect_VKeyFlags keys) {
    return static_cast<unsigned int>(button) | static_cast<unsigned int>(keys);
}

// OCCT's mouse handling for one view. It turns clicks into picks and asks for
// more frames while the camera animates.
class ViewController final : public AIS_ViewController {
public:
    explicit ViewController(ViewWidget& widget) : widget_(widget) {
        setLeftDrag(LeftDrag::Orbit);
        // A click selects; Ctrl- or Shift-click adds or removes.
        auto& schemes = ChangeMouseSelectionSchemes();
        schemes.Clear();
        schemes.Bind(Aspect_VKeyMouse_LeftButton, AIS_SelectionScheme_Replace);
        schemes.Bind(with(Aspect_VKeyMouse_LeftButton, Aspect_VKeyFlags_CTRL), AIS_SelectionScheme_XOR);
        schemes.Bind(with(Aspect_VKeyMouse_LeftButton, Aspect_VKeyFlags_SHIFT), AIS_SelectionScheme_XOR);
        SetLockOrbitZUp(true);  // Z stays up while orbiting, so the floor stays the floor
    }

    // Left drag orbits (or pans, or zooms: the view bar's choice); right,
    // middle or Shift+left drag pans; the wheel zooms.
    void setLeftDrag(LeftDrag drag) {
        AIS_MouseGesture left = AIS_MouseGesture_RotateOrbit;
        if (drag == LeftDrag::Pan) left = AIS_MouseGesture_Pan;
        if (drag == LeftDrag::Zoom) left = AIS_MouseGesture_Zoom;
        auto& gestures = ChangeMouseGestureMap();
        gestures.Clear();
        gestures.Bind(Aspect_VKeyMouse_LeftButton, left);
        gestures.Bind(with(Aspect_VKeyMouse_LeftButton, Aspect_VKeyFlags_CTRL), left);
        gestures.Bind(with(Aspect_VKeyMouse_LeftButton, Aspect_VKeyFlags_SHIFT), AIS_MouseGesture_Pan);
        gestures.Bind(Aspect_VKeyMouse_MiddleButton, AIS_MouseGesture_Pan);
        gestures.Bind(Aspect_VKeyMouse_RightButton, AIS_MouseGesture_Pan);
        leftDrag_ = drag;
    }
    LeftDrag leftDrag() const { return leftDrag_; }

    void OnSelectionChanged(const Handle(AIS_InteractiveContext) & ctx,
                            const Handle(V3d_View) & view) override;

protected:
    void handleViewRedraw(const Handle(AIS_InteractiveContext) & ctx,
                          const Handle(V3d_View) & view) override {
        AIS_ViewController::handleViewRedraw(ctx, view);
        if (toAskNextFrame()) {
            // The camera is still moving (the view cube's turn): draw again soon.
            QPointer<ViewWidget> w(&widget_);
            QMetaObject::invokeMethod(
                &widget_,
                [w] {
                    if (w) w->update();
                },
                Qt::QueuedConnection);
        }
    }

private:
    ViewWidget& widget_;
    LeftDrag leftDrag_ = LeftDrag::Orbit;
};

struct ViewWidget::Impl {
    explicit Impl(ViewWidget& w) : controller(w) {}

    // A drawn body: its step, and in an assembly the component it's drawn for.
    struct BodyKey {
        FeatureId feature;
        InstanceId component;
        auto operator<=>(const BodyKey&) const = default;
    };

    struct Shown {
        std::uint64_t shapeId = 0;  // the ShapeHandle it was made from
        bool ghost = false;
        bool context = false;  // faded, and not picked: see SceneBody::context
        Rgb color;
        Handle(AIS_Shape) object;
        kernel::Placement placement;  // where it's drawn (a component's place)
        TopoDS_Shape original;        // the design's shape, placed
        TopoDS_Shape drawn;           // the view's copy of it, meshed
        // The drawn copy's edges and faces, numbered as the design's shape numbers
        // them (kernel::IKernel::elements()), so a picked one's number is the design's.
        TopTools_IndexedMapOfShape edges;
        TopTools_IndexedMapOfShape faces;
        TopTools_IndexedMapOfShape vertices;
    };

    ViewController controller;
    Handle(V3d_Viewer) viewer;
    Handle(AIS_InteractiveContext) context;
    Handle(V3d_View) view;
    Handle(AIS_ViewCube) cube;
    std::map<BodyKey, Shown> shown;
    SceneContent content;
    Handle(AIS_Shape) pickedEdges;   // the picked edges, drawn thick on top of their bodies
    Handle(AIS_Shape) pickedFaces;   // the picked faces, drawn in the picked color
    Handle(AIS_Shape) pickedPoints;  // the picked points, drawn as big dots in the picked color
    std::vector<Handle(OverlayObject)> overlays;
    std::vector<ViewHandle> handles;       // as given (lengths may be in pixels)
    std::vector<ViewHandle> shownHandles;  // the same, lengths in mm, as last drawn
    double handleScale = 0;                // mm per pixel where they were last drawn
    std::vector<Handle(OverlayObject)> handleObjects;
    std::vector<ViewChoice> choices;       // as given (sizes may be in pixels)
    std::vector<ViewChoice> shownChoices;  // the same, sizes in mm, as last drawn
    double choiceScale = 0;                // mm per pixel where they were last drawn
    std::vector<Handle(AIS_InteractiveObject)> choiceObjects;
    std::optional<std::size_t> hoveredChoice;   // the choice under the mouse
    std::optional<std::size_t> pressedChoice;   // the choice the left button went down on
    QPoint choicePress;                         // where it went down
    std::optional<std::size_t> hovered;         // the handle under the mouse
    std::optional<std::size_t> draggingHandle;  // the handle being dragged
    ViewHandle dragHandle;                      // the handle as it was when pressed (it may follow the body)
    Ray dragStart;                              // the line of sight when it was pressed
    kernel::Vec3 dragBy{};                      // how far it has been dragged
    bool fitPending = true;
    bool everShown = false;                  // has the view had a body to fit yet?
    std::optional<CameraPlace> placeLater;   // a camera place given before the view was made
    InputBoxes* inputs = nullptr;            // boxes to type numbers in (setInputs())
    std::vector<ViewOverlay> givenOverlays;  // as given, with Dimension overlays not yet worked out
    double dimensionScale = 0;               // mm per pixel the dimensions were last drawn at
    ScreenAxes dimensionScreen;              // which way the screen faced when they were
    bool ready = false;
    bool clickAdds = false;                     // the last click had Ctrl or Shift held
    std::optional<FeatureId> previewBody;       // a body drawn moved (setPreview())
    std::vector<InstanceId> previewComponents;  // or components (setComponentPreview())
    gp_Trsf previewMove;                        // how

    // Draws the previewed body moved, and every other body where it is.
    void applyPreview() {
        for (auto& [id, s] : shown) {
            const bool moved =
                (previewBody && *previewBody == id.feature && id.component.isNil()) ||
                (!id.component.isNil() && std::find(previewComponents.begin(), previewComponents.end(),
                                                    id.component) != previewComponents.end());
            if (moved || s.object->HasTransformation())
                context->SetLocation(s.object, moved ? TopLoc_Location(previewMove) : TopLoc_Location());
        }
    }
    DisplayStyle style = DisplayStyle::ShadedWithEdges;

    // Draws a body in the display style. Ghosts (bodies used up by a later
    // step, shown while picked) stay see-through shaded, so they read as ghosts.
    void applyStyle(const Handle(AIS_Shape) & obj, bool ghost) {
        if (ghost) {
            context->SetDisplayMode(obj, AIS_Shaded, false);
            return;
        }
        obj->Attributes()->SetFaceBoundaryDraw(style == DisplayStyle::ShadedWithEdges);
        context->SetDisplayMode(obj, style == DisplayStyle::Wireframe ? AIS_WireFrame : AIS_Shaded, false);
        context->Redisplay(obj, false);
    }
    QString problem;

    void place(const CameraPlace& p) {
        const Handle(Graphic3d_Camera)& cam = view->Camera();
        cam->SetEyeAndCenter(gp_Pnt(p.eye.x, p.eye.y, p.eye.z), gp_Pnt(p.at.x, p.at.y, p.at.z));
        cam->SetUp(gp_Dir(p.up.x, p.up.y, p.up.z));
        cam->SetScale(p.scale);
        view->ZFitAll();
        fitPending = false;
    }

    void fit() {
        Bnd_Box box;
        for (const auto& [id, s] : shown) BRepBndLib::Add(s.object->Shape(), box);
        if (box.IsVoid()) box.Update(-50, -50, 0, 50, 50, 30);  // around the origin
        view->FitAll(box, 0.12, false);
        view->ZFitAll();
        fitPending = false;
    }

    QString glInfo;     // the graphics card and OpenGL version, for failure details
    QPoint rightPress;  // where the right button went down, to tell a click from a drag
    // Drawing mode: the plane, and where the left button went down (a click, not a drag).
    std::optional<DrawingPlane> drawing;
    QPoint leftPress;
    bool leftDown = false;
    bool dragging = false;                 // moved far enough with the left button down to be a drag
    std::optional<PlanePoint> pressPoint;  // where the left button went down, on the plane

    // The point under the mouse on the drawing plane, and how big a pixel is there.
    std::optional<PlanePoint> planePoint(QPointF at, double ratio) const {
        if (!drawing || !view) return std::nullopt;
        auto hit = [&](double x, double y) -> std::optional<kernel::Vec3> {
            Standard_Real px = 0, py = 0, pz = 0, vx = 0, vy = 0, vz = 0;
            view->ConvertWithProj(static_cast<int>(std::lround(x)), static_cast<int>(std::lround(y)), px, py,
                                  pz, vx, vy, vz);
            return rayHitsPlane({px, py, pz}, {vx, vy, vz}, *drawing);
        };
        const double x = at.x() * ratio, y = at.y() * ratio;
        const auto here = hit(x, y);
        const auto next = hit(x + ratio, y);  // one screen pixel to the right
        if (!here || !next) return std::nullopt;
        const double pixel = std::hypot(next->x - here->x, next->y - here->y, next->z - here->z);
        return PlanePoint{*here, pixel};
    }

    // The line of sight through a point of the widget (in device pixels).
    Ray rayAt(double x, double y) const {
        Standard_Real px = 0, py = 0, pz = 0, vx = 0, vy = 0, vz = 0;
        view->ConvertWithProj(static_cast<int>(std::lround(x)), static_cast<int>(std::lround(y)), px, py, pz,
                              vx, vy, vz);
        return {{px, py, pz}, {vx, vy, vz}};
    }

    // Where a point is drawn, in device pixels.
    Projection projection() const {
        return [this](const kernel::Vec3& p) -> std::optional<ScreenPoint> {
            Standard_Integer x = 0, y = 0;
            view->Convert(p.x, p.y, p.z, x, y);
            return ScreenPoint{static_cast<double>(x), static_cast<double>(y)};
        };
    }

    // How many mm one device pixel covers at a point, across the screen.
    double pixelSizeAt(const kernel::Vec3& p) const {
        const auto s = projection()(p);
        if (!s) return 0;
        const gp_Dir look = view->Camera()->Direction();
        const DrawingPlane facing{p, {look.X(), look.Y(), look.Z()}};
        const Ray a = rayAt(s->x, s->y), b = rayAt(s->x + 1, s->y);
        const auto pa = rayHitsPlane(a.from, a.direction, facing);
        const auto pb = rayHitsPlane(b.from, b.direction, facing);
        if (!pa || !pb) return 0;
        return std::hypot(pb->x - pa->x, pb->y - pa->y, pb->z - pa->z);
    }

    // How many mm one (logical) pixel covers where the first dimension is; 0 if there's none.
    double currentDimensionScale(double ratio) const {
        if (!view || view->Window().IsNull()) return 0;
        for (const auto& o : givenOverlays) {
            if (o.shape == OverlayShape::Dimension && o.points.size() >= 3)
                return pixelSizeAt(o.points[2]) * ratio;
        }
        return 0;
    }

    // Which way the screen's right and up point in the design now.
    ScreenAxes screenAxes() const {
        if (!view) return {};
        const Handle(Graphic3d_Camera)& cam = view->Camera();
        const gp_Dir up = cam->Up();
        const gp_Dir look = cam->Direction();  // into the screen
        const gp_Dir right = look.Crossed(up);
        return {{right.X(), right.Y(), right.Z()}, {up.X(), up.Y(), up.Z()}};
    }

    // The handles with their lengths in mm, for drawing and picking now.
    double currentHandleScale() const {
        for (const auto& h : handles) {
            if (h.pixels) return pixelSizeAt(h.at);
        }
        return 0;
    }

    // Draws the handles again, the one under the mouse (or being dragged) brighter and thicker.
    void showHandles(double ratio) {
        if (!context) return;
        for (auto& o : handleObjects) context->Remove(o, false);
        handleObjects.clear();
        shownHandles.clear();
        if (drawing) return;  // drawing on a sketch: no handles
        handleScale = view && !view->Window().IsNull() ? currentHandleScale() : 0;
        for (const auto& given : handles) {
            ViewHandle h = given;
            if (h.pixels) {
                h.length *= handleScale * ratio;  // pixels (before high-DPI scaling) to mm
                h.pixels = false;
            }
            shownHandles.push_back(h);
        }
        for (std::size_t i = 0; i < shownHandles.size(); ++i) {
            const auto& h = shownHandles[i];
            if (h.length <= 0) continue;
            // While dragging, the handle with the dragged one's id lights up (the
            // handles may have been given again, following the body).
            const bool lit = draggingHandle ? h.id == dragHandle.id : hovered == i;
            ViewOverlay o;
            o.shape = OverlayShape::Lines;
            o.points = handleLines(h);
            o.color = lit ? Rgb{255, 200, 40} : h.color;
            o.size = (lit ? 6.0 : 4.0) * ratio;
            Handle(OverlayObject) obj = new OverlayObject(o);
            obj->SetZLayer(Graphic3d_ZLayerId_Topmost);  // over the bodies, so it can always be grabbed
            context->Display(obj, 0, -1, false);         // -1: never picked by OCCT (the view picks it)
            handleObjects.push_back(obj);
        }
    }

    // How many mm a pixel covers where the choices are, for choices sized in pixels.
    double currentChoiceScale() const {
        for (const auto& c : choices) {
            if (c.pixels) return pixelSizeAt(c.origin);
        }
        return 0;
    }

    // Draws the choices again: see-through squares with a line round them and
    // their words, the one under the mouse more solid.
    void showChoices(double ratio) {
        if (!context) return;
        for (auto& o : choiceObjects) context->Remove(o, false);
        choiceObjects.clear();
        shownChoices.clear();
        if (drawing) return;
        choiceScale = view && !view->Window().IsNull() ? currentChoiceScale() : 0;
        for (const auto& given : choices) {
            ViewChoice c = given;
            if (c.pixels) {
                c.from *= choiceScale * ratio;  // pixels (before high-DPI scaling) to mm
                c.to *= choiceScale * ratio;
                c.pixels = false;
            }
            shownChoices.push_back(c);
        }
        for (std::size_t i = 0; i < shownChoices.size(); ++i) {
            const auto& c = shownChoices[i];
            if (c.to - c.from <= 0) continue;
            const bool lit = hoveredChoice == i;
            const auto corners = choiceCorners(c);
            BRepBuilderAPI_MakePolygon outline;
            for (const auto& p : corners) outline.Add(gp_Pnt(p.x, p.y, p.z));
            outline.Close();
            BRepBuilderAPI_MakeFace face(outline.Wire(), true);
            if (!face.IsDone()) continue;
            Handle(AIS_Shape) square = new AIS_Shape(face.Face());
            square->SetColor(rgb(c.color.r, c.color.g, c.color.b));
            square->SetTransparency(lit ? 0.35 : 0.7);
            context->Display(square, AIS_Shaded, -1, false);  // -1: never picked by OCCT (the view picks it)
            choiceObjects.push_back(square);
            ViewOverlay o;
            o.shape = OverlayShape::Lines;
            for (std::size_t k = 0; k < 4; ++k) {
                o.points.push_back(corners[k]);
                o.points.push_back(corners[(k + 1) % 4]);
            }
            o.color = c.color;
            o.size = (lit ? 3.0 : 1.5) * ratio;
            Handle(OverlayObject) edge = new OverlayObject(o);
            context->Display(edge, 0, -1, false);
            choiceObjects.push_back(edge);
            if (!c.label.empty()) {
                ViewOverlay words;
                words.shape = OverlayShape::Labels;
                words.points = {choiceLabelAt(c)};
                words.texts = {c.label};
                words.color = Rgb{30, 34, 40};
                words.size = (lit ? 15.0 : 13.0) * ratio;
                Handle(OverlayObject) label = new OverlayObject(words);
                label->SetZLayer(Graphic3d_ZLayerId_Topmost);
                context->Display(label, 0, -1, false);
                choiceObjects.push_back(label);
            }
        }
    }

    // The choice under the mouse, if any.
    std::optional<std::size_t> choiceUnder(QPointF at, double r) const {
        if (shownChoices.empty() || !ready) return std::nullopt;
        const ScreenPoint mouse{at.x() * r, at.y() * r};
        return choiceAt(shownChoices, projection(), mouse, 6 * r, rayAt(mouse.x, mouse.y));
    }

    // A failure's details, with the graphics card and OpenGL version when known.
    QString where(const QString& what) const {
        return glInfo.isEmpty() ? what : what + QStringLiteral(" [") + glInfo + QStringLiteral("]");
    }

    void fail(ViewWidget& w, const QString& why) {
        if (!problem.isEmpty()) return;
        problem = why;
        ready = false;
        emit w.failed(why);
    }
};

void ViewController::OnSelectionChanged(const Handle(AIS_InteractiveContext) & ctx,
                                        const Handle(V3d_View) &) {
    auto& d = *widget_.impl_;
    if (d.content.pickMeasure) {
        // The face, edge or point clicked: whichever is under the mouse.
        std::vector<ScenePick> clicked;
        for (ctx->InitSelected(); ctx->MoreSelected(); ctx->NextSelected()) {
            const auto owner = Handle(StdSelect_BRepOwner)::DownCast(ctx->SelectedOwner());
            if (owner.IsNull() || !owner->HasShape()) continue;
            const auto type = owner->Shape().ShapeType();
            const auto kind = type == TopAbs_FACE   ? kernel::TopoKind::Face
                              : type == TopAbs_EDGE ? kernel::TopoKind::Edge
                                                    : kernel::TopoKind::Vertex;
            for (const auto& [id, s] : d.shown) {
                if (s.object != ctx->SelectedInteractive()) continue;
                const auto& map = kind == kernel::TopoKind::Face   ? s.faces
                                  : kind == kernel::TopoKind::Edge ? s.edges
                                                                   : s.vertices;
                const int at = map.FindIndex(owner->Shape());
                if (at > 0) clicked.push_back({id.feature, at - 1, kind, id.component});
            }
        }
        if (clicked.size() > 1) clicked.resize(1);
        std::vector<ScenePick> now;
        if (!clicked.empty()) {
            now = d.content.picked;
            if (auto it = std::find(now.begin(), now.end(), clicked.front()); it != now.end()) {
                now.erase(it);  // clicked again: not picked any more
            } else {
                if (now.size() >= 2) now.clear();  // a third: start again
                now.push_back(clicked.front());
            }
        }
        if (now == d.content.picked) return;
        QPointer<ViewWidget> w(&widget_);
        QMetaObject::invokeMethod(
            &widget_,
            [w, now] {
                if (w) emit w->measurePicked(now);
            },
            Qt::QueuedConnection);
        return;
    }
    if (d.content.pickEdges || d.content.pickFaces) {
        // The edge or face clicked: which body's, and its number there.
        const bool faces = d.content.pickFaces;
        const auto kind = faces ? kernel::TopoKind::Face : kernel::TopoKind::Edge;
        std::vector<ScenePick> clicked;
        for (ctx->InitSelected(); ctx->MoreSelected(); ctx->NextSelected()) {
            const auto owner = Handle(StdSelect_BRepOwner)::DownCast(ctx->SelectedOwner());
            if (owner.IsNull() || !owner->HasShape()) continue;
            for (const auto& [id, s] : d.shown) {
                if (s.object != ctx->SelectedInteractive()) continue;
                const int at = (faces ? s.faces : s.edges).FindIndex(owner->Shape());
                if (at > 0) clicked.push_back({id.feature, at - 1, kind, id.component});
            }
        }
        if (faces) {
            // One face at a time: the one clicked, or none.
            if (clicked.size() > 1) clicked.resize(1);
            if (clicked == d.content.picked && !clicked.empty()) return;
            QPointer<ViewWidget> w(&widget_);
            QMetaObject::invokeMethod(
                &widget_,
                [w, clicked] {
                    if (w) emit w->facesPicked(clicked);
                },
                Qt::QueuedConnection);
            return;
        }
        // Ctrl or Shift adds the edge, or takes it away if it was picked.
        std::vector<ScenePick> now = d.clickAdds ? d.content.picked : std::vector<ScenePick>{};
        for (const auto& c : clicked) {
            if (auto it = std::find(now.begin(), now.end(), c); it != now.end()) {
                now.erase(it);
            } else {
                now.push_back(c);
            }
        }
        if (now == d.content.picked && !clicked.empty()) return;
        QPointer<ViewWidget> w(&widget_);
        QMetaObject::invokeMethod(
            &widget_,
            [w, now] {
                if (w) emit w->edgesPicked(now);
            },
            Qt::QueuedConnection);
        return;
    }
    std::vector<FeatureId> now;
    std::vector<InstanceId> components;
    for (ctx->InitSelected(); ctx->MoreSelected(); ctx->NextSelected()) {
        const Handle(AIS_InteractiveObject) obj = ctx->SelectedInteractive();
        for (const auto& [id, s] : d.shown) {
            if (s.object != obj) continue;
            if (id.component.isNil())
                now.push_back(id.feature);
            else if (std::find(components.begin(), components.end(), id.component) == components.end())
                components.push_back(id.component);
        }
    }
    // An assembly: clicking picks components.
    const bool assembly = std::any_of(d.shown.begin(), d.shown.end(),
                                      [](const auto& entry) { return !entry.first.component.isNil(); });
    if (assembly) {
        auto mergedComponents = mergePick(d.content.selectedComponents, components);
        if (mergedComponents == d.content.selectedComponents) return;
        QPointer<ViewWidget> w(&widget_);
        QMetaObject::invokeMethod(
            &widget_,
            [w, mergedComponents] {
                if (w) emit w->componentsPicked(mergedComponents);
            },
            Qt::QueuedConnection);
        return;
    }
    // Ctrl-click keeps selected steps this view doesn't draw (one that failed to build, say).
    if (d.clickAdds) {
        for (const auto& id : d.content.selected) {
            if (!d.shown.count(ViewWidget::Impl::BodyKey{id, {}})) now.push_back(id);
        }
    }
    auto merged = mergePick(d.content.selected, now);
    if (merged == d.content.selected) return;
    // Tell the world after OCCT has finished handling the click.
    QPointer<ViewWidget> w(&widget_);
    QMetaObject::invokeMethod(
        &widget_,
        [w, merged] {
            if (w) emit w->picked(merged);
        },
        Qt::QueuedConnection);
}

ViewWidget::ViewWidget(QWidget* parent) : QOpenGLWidget(parent), impl_(std::make_unique<Impl>(*this)) {
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setMinimumSize(120, 90);
    auto& d = *impl_;
    // Boxes to type in sit where their points are drawn (in logical pixels).
    d.inputs = new InputBoxes(this, [this](const kernel::Vec3& at) -> std::optional<QPointF> {
        auto& i = *impl_;
        if (!i.view || i.view->Window().IsNull()) return std::nullopt;
        const auto s = i.projection()(at);
        if (!s) return std::nullopt;
        const double ratio = devicePixelRatioF();
        return QPointF(s->x / ratio, s->y / ratio);
    });
    d.inputs->typed = [this](const std::string& id, const QString& text) { emit inputTyped(id, text); };
    d.inputs->entered = [this](const std::string& id) {
        const std::string copy = id;  // the boxes may change while it's handled
        emit inputEntered(copy);
    };
    d.inputs->cancelled = [this] { emit inputsCancelled(); };
    d.problem = graphicsProblem();
    if (!d.problem.isEmpty()) return;
    try {
        d.viewer = new V3d_Viewer(detail::sharedDriver());
        // Soft light all round, plus a lamp over the student's left shoulder that
        // turns with the camera, so the faces of a box come out in different shades.
        d.viewer->AddLight(new V3d_AmbientLight(Quantity_NOC_WHITE));
        d.viewer->AddLight(new V3d_DirectionalLight(gp_Dir(0.45, -0.6, -1.0), Quantity_NOC_WHITE, true));
        d.viewer->SetLightOn();
        d.context = new AIS_InteractiveContext(d.viewer);
        // Selected bodies turn orange, and the body under the mouse light blue: whole
        // faces, not just their edges.
        for (auto type : {Prs3d_TypeOfHighlight_Selected, Prs3d_TypeOfHighlight_LocalSelected,
                          Prs3d_TypeOfHighlight_Dynamic, Prs3d_TypeOfHighlight_LocalDynamic}) {
            const bool selected =
                type == Prs3d_TypeOfHighlight_Selected || type == Prs3d_TypeOfHighlight_LocalSelected;
            d.context->HighlightStyle(type)->SetColor(selected ? kSelected : kHover);
            d.context->HighlightStyle(type)->SetDisplayMode(AIS_Shaded);
        }

        d.view = d.viewer->CreateView();
        d.view->SetImmediateUpdate(false);
        d.view->SetBgGradientColors(kSkyTop, kSkyBottom, Aspect_GFM_VER, false);
        d.view->ChangeRenderingParams().NbMsaaSamples = 4;
        d.controller.ViewAnimation()->SetView(d.view);

        setStandardView(StandardView::Iso);
    } catch (const Standard_Failure& e) {
        d.problem = drawFailed(QStringLiteral("making the viewer: ") + failureText(e));
        d.view.Nullify();
        d.context.Nullify();
        d.viewer.Nullify();
    }
}

ViewWidget::~ViewWidget() {
    // OCCT frees OpenGL resources as the view goes, so make Qt's context current.
    if (impl_->view && context()) {
        makeCurrent();
        try {
            impl_->context->RemoveAll(false);
            impl_->view->Remove();
        } catch (const Standard_Failure&) {
        }
        doneCurrent();
    }
}

void ViewWidget::setContent(const SceneContent& content) {
    auto& d = *impl_;
    d.content = content;
    if (!d.context) return;
    try {
        std::map<Impl::BodyKey, Impl::Shown> next;
        for (const auto& body : content.bodies) {
            const TopoDS_Shape* given = kernel::occtShape(body.shape);
            if (!given || given->IsNull()) continue;
            const Impl::BodyKey key{body.feature, body.component};
            auto it = d.shown.find(key);
            if (it != d.shown.end() && it->second.shapeId == body.shape.id() &&
                it->second.ghost == body.ghost && it->second.context == body.context &&
                it->second.color == body.color &&
                kernel::samePlacement(it->second.placement, body.placement, 0)) {
                next.insert(d.shown.extract(it));
                continue;
            }
            // Where it's drawn: a component's place in an assembly. The same shape
            // placed again shares its faces, so its drawing is reused below.
            TopoDS_Shape placedShape = *given;
            if (!kernel::samePlacement(body.placement, kernel::Placement{}, 0)) {
                const auto& p = body.placement;
                const gp_Ax3 to(gp_Pnt(p.origin.x, p.origin.y, p.origin.z),
                                gp_Dir(p.zAxis.x, p.zAxis.y, p.zAxis.z),
                                gp_Dir(p.xAxis.x, p.xAxis.y, p.xAxis.z));
                gp_Trsf place;
                place.SetDisplacement(gp_Ax3(), to);
                placedShape = given->Moved(TopLoc_Location(place));
            }
            const TopoDS_Shape* shape = &placedShape;
            // Draw a copy: the view meshes what it draws, and the design's own shape
            // must not change (exports mesh it too, and must not depend on the view).
            TopoDS_Shape drawn;
            TopTools_IndexedMapOfShape edges, faces, vertices;
            // A body that's only moved (a Move step) shares its faces with one drawn
            // already: draw that copy moved, so nothing is copied or meshed again.
            for (const auto& [shownId, was] : d.shown) {
                if (was.original.IsNull() || !was.original.IsPartner(*shape) ||
                    was.original.Orientation() != shape->Orientation())
                    continue;
                const TopLoc_Location moved = shape->Location() * was.original.Location().Inverted();
                drawn = was.drawn.Moved(moved);
                for (auto [from, into] : {std::pair{&was.edges, &edges}, std::pair{&was.faces, &faces},
                                          std::pair{&was.vertices, &vertices}}) {
                    for (int i = 1; i <= from->Extent(); ++i) into->Add(from->FindKey(i).Moved(moved));
                }
                break;
            }
            if (drawn.IsNull()) {
                BRepBuilderAPI_Copy copier(*shape, false, false);
                drawn = copier.Shape();
                for (auto [type, into] : {std::pair{TopAbs_EDGE, &edges}, std::pair{TopAbs_FACE, &faces},
                                          std::pair{TopAbs_VERTEX, &vertices}}) {
                    TopTools_IndexedMapOfShape original;
                    TopExp::MapShapes(*shape, type, original);
                    for (int i = 1; i <= original.Extent(); ++i) into->Add(copier.ModifiedShape(original(i)));
                }
            }
            Handle(AIS_Shape) obj = new AIS_Shape(drawn);
            obj->SetColor(rgb(body.color.r, body.color.g, body.color.b));
            obj->Attributes()->SetDeviationAngle(0.1);  // radians: smooth round edges
            // Not a change: OCCT would throw away a reused drawing's mesh and make it again.
            obj->Attributes()->UpdatePreviousDeviationAngle();
            if (body.context) {
                obj->SetTransparency(0.85);  // the rest of the assembly, faded
            } else if (body.ghost) {
                obj->SetTransparency(0.7);
                // Drawn just in front of the body it became part of, so their shared
                // faces don't flicker between the two.
                obj->Attributes()->ShadingAspect()->Aspect()->SetPolygonOffsets(Aspect_POM_Fill, -1.0f,
                                                                                -2.0f);
            } else {
                obj->Attributes()->SetFaceBoundaryDraw(d.style == DisplayStyle::ShadedWithEdges);
                obj->Attributes()->SetFaceBoundaryAspect(new Prs3d_LineAspect(kEdges, Aspect_TOL_SOLID, 1.0));
                // Wireframe lines in the edge color, so they show on the grid.
                obj->Attributes()->SetWireAspect(new Prs3d_LineAspect(kEdges, Aspect_TOL_SOLID, 1.0));
                obj->Attributes()->SetFreeBoundaryAspect(new Prs3d_LineAspect(kEdges, Aspect_TOL_SOLID, 1.0));
                obj->Attributes()->SetUnFreeBoundaryAspect(
                    new Prs3d_LineAspect(kEdges, Aspect_TOL_SOLID, 1.0));
            }
            const bool see = body.ghost || body.context;  // see-through: always shaded
            const bool wire = !see && d.style == DisplayStyle::Wireframe;
            d.context->Display(obj, wire ? AIS_WireFrame : AIS_Shaded, 0, false);
            next[key] = {body.shape.id(),    body.ghost, body.context, body.color,       obj,
                         body.placement,     *shape,     drawn,        std::move(edges), std::move(faces),
                         std::move(vertices)};
        }
        for (auto& [id, s] : d.shown) d.context->Remove(s.object, false);
        d.shown = std::move(next);
        d.applyPreview();  // a new drawing of the previewed body is moved too

        // Clicking picks whole bodies, or edges or faces of the bodies (not of ghosts),
        // or, to measure, any face, edge or point.
        const bool picking = content.pickEdges || content.pickFaces || content.pickMeasure;
        std::vector<int> partModes;
        if (content.pickMeasure) {
            partModes = {AIS_Shape::SelectionMode(TopAbs_VERTEX), AIS_Shape::SelectionMode(TopAbs_EDGE),
                         AIS_Shape::SelectionMode(TopAbs_FACE)};
        } else {
            partModes = {AIS_Shape::SelectionMode(content.pickFaces ? TopAbs_FACE : TopAbs_EDGE)};
        }
        for (const auto& [id, s] : d.shown) {
            d.context->Deactivate(s.object);
            if (s.context) continue;  // the rest of an assembly: not picked
            if (!picking) {
                d.context->Activate(s.object, 0);
            } else if (!s.ghost) {
                for (const int mode : partModes) d.context->Activate(s.object, mode);
            }
        }

        d.context->ClearSelected(false);
        if (!picking) {
            auto in = [](const auto& list, const auto& id) {
                return std::find(list.begin(), list.end(), id) != list.end();
            };
            for (const auto& [key, s] : d.shown) {
                if (key.component.isNil() ? in(content.selected, key.feature)
                                          : in(content.selectedComponents, key.component))
                    d.context->AddOrRemoveSelected(s.object, false);
            }
        }

        // Picked edges, drawn thick in the picked color; picked faces, filled with it;
        // picked points, big dots of it.
        for (auto* old : {&d.pickedEdges, &d.pickedFaces, &d.pickedPoints}) {
            if (old->IsNull()) continue;
            d.context->Remove(*old, false);
            old->Nullify();
        }
        TopoDS_Compound picked, pickedFaces, pickedPoints;
        BRep_Builder builder;
        builder.MakeCompound(picked);
        builder.MakeCompound(pickedFaces);
        builder.MakeCompound(pickedPoints);
        bool anyPicked = false, anyFace = false, anyPoint = false;
        for (const auto& p : content.picked) {
            auto it = d.shown.find(Impl::BodyKey{p.body, p.component});
            if (it == d.shown.end() || p.index < 0) continue;
            const bool face = p.kind == kernel::TopoKind::Face;
            const bool point = p.kind == kernel::TopoKind::Vertex;
            const auto& map = face ? it->second.faces : point ? it->second.vertices : it->second.edges;
            if (p.index >= map.Extent()) continue;
            builder.Add(face ? pickedFaces : point ? pickedPoints : picked, map(p.index + 1));
            (face ? anyFace : point ? anyPoint : anyPicked) = true;
        }
        if (anyPoint) {
            d.pickedPoints = new AIS_Shape(pickedPoints);
            d.pickedPoints->Attributes()->SetPointAspect(
                new Prs3d_PointAspect(Aspect_TOM_POINT, kSelected, 9.0));
            d.pickedPoints->SetZLayer(Graphic3d_ZLayerId_Topmost);  // over the body, so it always shows
            d.context->Display(d.pickedPoints, AIS_WireFrame, -1, false);  // -1: never picked itself
        }
        if (anyFace) {
            d.pickedFaces = new AIS_Shape(pickedFaces);
            d.pickedFaces->SetColor(kSelected);
            // Drawn after the bodies with their depth, so the picked face shows over its body.
            d.pickedFaces->SetZLayer(Graphic3d_ZLayerId_Top);
            d.context->Display(d.pickedFaces, AIS_Shaded, -1, false);  // -1: never picked itself
        }
        if (anyPicked) {
            d.pickedEdges = new AIS_Shape(picked);
            d.pickedEdges->SetColor(kSelected);
            d.pickedEdges->SetWidth(5.0);
            // Drawn after the bodies (with their depth), so the bodies' own edge
            // lines don't cover them; hidden edges stay hidden.
            d.pickedEdges->SetZLayer(Graphic3d_ZLayerId_Top);
            d.context->Display(d.pickedEdges, AIS_WireFrame, -1, false);  // -1: never picked itself
        }
        if (!d.everShown && !d.shown.empty()) {
            d.everShown = true;
            d.fitPending = true;
        }
    } catch (const Standard_Failure& e) {
        d.fail(*this, drawFailed(d.where(QStringLiteral("showing bodies: ") + failureText(e))));
    }
    update();
}

void ViewWidget::setOverlays(const std::vector<ViewOverlay>& overlays) {
    auto& d = *impl_;
    if (!d.context) return;
    try {
        for (auto& o : d.overlays) d.context->Remove(o, false);
        d.overlays.clear();
        int order = 0;
        const double ratio = devicePixelRatioF();
        // Dimensions are drawn at today's zoom (their arrowheads are sized in
        // pixels), and again when it changes (see paintGL()).
        d.givenOverlays = overlays;
        d.dimensionScale = d.currentDimensionScale(ratio);
        d.dimensionScreen = d.screenAxes();
        for (const auto& given : expandDimensions(overlays, d.dimensionScale, d.dimensionScreen)) {
            if (given.shape == OverlayShape::Dimension) continue;
            if (given.onViewCube && given.shape == OverlayShape::Lattice) continue;
            ViewOverlay o = given;
            if (o.onViewCube) {
                // In screen pixels, like the cube: scaled the same way on high-DPI screens.
                for (auto& p : o.points) p = {p.x * ratio, p.y * ratio, p.z * ratio};
                o.size *= ratio;
            }
            Handle(OverlayObject) obj = new OverlayObject(o);
            const int priority = std::min(10, order++);
            // A lattice (the grid) stays in the normal layer. Lines, points and
            // words go in the layer drawn after it: still hidden behind bodies,
            // but where they lie exactly on the grid (a sketch on the ground),
            // they win instead of flickering with it. Among them, later ones win.
            obj->SetZLayer(o.shape == OverlayShape::Lattice ? Graphic3d_ZLayerId_Default
                                                            : Graphic3d_ZLayerId_Top);
            if (o.overBodies) obj->SetZLayer(Graphic3d_ZLayerId_Topmost);  // drawn last, over everything
            if (o.onViewCube) {
                // Turned and placed like the cube, and in its layer (drawn over the
                // design), so the cube hides the parts behind it.
                obj->SetTransformPersistence(atTheCube(ratio));
                obj->SetZLayer(Graphic3d_ZLayerId_Topmost);
            }
            d.context->Display(obj, 0, -1, false);  // -1: never picked
            // OCCT draws higher priorities later (0 to 10): keep the overlays' order.
#if OCC_VERSION_HEX >= 0x070700
            d.context->SetDisplayPriority(obj, static_cast<Graphic3d_DisplayPriority>(priority));
#else
            d.context->SetDisplayPriority(obj, priority);
#endif
            d.overlays.push_back(obj);
        }
    } catch (const Standard_Failure& e) {
        d.fail(*this, drawFailed(d.where(QStringLiteral("showing overlays: ") + failureText(e))));
    }
    update();
}

void ViewWidget::setHandles(const std::vector<ViewHandle>& handles) {
    auto& d = *impl_;
    d.handles = handles;
    if (d.hovered && *d.hovered >= handles.size()) d.hovered.reset();
    try {
        d.showHandles(devicePixelRatioF());
    } catch (const Standard_Failure& e) {
        d.fail(*this, drawFailed(d.where(QStringLiteral("showing handles: ") + failureText(e))));
    }
    update();
}

const std::vector<ViewHandle>& ViewWidget::handles() const {
    return impl_->handles;
}

namespace {

// Turned about the line, then slid: as kernel::IKernel::transform() moves a body.
gp_Trsf motionTrsf(const kernel::Motion& motion) {
    gp_Trsf turn;
    const auto& a = motion.axis;
    if (motion.angle != 0 && a.x * a.x + a.y * a.y + a.z * a.z > 1e-18) {
        turn.SetRotation(
            gp_Ax1(gp_Pnt(motion.center.x, motion.center.y, motion.center.z), gp_Dir(a.x, a.y, a.z)),
            motion.angle);
    }
    gp_Trsf slide;
    slide.SetTranslation(gp_Vec(motion.offset.x, motion.offset.y, motion.offset.z));
    return slide * turn;
}

}  // namespace

void ViewWidget::setPreview(const FeatureId& body, const kernel::Motion& motion) {
    auto& d = *impl_;
    d.previewBody = body;
    d.previewComponents.clear();
    d.previewMove = motionTrsf(motion);
    if (!d.context) return;
    try {
        d.applyPreview();
    } catch (const Standard_Failure& e) {
        d.fail(*this, drawFailed(d.where(QStringLiteral("showing a moved body: ") + failureText(e))));
    }
    update();
}

void ViewWidget::setComponentPreview(const InstanceId& component, const kernel::Motion& motion) {
    setComponentPreview(std::vector<InstanceId>{component}, motion);
}

void ViewWidget::setComponentPreview(const std::vector<InstanceId>& components,
                                     const kernel::Motion& motion) {
    auto& d = *impl_;
    d.previewComponents = components;
    d.previewBody.reset();
    d.previewMove = motionTrsf(motion);
    if (!d.context) return;
    try {
        d.applyPreview();
    } catch (const Standard_Failure& e) {
        d.fail(*this, drawFailed(d.where(QStringLiteral("showing a moved body: ") + failureText(e))));
    }
    update();
}

void ViewWidget::clearPreview() {
    auto& d = *impl_;
    if (!d.previewBody && d.previewComponents.empty()) return;
    d.previewBody.reset();
    d.previewComponents.clear();
    if (!d.context) return;
    try {
        d.applyPreview();
    } catch (const Standard_Failure& e) {
        d.fail(*this, drawFailed(d.where(QStringLiteral("showing a moved body: ") + failureText(e))));
    }
    update();
}

void ViewWidget::setChoices(const std::vector<ViewChoice>& choices) {
    auto& d = *impl_;
    d.choices = choices;
    d.hoveredChoice.reset();
    d.pressedChoice.reset();
    setMouseTracking(true);
    if (!choices.empty()) setFocus(Qt::OtherFocusReason);  // so Escape reaches it
    try {
        d.showChoices(devicePixelRatioF());
    } catch (const Standard_Failure& e) {
        d.fail(*this, drawFailed(d.where(QStringLiteral("showing planes to pick: ") + failureText(e))));
    }
    update();
}

const std::vector<ViewChoice>& ViewWidget::choices() const {
    return impl_->choices;
}

std::optional<QPoint> ViewWidget::toScreen(const kernel::Vec3& point) const {
    const auto& d = *impl_;
    if (!d.view || !d.ready) return std::nullopt;
    const auto p = d.projection()(point);
    if (!p) return std::nullopt;
    const double ratio = devicePixelRatioF();
    return QPoint(static_cast<int>(std::lround(p->x / ratio)), static_cast<int>(std::lround(p->y / ratio)));
}

void ViewWidget::setLeftDrag(LeftDrag drag) {
    impl_->controller.setLeftDrag(drag);
}

LeftDrag ViewWidget::leftDrag() const {
    return impl_->controller.leftDrag();
}

void ViewWidget::setDisplayStyle(DisplayStyle style) {
    auto& d = *impl_;
    if (d.style == style) return;
    d.style = style;
    if (!d.context) return;  // used when the bodies are first shown
    try {
        for (auto& [id, s] : d.shown) d.applyStyle(s.object, s.ghost || s.context);
    } catch (const Standard_Failure& e) {
        d.fail(*this,
               drawFailed(d.where(QStringLiteral("changing how bodies are drawn: ") + failureText(e))));
    }
    update();
}

DisplayStyle ViewWidget::displayStyle() const {
    return impl_->style;
}

int ViewWidget::overlayCount() const {
    return static_cast<int>(impl_->overlays.size());
}

void ViewWidget::setStandardView(StandardView v) {
    auto& d = *impl_;
    if (!d.view) return;
    const ViewDirection dir = direction(v);
    d.controller.AbortViewAnimation();
    d.view->SetProj(dir.toEye.x, dir.toEye.y, dir.toEye.z);
    d.view->SetUp(dir.up.x, dir.up.y, dir.up.z);
    d.fitPending = true;
    update();
}

void ViewWidget::setDrawingPlane(std::optional<DrawingPlane> plane) {
    impl_->drawing = plane;
    impl_->leftDown = impl_->dragging = false;
    impl_->draggingHandle.reset();
    try {
        impl_->showHandles(devicePixelRatioF());
    } catch (const Standard_Failure&) {
    }
    setFocusPolicy(plane ? Qt::StrongFocus : Qt::NoFocus);
    setCursor(plane ? Qt::CrossCursor : Qt::ArrowCursor);
}

void ViewWidget::lookAlong(const ViewDirection& dir) {
    auto& d = *impl_;
    if (!d.view) return;
    d.controller.AbortViewAnimation();
    d.view->SetProj(dir.toEye.x, dir.toEye.y, dir.toEye.z);
    d.view->SetUp(dir.up.x, dir.up.y, dir.up.z);
    d.fitPending = true;
    update();
}

void ViewWidget::keyPressEvent(QKeyEvent* event) {
    if (impl_->draggingHandle && event->key() == Qt::Key_Escape) {
        impl_->draggingHandle.reset();
        impl_->showHandles(devicePixelRatioF());
        update();
        emit handleCancelled();
        event->accept();
        return;
    }
    if (impl_->drawing && event->key() == Qt::Key_Escape) {
        emit planeCancelled();
        event->accept();
        return;
    }
    if (!impl_->choices.empty() && event->key() == Qt::Key_Escape) {
        emit choicesCancelled();
        event->accept();
        return;
    }
    QOpenGLWidget::keyPressEvent(event);
}

void ViewWidget::fitAll() {
    impl_->fitPending = true;
    update();
}

ViewDirection ViewWidget::camera() const {
    const auto& d = *impl_;
    if (!d.view) return direction(StandardView::Iso);
    const Handle(Graphic3d_Camera)& cam = d.view->Camera();
    const gp_Dir look = cam->Direction();
    const gp_Dir up = cam->Up();
    return {{-look.X(), -look.Y(), -look.Z()}, {up.X(), up.Y(), up.Z()}};
}

void ViewWidget::setInputs(const std::vector<ViewInput>& inputs, const std::string& focus) {
    impl_->inputs->set(inputs, focus);
}

const std::vector<ViewInput>& ViewWidget::inputs() const {
    return impl_->inputs->inputs();
}

QLineEdit* ViewWidget::input(const std::string& id) const {
    return impl_->inputs->box(id);
}

CameraPlace ViewWidget::cameraPlace() const {
    const auto& d = *impl_;
    if (d.placeLater) return *d.placeLater;
    if (!d.view) return {};
    const Handle(Graphic3d_Camera)& cam = d.view->Camera();
    const gp_Pnt eye = cam->Eye();
    const gp_Pnt at = cam->Center();
    const gp_Dir up = cam->Up();
    return {{eye.X(), eye.Y(), eye.Z()}, {at.X(), at.Y(), at.Z()}, {up.X(), up.Y(), up.Z()}, cam->Scale()};
}

void ViewWidget::setCameraPlace(const CameraPlace& place) {
    auto& d = *impl_;
    if (place.scale <= 0) return;
    d.everShown = true;  // the camera is where it should be: no fitting to the first body
    d.fitPending = false;
    if (!d.view) {
        d.placeLater = place;
        return;
    }
    d.controller.AbortViewAnimation();
    try {
        d.place(place);
    } catch (const Standard_Failure&) {
        d.fitPending = true;  // a place OCCT can't use (the eye on the point looked at): fit instead
    }
    update();
}

void ViewWidget::fitWhenShown() {
    auto& d = *impl_;
    d.placeLater.reset();
    d.everShown = false;  // setContent() fits once there's a body to show
    d.fitPending = false;
    update();
}

const SceneContent& ViewWidget::content() const {
    return impl_->content;
}

int ViewWidget::shownCount() const {
    return static_cast<int>(impl_->shown.size());
}

bool ViewWidget::ready() const {
    return impl_->ready && impl_->problem.isEmpty();
}

QString ViewWidget::problem() const {
    return impl_->problem;
}

void ViewWidget::clickAt(QPoint point, bool add) {
    auto& d = *impl_;
    if (!d.view) return;
    const double ratio = devicePixelRatioF();
    const Graphic3d_Vec2i p(static_cast<int>(point.x() * ratio), static_cast<int>(point.y() * ratio));
    const Aspect_VKeyFlags flags = add ? Aspect_VKeyFlags_CTRL : Aspect_VKeyFlags_NONE;
    d.clickAdds = add;
    d.controller.UpdateMousePosition(p, Aspect_VKeyMouse_NONE, flags, false);
    d.controller.UpdateMouseButtons(p, Aspect_VKeyMouse_LeftButton, flags, false);
    d.controller.UpdateMouseButtons(p, Aspect_VKeyMouse_NONE, flags, false);
    update();
}

QSize ViewWidget::sizeHint() const {
    return {640, 480};
}

void ViewWidget::initializeGL() {
    auto& d = *impl_;
    if (!d.view) return;
    if (auto* gl = QOpenGLContext::currentContext()) {
        auto* f = gl->functions();
        const auto text = [f](GLenum what) {
            const auto* s = reinterpret_cast<const char*>(f->glGetString(what));
            return s ? QString::fromLatin1(s) : QStringLiteral("?");
        };
        const auto fmt = gl->format();
        d.glInfo = QStringLiteral("%1, OpenGL %2, %3 profile")
                       .arg(text(GL_RENDERER), text(GL_VERSION),
                            fmt.profile() == QSurfaceFormat::CoreProfile ? QStringLiteral("core")
                                                                         : QStringLiteral("compatibility"));
    }
    try {
        // OCCT wants a native window. On Windows, the one Qt made the context
        // current on (0 asks for it), or the top-level window if that's an
        // offscreen buffer with none; elsewhere Qt draws into a buffer, so the
        // top-level window (asking our own widget would make it native, which
        // QOpenGLWidget dislikes).
        const auto topLevel = toDrawable(window()->winId());
#if defined(_WIN32)
        const Aspect_Drawable native = nullptr;
#else
        const Aspect_Drawable native = topLevel;
#endif
        const double ratio = devicePixelRatioF();
        const bool first = d.view->Window().IsNull();
        // Called again when the panel moves to another window: OCCT takes the new context.
        std::string used;
        const auto why = detail::attachContext(
            d.view, context()->format().profile() == QSurfaceFormat::CoreProfile, native, topLevel,
            static_cast<int>(width() * ratio), static_cast<int>(height() * ratio), used);
        d.glInfo += QStringLiteral(", window: ") + QString::fromStdString(used);
        if (!why.empty()) {
            d.fail(*this, drawFailed(d.where(QStringLiteral("OCCT couldn't use Qt's OpenGL context: ") +
                                             QString::fromStdString(why))));
            return;
        }
        if (first) {
            d.cube = new AIS_ViewCube();
            d.cube->SetSize(kCubeSize * ratio);
            d.cube->SetFontHeight(9 * ratio);
            d.cube->SetDrawAxes(false);  // the sb.axes plugin draws them, as overlays on the cube
            d.cube->SetViewAnimation(d.controller.ViewAnimation());
            d.cube->SetFixedAnimationLoop(false);
            d.cube->SetAutoStartAnimation(true);
            d.cube->SetTransformPersistence(atTheCube(ratio));
            d.cube->SetZLayer(Graphic3d_ZLayerId_Topmost);  // OCCT's default, said here for its overlays
            d.context->Display(d.cube, 0, 0, false);
        }
    } catch (const Standard_Failure& e) {
        d.fail(*this, drawFailed(d.where(QStringLiteral("starting OpenGL: ") + failureText(e))));
    }
}

void ViewWidget::paintGL() {
    auto& d = *impl_;
    if (!d.view || d.view->Window().IsNull() || !d.problem.isEmpty()) return;
    try {
        if (const auto why = detail::useQtFramebuffer(d.view); !why.empty()) {
            d.fail(*this,
                   drawFailed(d.where(QStringLiteral("OCCT couldn't draw into Qt's framebuffer: %1; Qt's "
                                                     "framebuffer is %2")
                                          .arg(QString::fromStdString(why))
                                          .arg(defaultFramebufferObject()))));
            return;
        }
        if (d.placeLater) {
            d.place(*d.placeLater);
            d.placeLater.reset();
        }
        if (d.fitPending) d.fit();
        // Dimensions keep their arrowheads the same size on the screen as the view
        // zooms, and their numbers' room as it turns.
        const double dimensionScale = d.currentDimensionScale(devicePixelRatioF());
        if (dimensionScale > 0) {
            const auto screen = d.screenAxes();
            auto turned = [](const kernel::Vec3& a, const kernel::Vec3& b) {
                return a.x * b.x + a.y * b.y + a.z * b.z < 0.9995;
            };
            if (std::abs(dimensionScale - d.dimensionScale) > 0.02 * dimensionScale ||
                turned(screen.right, d.dimensionScreen.right) || turned(screen.up, d.dimensionScreen.up))
                setOverlays(d.givenOverlays);
        }
        // Handles sized in pixels stay that size when the view zooms.
        if (!d.handles.empty() && !d.drawing) {
            const double scale = d.currentHandleScale();
            if (d.handleScale <= 0 || std::abs(scale - d.handleScale) > 0.01 * d.handleScale)
                d.showHandles(devicePixelRatioF());
        }
        // Choices sized in pixels stay that size too.
        if (!d.choices.empty() && !d.drawing) {
            const double scale = d.currentChoiceScale();
            if (d.choiceScale <= 0 || std::abs(scale - d.choiceScale) > 0.01 * d.choiceScale)
                d.showChoices(devicePixelRatioF());
        }
        d.view->Invalidate();
        d.view->InvalidateImmediate();
        d.controller.FlushViewEvents(d.context, d.view, true);
        d.ready = true;
        d.inputs->place();  // the boxes follow their points as the view turns, pans and zooms
    } catch (const Standard_Failure& e) {
        d.fail(*this, drawFailed(d.where(QStringLiteral("drawing: ") + failureText(e))));
    }
}

void ViewWidget::mousePressEvent(QMouseEvent* event) {
    QOpenGLWidget::mousePressEvent(event);
    emit activated();
    auto& d = *impl_;
    if (d.drawing && event->button() == Qt::LeftButton) {
        // Drawing: the left button draws; it doesn't orbit or select.
        setFocus(Qt::MouseFocusReason);
        d.leftPress = event->position().toPoint();
        d.leftDown = true;
        d.dragging = false;
        d.pressPoint = d.planePoint(event->position(), devicePixelRatioF());
        if (d.pressPoint) d.pressPoint->add = event->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier);
        return;
    }
    if (event->button() == Qt::RightButton) d.rightPress = event->position().toPoint();
    if (!d.view) return;
    if (event->button() == Qt::LeftButton && !d.drawing && !d.handles.empty() && d.ready) {
        // A handle under the mouse: drag it, instead of orbiting or selecting.
        const double r = devicePixelRatioF();
        const ScreenPoint mouse{event->position().x() * r, event->position().y() * r};
        if (const auto i = handleAt(d.shownHandles, d.projection(), mouse, 8 * r)) {
            setFocus(Qt::MouseFocusReason);  // so Escape reaches it
            d.draggingHandle = i;
            d.dragHandle = d.shownHandles[*i];
            d.dragStart = d.rayAt(mouse.x, mouse.y);
            d.dragBy = {};
            d.showHandles(r);
            update();
            return;
        }
    }
    if (event->button() == Qt::LeftButton && !d.drawing) {
        // A choice under the mouse: wait for the button to come up. A drag still orbits.
        d.pressedChoice = d.choiceUnder(event->position(), devicePixelRatioF());
        d.choicePress = event->position().toPoint();
        if (d.pressedChoice) setFocus(Qt::MouseFocusReason);
    }
    d.clickAdds = event->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier);
    const double ratio = devicePixelRatioF();
    const Graphic3d_Vec2i p(static_cast<int>(event->position().x() * ratio),
                            static_cast<int>(event->position().y() * ratio));
    if (d.controller.UpdateMouseButtons(p, toButtons(event->buttons()), toFlags(event->modifiers()), false))
        update();
}

void ViewWidget::mouseReleaseEvent(QMouseEvent* event) {
    QOpenGLWidget::mouseReleaseEvent(event);
    auto& d = *impl_;
    if (d.drawing && event->button() == Qt::LeftButton && d.leftDown) {
        d.leftDown = false;
        auto p = d.planePoint(event->position(), devicePixelRatioF());
        if (p) p->add = event->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier);
        if (d.dragging) {
            d.dragging = false;
            if (p && d.pressPoint) emit planeDropped(*d.pressPoint, *p);
        } else if (p) {
            emit planeClicked(*p);
        }
        return;
    }
    if (d.draggingHandle && event->button() == Qt::LeftButton) {
        const std::string id = d.dragHandle.id;
        d.draggingHandle.reset();
        d.showHandles(devicePixelRatioF());
        update();
        emit handleDropped(id, d.dragBy);
        return;
    }
    if (d.pressedChoice && event->button() == Qt::LeftButton) {
        // A click (not a drag) on the choice it went down on chooses it, instead of picking.
        const auto pressed = *d.pressedChoice;
        d.pressedChoice.reset();
        if ((event->position().toPoint() - d.choicePress).manhattanLength() < 6 &&
            d.choiceUnder(event->position(), devicePixelRatioF()) == pressed &&
            pressed < d.shownChoices.size()) {
            const std::string id = d.shownChoices[pressed].id;
            // Let go of the button without picking what's behind the square.
            d.controller.ResetViewInput();
            emit choiceClicked(id);
            update();
            return;
        }
    }
    // A right click that didn't move (much) asks for the menu; a right drag pans.
    if (event->button() == Qt::RightButton &&
        (event->position().toPoint() - d.rightPress).manhattanLength() < 4) {
        emit menuRequested(event->globalPosition().toPoint());
    }
    if (!d.view) return;
    const double ratio = devicePixelRatioF();
    const Graphic3d_Vec2i p(static_cast<int>(event->position().x() * ratio),
                            static_cast<int>(event->position().y() * ratio));
    if (d.controller.UpdateMouseButtons(p, toButtons(event->buttons()), toFlags(event->modifiers()), false))
        update();
}

void ViewWidget::mouseMoveEvent(QMouseEvent* event) {
    QOpenGLWidget::mouseMoveEvent(event);
    auto& d = *impl_;
    if (!d.view) return;
    if (d.draggingHandle) {
        const double r = devicePixelRatioF();
        const auto& h = d.dragHandle;
        if (const auto by =
                handleDrag(h, d.dragStart, d.rayAt(event->position().x() * r, event->position().y() * r))) {
            auto turned = *by;
            if (h.kind == HandleKind::Ring) {
                // Round and round: past half a turn it carries on, rather than jumping back.
                constexpr double kTurn = 2 * std::numbers::pi;
                turned.x += kTurn * std::round((d.dragBy.x - turned.x) / kTurn);
            }
            d.dragBy = turned;
            emit handleDragged(h.id, turned);
        }
        return;
    }
    if (!d.drawing && !d.handles.empty() && d.ready && !(event->buttons() & Qt::LeftButton)) {
        // Light up the handle under the mouse.
        const double r = devicePixelRatioF();
        const auto now = handleAt(d.shownHandles, d.projection(),
                                  {event->position().x() * r, event->position().y() * r}, 8 * r);
        if (now != d.hovered) {
            d.hovered = now;
            d.showHandles(r);
            setCursor(now ? Qt::SizeAllCursor : Qt::ArrowCursor);
            update();
        }
    }
    if (!d.drawing && !d.choices.empty() && !(event->buttons() & Qt::LeftButton)) {
        // Light up the choice under the mouse.
        const auto now = d.choiceUnder(event->position(), devicePixelRatioF());
        if (now != d.hoveredChoice) {
            d.hoveredChoice = now;
            d.showChoices(devicePixelRatioF());
            setCursor(now ? Qt::PointingHandCursor : Qt::ArrowCursor);
            update();
        }
    }
    if (d.drawing) {
        auto p = d.planePoint(event->position(), devicePixelRatioF());
        if (p) p->add = event->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier);
        if (d.leftDown && (event->buttons() & Qt::LeftButton)) {
            // The left button drags on the plane: no orbit.
            if ((event->position().toPoint() - d.leftPress).manhattanLength() >= 6) d.dragging = true;
            if (d.dragging && p && d.pressPoint) emit planeDragged(*d.pressPoint, *p);
            return;
        }
        if (p) emit planeMoved(*p);
        if (event->buttons() & Qt::LeftButton) return;
    }
    const double ratio = devicePixelRatioF();
    const Graphic3d_Vec2i p(static_cast<int>(event->position().x() * ratio),
                            static_cast<int>(event->position().y() * ratio));
    if (d.controller.UpdateMousePosition(p, toButtons(event->buttons()), toFlags(event->modifiers()), false))
        update();
}

void ViewWidget::wheelEvent(QWheelEvent* event) {
    QOpenGLWidget::wheelEvent(event);
    auto& d = *impl_;
    if (!d.view) return;
    const double ratio = devicePixelRatioF();
    const Graphic3d_Vec2i p(static_cast<int>(event->position().x() * ratio),
                            static_cast<int>(event->position().y() * ratio));
    if (d.controller.UpdateMouseScroll(Aspect_ScrollDelta(p, event->angleDelta().y() / 8.0))) update();
}

void ViewWidget::leaveEvent(QEvent* event) {
    QOpenGLWidget::leaveEvent(event);
    if (impl_->context) {
        try {
            impl_->context->ClearDetected(false);
        } catch (const Standard_Failure&) {
        }
        update();
    }
}

}  // namespace sb::view
