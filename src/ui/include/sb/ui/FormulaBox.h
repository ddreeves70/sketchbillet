/// @file
/// A number box that also takes a formula, like `wall * 2`, for panels.
#pragma once

#include <functional>
#include <utility>

#include <QDoubleSpinBox>
#include <QFocusEvent>
#include <QFont>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QPalette>
#include <QPointer>
#include <QResizeEvent>
#include <QTimer>

namespace sb::ui {

/// A number box (a QDoubleSpinBox) that also takes a formula. Type a number and
/// it works like any number box. Type anything else, like `wall * 2` or `1 in`,
/// and it hands the text to your handler, to set with a command such as
/// `feature.set_value` with `formula`.
///
/// A box whose number comes from a formula (setFormula()) shows a small "fx"
/// mark, and shows the formula itself, ready to change, while it's being typed
/// in. Without a handler it's a plain number box.
///
/// @code
/// auto* box = new sb::ui::FormulaBox(panel);
/// box->setSuffix(" mm");
/// box->setFormulaHandler([&](const QString& text) {
///     ctx.documents().run(doc, "feature.set_value",
///                         {{"feature", step}, {"setting", std::string("distance")},
///                          {"formula", text.toStdString()}});
/// });
/// box->setValue(6.0);
/// box->setFormula("wall * 2");  // the number is worked out from this
/// @endcode
class FormulaBox : public QDoubleSpinBox {
public:
    /// What to do with a formula the student typed.
    using Handler = std::function<void(const QString& formula)>;

    /// Makes an empty box that takes numbers only, until it's given a handler.
    /// @param parent The widget it's in.
    explicit FormulaBox(QWidget* parent = nullptr) : QDoubleSpinBox(parent) {}

    /// Sets what happens when a formula is typed. It's called just after Enter
    /// is pressed or the box is left, with the text as typed, when the text
    /// isn't a plain number and isn't the formula the box already has.
    /// @param handler What to call, or nothing to take numbers only.
    void setFormulaHandler(Handler handler) { handler_ = std::move(handler); }

    /// Says whether typing a formula does anything.
    /// @return True if the box has a handler.
    bool takesFormulas() const { return static_cast<bool>(handler_); }

    /// Says which formula the box's number comes from, to show the "fx" mark and
    /// the formula while typing. Set the number itself with setValue().
    /// @param formula The formula as typed, or empty if the number was typed.
    void setFormula(const QString& formula) {
        if (formula == formula_) return;
        formula_ = formula;
        setProperty("formula", formula_);  // for style sheets and tests
        if (formula_.isEmpty()) {
            delete mark_;
            mark_ = nullptr;
            lineEdit()->setTextMargins(0, 0, 0, 0);
        } else {
            if (!mark_) {
                // A slim mark, so it fits the narrow x, y, z boxes too.
                mark_ = new QLabel(QStringLiteral("fx"), lineEdit());
                mark_->setObjectName(QStringLiteral("formulaMark"));
                QFont f = font();
                f.setItalic(true);
                f.setBold(true);
                f.setPointSizeF(f.pointSizeF() * 0.8);
                mark_->setFont(f);
                // In the theme's highlight color (a style sheet, so the app's own doesn't undo it).
                mark_->setStyleSheet(
                    QStringLiteral("color: %1;").arg(palette().color(QPalette::Highlight).name()));
                mark_->setAlignment(Qt::AlignCenter);
                mark_->setCursor(Qt::ArrowCursor);
                mark_->show();
                placeMark();
            }
            mark_->setToolTip(tr("Worked out from %1").arg(formula_));
        }
        if (hasFocus()) showFormula();
    }

    /// Gives the formula the box's number comes from.
    /// @return The formula, or empty if the number was typed.
    QString formula() const { return formula_; }

    /// Shows whole numbers without a trail of zeros: "40 mm", not "40.000 mm".
    /// @param on True to trim them. Off to start with.
    void setTrimZeros(bool on) {
        trimZeros_ = on;
        setValue(value());
    }

    /// Types text into the box and presses Enter, as a student would. For tests
    /// and scripts.
    /// @param text A number or a formula.
    void type(const QString& text) {
        startTyping();
        lineEdit()->setText(text);
        commit();
        interpretText();
        stopTyping();
        emit editingFinished();
    }

    /// Takes any text when the box takes formulas, so a formula can be typed.
    /// @param input The text so far.
    /// @param pos The cursor's place in it.
    /// @return Whether the text can stay.
    QValidator::State validate(QString& input, int& pos) const override {
        if (!handler_ || !isFormula(input)) return QDoubleSpinBox::validate(input, pos);
        return QValidator::Acceptable;
    }

    /// Reads the box's text. A formula, or text that hasn't changed, leaves the
    /// number as it is.
    /// @param text The text.
    /// @return The number.
    double valueFromText(const QString& text) const override {
        if (handler_ && isFormula(text)) return value();
        // Enter on an untouched box mustn't round the number to what's shown.
        if (text == prefix() + textFromValue(value()) + suffix()) return value();
        return QDoubleSpinBox::valueFromText(text);
    }

    /// Writes a number for the box.
    /// @param v The number.
    /// @return Its text, trimmed if setTrimZeros() is on.
    QString textFromValue(double v) const override {
        QString text = QDoubleSpinBox::textFromValue(v);
        if (!trimZeros_) return text;
        const QString point = locale().decimalPoint();
        if (text.contains(point)) {
            while (text.endsWith(QLatin1Char('0'))) text.chop(1);
            if (text.endsWith(point)) text.chop(point.size());
        }
        return text;
    }

protected:
    /// Shows the formula, ready to change, when the box is clicked or tabbed into.
    /// The unit and any label in the box are hidden while a formula is typed, so
    /// what's typed is the whole formula.
    /// @param event The event.
    void focusInEvent(QFocusEvent* event) override {
        QDoubleSpinBox::focusInEvent(event);
        if (!formula_.isEmpty()) startTyping();
    }

    /// Takes what was typed when the box is left.
    /// @param event The event.
    void focusOutEvent(QFocusEvent* event) override {
        commit();
        QDoubleSpinBox::focusOutEvent(event);
        stopTyping();
    }

    /// Keeps the "fx" mark at the left of the box.
    /// @param event The event.
    void resizeEvent(QResizeEvent* event) override {
        QDoubleSpinBox::resizeEvent(event);
        placeMark();
    }

    /// Takes what was typed on Enter.
    /// @param event The event.
    void keyPressEvent(QKeyEvent* event) override {
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
            commit();
        } else if (handler_ && !typing_ && startsFormula(*event)) {
            // The first letter of a formula (or a paste): the unit goes, the text stays.
            const QString typed = bare(lineEdit()->text());
            const bool all =
                lineEdit()->selectedText().trimmed() == typed;  // the number, picked to type over
            startTyping();
            lineEdit()->setText(all ? QString() : typed);
        }
        QDoubleSpinBox::keyPressEvent(event);
    }

private:
    // Hides the box's own label and unit, and shows its formula, for typing.
    void startTyping() {
        if (!handler_ || typing_) return;
        typing_ = true;
        prefix_ = prefix();
        suffix_ = suffix();
        setPrefix(QString());
        setSuffix(QString());
        showFormula();
    }

    // True for a key that a plain number doesn't have: a letter, a bracket, a
    // star, or a paste.
    static bool startsFormula(const QKeyEvent& event) {
        if (event.matches(QKeySequence::Paste)) return true;
        const QString text = event.text();
        if (text.isEmpty() || !text.at(0).isPrint()) return false;
        const QChar c = text.at(0);
        return !c.isDigit() && !QStringLiteral("+-., ").contains(c);
    }

    void stopTyping() {
        if (!typing_) return;
        typing_ = false;
        setPrefix(prefix_);
        setSuffix(suffix_);
    }

    // The text without the box's own label and unit.
    QString bare(QString text) const {
        if (!prefix().isEmpty() && text.startsWith(prefix())) text.remove(0, prefix().size());
        if (!suffix().isEmpty() && text.endsWith(suffix())) text.chop(suffix().size());
        return text.trimmed();
    }

    // True if the text is something other than a plain number.
    bool isFormula(const QString& text) const {
        const QString t = bare(text);
        if (t.isEmpty()) return false;
        bool number = false;
        locale().toDouble(t, &number);
        if (!number) t.toDouble(&number);
        return !number;
    }

    void showFormula() {
        if (formula_.isEmpty() || !handler_) return;
        lineEdit()->setText(formula_);
        lineEdit()->selectAll();
    }

    // If a formula was typed, puts the number back in the box and hands the
    // formula on, once the box has finished with this key or click: the handler
    // may rebuild the panel, which deletes the box.
    void commit() {
        if (!handler_ || !isFormula(lineEdit()->text())) return;
        const QString typed = bare(lineEdit()->text());
        lineEdit()->setText(prefix() + textFromValue(value()) + suffix());
        if (typed == formula_) return;
        QTimer::singleShot(0, this, [box = QPointer<FormulaBox>(this), typed] {
            if (box && box->handler_) {
                const Handler handler = box->handler_;  // a copy: the box may go while it runs
                handler(typed);
            }
        });
    }

    void placeMark() {
        if (!mark_) return;
        const int width = mark_->sizeHint().width() + 4;
        mark_->setGeometry(1, 0, width, lineEdit()->height());
        lineEdit()->setTextMargins(width, 0, 0, 0);
    }

    Handler handler_;
    QString formula_;
    QLabel* mark_ = nullptr;
    QString prefix_, suffix_;  // the box's own, while they're hidden for typing
    bool typing_ = false;
    bool trimZeros_ = false;
};

}  // namespace sb::ui
