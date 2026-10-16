#pragma once

#include <QString>
#include <QStringList>
#include <QWidget>

class QButtonGroup;
class QDoubleSpinBox;
class QGridLayout;
class QLabel;
class QLayout;
class QSlider;
class QToolButton;
class QVariant;
class QVBoxLayout;

// Dense property editing widgets shared by the mode panels (Light, Render, Materials…): collapsible
// sections with a label column, slider + number fields and segmented choices. Setters never emit the
// "edited" signals, so panels can refresh from the document without feedback loops.
namespace lb::app::ui {

// Scrollable panel page: returns the vertical layout to fill. Add a stretch at the end.
QVBoxLayout* scrollPage(QWidget* panel);

// Typed number field styled like SliderField's: no spin buttons, commits on Enter or focus loss.
// (Named valueField: panels with a Section* argument would otherwise find it via ADL next to their
// own local numberField helpers.)
QDoubleSpinBox* valueField(double min, double max, int decimals, const QString& suffix,
                           QWidget* parent);

// Sets a dynamic property that stylesheet selectors match on (theme.cpp roles) and re-polishes the
// widget, because Qt only re-evaluates property selectors on polish.
void setStyleProperty(QWidget* widget, const char* name, const QVariant& value);

// One label + editor line inside a Section.
struct PropertyRow {
    QLabel* label = nullptr;
    QWidget* editor = nullptr;
    void setVisible(bool visible) const;
};

// Collapsible titled group of property rows. All sections share the label column width, so editors
// line up across a panel.
class Section : public QWidget {
    Q_OBJECT

public:
    Section(const QString& title, QWidget* parent, bool expanded = true);

    PropertyRow addRow(const QString& label, QWidget* editor);
    PropertyRow addRow(const QString& label, QLayout* editor);
    // Full-width line (buttons, hints, check boxes).
    QWidget* addWide(QWidget* widget);
    QWidget* addWide(QLayout* layout);

    void setExpanded(bool expanded);
    bool isExpanded() const;

private:
    QToolButton* toggle_ = nullptr;
    QWidget* body_ = nullptr;
    QGridLayout* grid_ = nullptr;
    int rows_ = 0;
};

// Slider with a typed number field. The slider covers [min, max] linearly; the field accepts the same
// range at `decimals` precision.
class SliderField : public QWidget {
    Q_OBJECT

public:
    SliderField(double min, double max, int decimals, const QString& suffix, QWidget* parent);

    double value() const;
    void setValue(double value); // does not emit valueEdited

signals:
    // The user moved the slider (continuously while dragging) or committed a typed value.
    void valueEdited(double value);

private:
    int toSlider(double value) const;
    double fromSlider(int position) const;

    double min_;
    double max_;
    QSlider* slider_ = nullptr;
    QDoubleSpinBox* field_ = nullptr;
    bool updating_ = false;
};

// Row of exclusive toggle buttons (like a tab strip for settings).
class SegmentedControl : public QWidget {
    Q_OBJECT

public:
    SegmentedControl(const QStringList& options, QWidget* parent);

    int current() const;
    void setCurrent(int index); // does not emit currentEdited

signals:
    void currentEdited(int index);

private:
    QButtonGroup* group_ = nullptr;
};

} // namespace lb::app::ui
