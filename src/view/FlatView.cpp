#include "sb/view/FlatView.h"

#include "InputBoxes.h"
#include "sb/view/Dimensions.h"

#include <QFontMetricsF>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPolygonF>

#include <algorithm>
#include <cmath>

namespace sb::view {

namespace {
constexpr double kPixelsPerMm = 4.0;
constexpr double kMinPixels = 8.0;  // closer than this, a lattice turns into a grey smear

QColor qcolor(const ViewOverlay& o) {
    return QColor(o.color.r, o.color.g, o.color.b, std::clamp(o.alpha, 0, 255));
}
}  // namespace

FlatView::FlatView(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("sb.view.flat"));
    setAutoFillBackground(true);
    QPalette p = palette();
    p.setColor(QPalette::Window, QColor(246, 247, 249));
    setPalette(p);
    inputs_ =
        new InputBoxes(this, [this](const kernel::Vec3& at) { return std::optional<QPointF>(toScreen(at)); });
    inputs_->typed = [this](const std::string& id, const QString& text) { emit inputTyped(id, text); };
    inputs_->entered = [this](const std::string& id) {
        const std::string copy = id;  // the boxes may change while it's handled
        emit inputEntered(copy);
    };
    inputs_->cancelled = [this] { emit inputsCancelled(); };
}

void FlatView::setInputs(const std::vector<ViewInput>& inputs, const std::string& focus) {
    inputs_->set(inputs, focus);
}

const std::vector<ViewInput>& FlatView::inputs() const {
    return inputs_->inputs();
}

QLineEdit* FlatView::input(const std::string& id) const {
    return inputs_->box(id);
}

void FlatView::setText(const QString& text) {
    text_ = text;
    update();
}

void FlatView::setOverlays(const std::vector<ViewOverlay>& overlays) {
    overlays_ = overlays;
    update();
}

QPointF FlatView::toScreen(const kernel::Vec3& p) const {
    return {width() / 2.0 + p.x * kPixelsPerMm, height() / 2.0 - p.y * kPixelsPerMm};
}

void FlatView::setDrawingPlane(std::optional<DrawingPlane> plane) {
    drawing_ = plane;
    leftDown_ = dragging_ = false;
    updateTracking();
    setCursor(plane ? Qt::CrossCursor : Qt::ArrowCursor);
}

void FlatView::updateTracking() {
    const bool on = drawing_.has_value() || !choices_.empty();
    setMouseTracking(on);
    setFocusPolicy(on ? Qt::StrongFocus : Qt::NoFocus);
}

void FlatView::setChoices(const std::vector<ViewChoice>& choices) {
    choices_ = choices;
    hoveredChoice_.reset();
    pressedChoice_.reset();
    updateTracking();
    if (!choices_.empty()) setFocus(Qt::OtherFocusReason);  // so Escape reaches it
    update();
}

std::vector<ViewChoice> FlatView::shownChoices() const {
    auto shown = choices_;
    for (auto& c : shown) {
        if (!c.pixels) continue;
        c.from /= kPixelsPerMm;
        c.to /= kPixelsPerMm;
        c.pixels = false;
    }
    return shown;
}

std::optional<std::size_t> FlatView::choiceUnder(QPointF at) const {
    if (choices_.empty() || drawing_) return std::nullopt;
    const Projection project = [this](const kernel::Vec3& p) -> std::optional<ScreenPoint> {
        const QPointF s = toScreen(p);
        return ScreenPoint{s.x(), s.y()};
    };
    return choiceAt(shownChoices(), project, {at.x(), at.y()}, 6);
}

std::optional<PlanePoint> FlatView::planePoint(QPointF at) const {
    if (!drawing_) return std::nullopt;
    // Seen from above: the line of sight goes straight down through the point.
    const kernel::Vec3 above{(at.x() - width() / 2.0) / kPixelsPerMm,
                             (height() / 2.0 - at.y()) / kPixelsPerMm, 0};
    const auto hit = rayHitsPlane(above, {0, 0, -1}, *drawing_);
    if (!hit) return std::nullopt;
    return PlanePoint{*hit, 1.0 / kPixelsPerMm};
}

namespace {

constexpr int kDragPixels = 6;  // the mouse must move this far with the button down to drag

bool adding(const QMouseEvent* event) {
    return event->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier);
}

}  // namespace

void FlatView::mousePressEvent(QMouseEvent* event) {
    QWidget::mousePressEvent(event);
    if (!drawing_ && event->button() == Qt::LeftButton) {
        pressedChoice_ = choiceUnder(event->position());
        if (pressedChoice_) setFocus(Qt::MouseFocusReason);
        leftPress_ = event->position().toPoint();
    }
    if (!drawing_ || event->button() != Qt::LeftButton) return;
    setFocus(Qt::MouseFocusReason);
    leftPress_ = event->position().toPoint();
    leftDown_ = true;
    dragging_ = false;
    pressPoint_ = planePoint(event->position());
    if (pressPoint_) pressPoint_->add = adding(event);
}

void FlatView::mouseMoveEvent(QMouseEvent* event) {
    QWidget::mouseMoveEvent(event);
    if (!drawing_ && !choices_.empty()) {
        const auto now = choiceUnder(event->position());
        if (now != hoveredChoice_) {
            hoveredChoice_ = now;
            setCursor(now ? Qt::PointingHandCursor : Qt::ArrowCursor);
            update();
        }
    }
    auto p = planePoint(event->position());
    if (!p) return;
    p->add = adding(event);
    if (leftDown_ && (event->buttons() & Qt::LeftButton)) {
        if ((event->position().toPoint() - leftPress_).manhattanLength() >= kDragPixels) dragging_ = true;
        if (dragging_ && pressPoint_) emit planeDragged(*pressPoint_, *p);
        return;
    }
    emit planeMoved(*p);
}

void FlatView::mouseReleaseEvent(QMouseEvent* event) {
    QWidget::mouseReleaseEvent(event);
    if (pressedChoice_ && event->button() == Qt::LeftButton) {
        const auto pressed = *pressedChoice_;
        pressedChoice_.reset();
        if ((event->position().toPoint() - leftPress_).manhattanLength() < kDragPixels &&
            choiceUnder(event->position()) == pressed && pressed < choices_.size()) {
            // A copy: whoever hears it may take the choices away (and their ids with them).
            const std::string id = choices_[pressed].id;
            emit choiceClicked(id);
            return;
        }
    }
    if (!drawing_ || event->button() != Qt::LeftButton || !leftDown_) return;
    leftDown_ = false;
    auto p = planePoint(event->position());
    if (!p) return;
    p->add = adding(event);
    if (dragging_) {
        dragging_ = false;
        if (pressPoint_) emit planeDropped(*pressPoint_, *p);
    } else {
        emit planeClicked(*p);
    }
}

void FlatView::keyPressEvent(QKeyEvent* event) {
    if (drawing_ && event->key() == Qt::Key_Escape) {
        emit planeCancelled();
        event->accept();
        return;
    }
    if (!choices_.empty() && event->key() == Qt::Key_Escape) {
        emit choicesCancelled();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

double FlatView::pixelsPerSpace(const ViewOverlay& lattice) {
    double step = lattice.spacing * kPixelsPerMm;
    // Too dense to see: draw every 2nd, 5th, 10th, ... line instead.
    for (double factor : {2.0, 5.0, 10.0, 20.0, 50.0, 100.0, 200.0, 500.0, 1000.0}) {
        if (step >= kMinPixels) break;
        step = lattice.spacing * factor * kPixelsPerMm;
    }
    return step;
}

void FlatView::paintEvent(QPaintEvent*) {
    inputs_->place();  // the middle of the view moves as it resizes
    QPainter painter(this);

    // Lattices (like the grid) first, underneath everything else, so lines that
    // lie on them (like the axes) still show.
    // Dimensions become lines and boxed words, at this view's fixed scale.
    const auto overlays = expandDimensions(overlays_, 1.0 / kPixelsPerMm);
    std::vector<const ViewOverlay*> order;
    for (const auto& o : overlays) {
        if (o.shape == OverlayShape::Lattice) order.push_back(&o);
    }
    for (const auto& o : overlays) {
        if (o.shape != OverlayShape::Lattice && o.shape != OverlayShape::Dimension) order.push_back(&o);
    }
    for (const auto* drawn : order) {
        const auto& o = *drawn;
        // The origin is the middle of the view, so drawings stay put as the window
        // resizes. There's no view cube here, but what's drawn on it goes where it
        // would be, in the top-right corner, seen from above like the rest.
        auto toScreen = [this, &o](const kernel::Vec3& p) {
            if (!o.onViewCube) return this->toScreen(p);
            return QPointF(width() - kViewCubeOffset + p.x, kViewCubeOffset - p.y);
        };
        if (o.onViewCube && o.shape == OverlayShape::Lattice) continue;
        if (o.shape == OverlayShape::Lattice) {
            // Seen from above: only a lattice on a floor-like plane makes sense here,
            // and it covers the whole view.
            if (!(o.spacing > 0)) continue;
            const double step = pixelsPerSpace(o);
            const QPointF o0 = toScreen(o.origin);
            const double x0 = o0.x() - std::floor(o0.x() / step) * step;
            const double y0 = o0.y() - std::floor(o0.y() / step) * step;
            if (!o.dots) {
                painter.setPen(QPen(qcolor(o), std::max(1.0, o.size)));
                for (double x = x0; x < width(); x += step)
                    painter.drawLine(QPointF(x, 0), QPointF(x, height()));
                for (double y = y0; y < height(); y += step)
                    painter.drawLine(QPointF(0, y), QPointF(width(), y));
            } else {
                painter.setPen(Qt::NoPen);
                painter.setRenderHint(QPainter::Antialiasing);
                painter.setBrush(qcolor(o));
                const double r = std::max(1.0, o.size) / 2 + 0.1;
                for (double x = x0; x < width(); x += step) {
                    for (double y = y0; y < height(); y += step) painter.drawEllipse(QPointF(x, y), r, r);
                }
                painter.setRenderHint(QPainter::Antialiasing, false);
            }
        } else if (o.shape == OverlayShape::Labels) {
            const QFont before = painter.font();
            QFont font = before;
            font.setBold(true);
            font.setPixelSize(static_cast<int>(std::max(6.0, o.size)));
            painter.setFont(font);
            painter.setPen(qcolor(o));
            const QFontMetricsF metrics(font);
            for (std::size_t i = 0; i < std::min(o.points.size(), o.texts.size()); ++i) {
                const QString word = QString::fromStdString(o.texts[i]);
                const QRectF box = metrics.boundingRect(word);
                const QPointF at = toScreen(o.points[i]) -
                                   QPointF(box.width() / 2, -metrics.ascent() / 2 + metrics.descent() / 2);
                if (o.boxed) {
                    // On a box of the background's color, so lines under the words don't show.
                    painter.fillRect(QRectF(at.x() - 2, at.y() - metrics.ascent() - 1, box.width() + 4,
                                            metrics.height() + 2),
                                     palette().color(QPalette::Base));
                }
                painter.drawText(at, word);
            }
            painter.setFont(before);
        } else if (o.shape == OverlayShape::Triangles) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(qcolor(o));
            for (std::size_t i = 0; i + 2 < o.points.size(); i += 3) {
                const QPointF corners[3] = {toScreen(o.points[i]), toScreen(o.points[i + 1]),
                                            toScreen(o.points[i + 2])};
                painter.drawPolygon(corners, 3);
            }
        } else if (o.shape == OverlayShape::Lines) {
            painter.setPen(QPen(qcolor(o), std::max(1.0, o.size)));
            for (std::size_t i = 0; i + 1 < o.points.size(); i += 2)
                painter.drawLine(toScreen(o.points[i]), toScreen(o.points[i + 1]));
        } else {
            painter.setPen(Qt::NoPen);
            painter.setRenderHint(QPainter::Antialiasing);
            painter.setBrush(qcolor(o));
            const double r = std::max(1.0, o.size) / 2 + 0.1;
            for (const auto& p : o.points) painter.drawEllipse(toScreen(p), r, r);
            painter.setRenderHint(QPainter::Antialiasing, false);
        }
    }
    // Squares to choose, seen from above: one lying flat is a square, one standing
    // up is a line. The one under the mouse is more solid.
    if (!drawing_) {
        const auto shown = shownChoices();
        painter.setRenderHint(QPainter::Antialiasing);
        for (std::size_t i = 0; i < shown.size(); ++i) {
            const auto& c = shown[i];
            const bool lit = hoveredChoice_ == i;
            QPolygonF poly;
            for (const auto& p : choiceCorners(c)) poly << toScreen(p);
            const QColor color(c.color.r, c.color.g, c.color.b);
            QColor fill = color;
            fill.setAlpha(lit ? 160 : 70);
            painter.setPen(QPen(color, lit ? 3.0 : 1.5));
            painter.setBrush(fill);
            painter.drawPolygon(poly);
            if (!c.label.empty()) {
                painter.setPen(QColor(30, 34, 40));
                painter.drawText(toScreen(choiceLabelAt(c)), QString::fromStdString(c.label));
            }
        }
        painter.setRenderHint(QPainter::Antialiasing, false);
    }
    // The text, on a light card in the top-left corner, so the origin stays clear.
    if (!text_.isEmpty()) {
        const int margin = 16, pad = 12;
        const QRect room(margin + pad, margin + pad, std::max(120, width() / 2), height() / 2);
        const QRect used = painter.boundingRect(room, Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, text_);
        painter.setPen(QColor(214, 218, 225));
        painter.setBrush(QColor(255, 255, 255, 235));
        painter.drawRoundedRect(used.adjusted(-pad, -pad, pad, pad), 8, 8);
        // The card is always light, so its text is always dark: the window's own
        // text color is white in dark mode, which made the words invisible.
        painter.setPen(QColor(31, 35, 40));
        painter.drawText(used, Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, text_);
    }
}

}  // namespace sb::view
