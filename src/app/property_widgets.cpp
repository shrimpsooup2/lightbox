#include "app/property_widgets.hpp"

#include <algorithm>
#include <cmath>

#include <QButtonGroup>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QStyle>
#include <QToolButton>
#include <QVariant>
#include <QVBoxLayout>

#include "app/theme.hpp"
#include "app/ui_helpers.hpp"

namespace lb::app::ui {
namespace {

constexpr int kLabelWidth = 86;
constexpr int kSliderSteps = 1000;

} // namespace

QVBoxLayout* scrollPage(QWidget* panel) {
    panel->setProperty("panel", true);
    auto* scroll = new QScrollArea(panel);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* page = new QWidget(scroll);
    page->setProperty("panel", true);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(10, 8, 10, 12);
    layout->setSpacing(6);
    scroll->setWidget(page);
    auto* outer = new QVBoxLayout(panel);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->addWidget(scroll);
    return layout;
}

void PropertyRow::setVisible(bool visible) const {
    if (label) {
        label->setVisible(visible);
    }
    if (editor) {
        editor->setVisible(visible);
    }
}

QDoubleSpinBox* valueField(double min, double max, int decimals, const QString& suffix,
                           QWidget* parent) {
    auto* field = new QDoubleSpinBox(parent);
    field->setRange(min, max);
    field->setDecimals(decimals);
    field->setSingleStep(std::pow(10.0, -decimals));
    field->setSuffix(suffix);
    field->setButtonSymbols(QAbstractSpinBox::NoButtons);
    field->setKeyboardTracking(false);
    field->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    return field;
}

void setStyleProperty(QWidget* widget, const char* name, const QVariant& value) {
    if (widget->property(name) == value) {
        return;
    }
    widget->setProperty(name, value);
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
    widget->update();
}

Section::Section(const QString& title, QWidget* parent, bool expanded) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    toggle_ = new QToolButton(this);
    toggle_->setProperty("role", "sectionToggle");
    toggle_->setText(title);
    toggle_->setCheckable(true);
    toggle_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    toggle_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    toggle_->setIconSize(QSize(14, 14));
    layout->addWidget(toggle_);

    body_ = new QWidget(this);
    grid_ = new QGridLayout(body_);
    grid_->setContentsMargins(4, 4, 0, 10);
    grid_->setHorizontalSpacing(8);
    grid_->setVerticalSpacing(5);
    grid_->setColumnMinimumWidth(0, kLabelWidth);
    grid_->setColumnStretch(1, 1);
    layout->addWidget(body_);

    connect(toggle_, &QToolButton::toggled, this, &Section::setExpanded);
    setExpanded(expanded);
}

PropertyRow Section::addRow(const QString& text, QWidget* editor) {
    auto* name = label(text, "property", body_);
    name->setMinimumWidth(kLabelWidth);
    grid_->addWidget(name, rows_, 0, Qt::AlignLeft | Qt::AlignVCenter);
    grid_->addWidget(editor, rows_, 1);
    ++rows_;
    return {name, editor};
}

PropertyRow Section::addRow(const QString& text, QLayout* editor) {
    auto* holder = new QWidget(body_);
    editor->setContentsMargins(0, 0, 0, 0);
    holder->setLayout(editor);
    return addRow(text, holder);
}

QWidget* Section::addWide(QWidget* widget) {
    grid_->addWidget(widget, rows_++, 0, 1, 2);
    return widget;
}

QWidget* Section::addWide(QLayout* layout) {
    auto* holder = new QWidget(body_);
    layout->setContentsMargins(0, 0, 0, 0);
    holder->setLayout(layout);
    return addWide(holder);
}

void Section::setExpanded(bool expanded) {
    toggle_->blockSignals(true);
    toggle_->setChecked(expanded);
    toggle_->blockSignals(false);
    toggle_->setIcon(theme::icon(expanded ? QStringLiteral("chevron-down")
                                          : QStringLiteral("chevron-right")));
    body_->setVisible(expanded);
}

bool Section::isExpanded() const {
    return body_->isVisibleTo(const_cast<Section*>(this));
}

SliderField::SliderField(double min, double max, int decimals, const QString& suffix,
                         QWidget* parent)
    : QWidget(parent), min_(min), max_(max) {
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);
    slider_ = new QSlider(Qt::Horizontal, this);
    slider_->setRange(0, kSliderSteps);
    slider_->setPageStep(kSliderSteps / 10);
    field_ = new QDoubleSpinBox(this);
    field_->setRange(min, max);
    field_->setDecimals(decimals);
    field_->setSingleStep(std::pow(10.0, -decimals));
    field_->setSuffix(suffix);
    field_->setButtonSymbols(QAbstractSpinBox::NoButtons);
    field_->setKeyboardTracking(false);
    field_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    field_->setFixedWidth(suffix.size() > 2 ? 72 : 60);
    layout->addWidget(slider_, 1);
    layout->addWidget(field_);

    connect(slider_, &QSlider::valueChanged, this, [this](int position) {
        if (updating_) {
            return;
        }
        const double value = fromSlider(position);
        updating_ = true;
        field_->setValue(value);
        updating_ = false;
        emit valueEdited(field_->value());
    });
    connect(field_, &QDoubleSpinBox::valueChanged, this, [this](double value) {
        if (updating_) {
            return;
        }
        updating_ = true;
        slider_->setValue(toSlider(value));
        updating_ = false;
        emit valueEdited(value);
    });
}

double SliderField::value() const {
    return field_->value();
}

void SliderField::setValue(double value) {
    if (field_->hasFocus() && std::abs(field_->value() - value) < 1e-9) {
        return; // don't disturb typing
    }
    updating_ = true;
    field_->setValue(value);
    slider_->setValue(toSlider(value));
    updating_ = false;
}

int SliderField::toSlider(double value) const {
    const double t = (std::clamp(value, min_, max_) - min_) / (max_ - min_);
    return static_cast<int>(std::lround(t * kSliderSteps));
}

double SliderField::fromSlider(int position) const {
    return min_ + (max_ - min_) * static_cast<double>(position) / kSliderSteps;
}

SegmentedControl::SegmentedControl(const QStringList& options, QWidget* parent) : QWidget(parent) {
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    group_ = new QButtonGroup(this);
    group_->setExclusive(true);
    for (int i = 0; i < options.size(); ++i) {
        auto* button = new QPushButton(options[i], this);
        button->setCheckable(true);
        button->setProperty("segment", i == 0                    ? "first"
                                       : i == options.size() - 1 ? "last"
                                                                 : "middle");
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        group_->addButton(button, i);
        layout->addWidget(button);
    }
    connect(group_, &QButtonGroup::idClicked, this, &SegmentedControl::currentEdited);
}

int SegmentedControl::current() const {
    return group_->checkedId();
}

void SegmentedControl::setCurrent(int index) {
    if (QAbstractButton* button = group_->button(index)) {
        button->setChecked(true);
    }
}

} // namespace lb::app::ui
