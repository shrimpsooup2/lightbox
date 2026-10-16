#include "app/design_gallery.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QSlider>
#include <QStandardItemModel>
#include <QTabWidget>
#include <QToolButton>
#include <QTreeView>
#include <QVBoxLayout>

#include "app/theme.hpp"

namespace lb::app {
namespace {

QLabel* label(const QString& text, const char* role, QWidget* parent) {
    auto* l = new QLabel(text, parent);
    if (role != nullptr) {
        l->setProperty("role", role);
    }
    return l;
}

QWidget* section(const QString& title, QLayout* content, QWidget* parent) {
    auto* box = new QWidget(parent);
    auto* layout = new QVBoxLayout(box);
    layout->setContentsMargins(0, 8, 0, 8);
    layout->setSpacing(10);
    layout->addWidget(label(title.toUpper(), "section", box));
    auto* line = new QFrame(box);
    line->setProperty("role", "separator");
    layout->addWidget(line);
    layout->addLayout(content);
    return box;
}

// Swatch that paints a palette token by name, so it follows live theme switches.
class TokenSwatch : public QWidget {
public:
    TokenSwatch(QColor theme::Palette::*token, const QString& name, QWidget* parent)
        : QWidget(parent), token_(token), name_(name) {
        setMinimumSize(120, 52);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const theme::Palette& p = theme::palette();
        painter.setPen(QPen(p.border, 1.0));
        painter.setBrush(p.*token_);
        painter.drawRoundedRect(QRectF(0.5, 0.5, width() - 1.0, 28.0), 6.0, 6.0);
        painter.setPen(p.textMuted);
        painter.drawText(QRect(0, 32, width(), 20), Qt::AlignLeft | Qt::AlignVCenter, name_);
    }

private:
    QColor theme::Palette::*token_;
    QString name_;
};

} // namespace

DesignGallery::DesignGallery(QWidget* parent) : QWidget(parent) {
    setWindowTitle(tr("LightBox Design Gallery"));
    setProperty("panel", true);
    resize(980, 860);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    auto* page = new QWidget(scroll);
    page_ = page;
    page->setProperty("panel", true);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(28, 24, 28, 28);
    layout->setSpacing(8);

    auto* header = new QHBoxLayout;
    auto* titles = new QVBoxLayout;
    titles->addWidget(label(tr("Design gallery"), "title", page));
    titles->addWidget(label(tr("Every control in the LightBox design system. Switch themes to review "
                               "both palettes."),
                            "muted", page));
    header->addLayout(titles, 1);
    themeButton_ = new QPushButton(page);
    header->addWidget(themeButton_, 0, Qt::AlignTop);
    layout->addLayout(header);

    // Color tokens.
    auto* colors = new QGridLayout;
    colors->setHorizontalSpacing(12);
    colors->setVerticalSpacing(8);
    const std::pair<QColor theme::Palette::*, const char*> tokens[] = {
        {&theme::Palette::window, "window"},     {&theme::Palette::panel, "panel"},
        {&theme::Palette::raised, "raised"},     {&theme::Palette::hover, "hover"},
        {&theme::Palette::border, "border"},     {&theme::Palette::borderStrong, "borderStrong"},
        {&theme::Palette::text, "text"},         {&theme::Palette::textMuted, "textMuted"},
        {&theme::Palette::accent, "accent"},     {&theme::Palette::selection, "selection"},
        {&theme::Palette::axisX, "axisX"},       {&theme::Palette::danger, "danger"},
    };
    int index = 0;
    for (const auto& [token, name] : tokens) {
        colors->addWidget(new TokenSwatch(token, QString::fromLatin1(name), page), index / 6,
                          index % 6);
        ++index;
    }
    layout->addWidget(section(tr("Color tokens"), colors, page));

    // Typography.
    auto* type = new QVBoxLayout;
    type->addWidget(label(tr("Title — Sun & Sky"), "title", page));
    type->addWidget(label(tr("Heading — Materials"), "heading", page));
    type->addWidget(label(tr("Body — Drop a model or a plan anywhere to import it."), nullptr, page));
    type->addWidget(label(tr("Muted — 24 objects · 3 materials"), "muted", page));
    type->addWidget(label(tr("SECTION LABEL"), "section", page));
    layout->addWidget(section(tr("Typography (Inter)"), type, page));

    // Buttons.
    auto* buttons = new QHBoxLayout;
    buttons->setSpacing(10);
    auto* primary = new QPushButton(tr("Render image"), page);
    primary->setProperty("accent", true);
    auto* primaryDisabled = new QPushButton(tr("Render image"), page);
    primaryDisabled->setProperty("accent", true);
    primaryDisabled->setEnabled(false);
    auto* secondary = new QPushButton(tr("Import…"), page);
    auto* disabled = new QPushButton(tr("Disabled"), page);
    disabled->setEnabled(false);
    buttons->addWidget(primary);
    buttons->addWidget(primaryDisabled);
    buttons->addWidget(secondary);
    buttons->addWidget(disabled);
    for (const char* iconName : {"scan", "rotate-ccw", "grid-3x3"}) {
        auto* tool = new QToolButton(page);
        tool->setIcon(theme::icon(QString::fromLatin1(iconName)));
        tool->setIconSize(QSize(18, 18));
        tool->setCheckable(QString::fromLatin1(iconName) == QLatin1String("grid-3x3"));
        tool->setChecked(tool->isCheckable());
        buttons->addWidget(tool);
    }
    buttons->addStretch(1);
    layout->addWidget(section(tr("Buttons"), buttons, page));

    // Inputs.
    auto* inputs = new QGridLayout;
    inputs->setHorizontalSpacing(12);
    inputs->setVerticalSpacing(10);
    auto* text = new QLineEdit(page);
    text->setPlaceholderText(tr("Search scene"));
    text->addAction(theme::icon(QStringLiteral("search")), QLineEdit::LeadingPosition);
    auto* filled = new QLineEdit(tr("Living room"), page);
    auto* spin = new QDoubleSpinBox(page);
    spin->setDecimals(3);
    spin->setValue(2.750);
    spin->setSuffix(QStringLiteral(" m"));
    spin->setButtonSymbols(QAbstractSpinBox::NoButtons);
    auto* combo = new QComboBox(page);
    combo->addItems({tr("Golden hour"), tr("Overcast"), tr("Blue hour"), tr("Night")});
    inputs->addWidget(text, 0, 0);
    inputs->addWidget(filled, 0, 1);
    inputs->addWidget(spin, 0, 2);
    inputs->addWidget(combo, 0, 3);
    auto* check = new QCheckBox(tr("Show grid"), page);
    check->setChecked(true);
    auto* checkOff = new QCheckBox(tr("Snap to edges"), page);
    auto* checkDisabled = new QCheckBox(tr("Ray tracing (unsupported)"), page);
    checkDisabled->setEnabled(false);
    auto* radioA = new QRadioButton(tr("Metric"), page);
    radioA->setChecked(true);
    auto* radioB = new QRadioButton(tr("Imperial"), page);
    inputs->addWidget(check, 1, 0);
    inputs->addWidget(checkOff, 1, 1);
    inputs->addWidget(checkDisabled, 1, 2);
    auto* radios = new QHBoxLayout;
    radios->addWidget(radioA);
    radios->addWidget(radioB);
    inputs->addLayout(radios, 1, 3);
    auto* slider = new QSlider(Qt::Horizontal, page);
    slider->setRange(0, 100);
    slider->setValue(62);
    auto* progress = new QProgressBar(page);
    progress->setRange(0, 100);
    progress->setValue(40);
    inputs->addWidget(slider, 2, 0, 1, 2);
    inputs->addWidget(progress, 2, 2, 1, 2);
    layout->addWidget(section(tr("Inputs"), inputs, page));

    // Lists and tabs.
    auto* lists = new QHBoxLayout;
    lists->setSpacing(16);
    auto* model = new QStandardItemModel(this);
    auto* house = new QStandardItem(theme::icon(QStringLiteral("folder")), tr("Sample house"));
    for (const QString& name : {tr("Main volume"), tr("Wing"), tr("Stair tower")}) {
        house->appendRow(new QStandardItem(theme::icon(QStringLiteral("box")), name));
    }
    model->appendRow(house);
    auto* tree = new QTreeView(page);
    tree->setModel(model);
    tree->setHeaderHidden(true);
    tree->expandAll();
    tree->setCurrentIndex(model->index(1, 0, house->index()));
    tree->setMinimumHeight(140);
    auto* tabs = new QTabWidget(page);
    tabs->addTab(label(tr("Daylight settings live here."), "muted", tabs), tr("Sun & Sky"));
    tabs->addTab(label(tr("Exposure and white balance."), "muted", tabs), tr("Camera"));
    tabs->addTab(label(tr("Resolution and output."), "muted", tabs), tr("Output"));
    lists->addWidget(tree, 1);
    lists->addWidget(tabs, 1);
    layout->addWidget(section(tr("Lists & tabs"), lists, page));

    // Icons.
    auto* icons = new QHBoxLayout;
    icons->setSpacing(14);
    const QStringList iconFiles =
        QDir(QStringLiteral(":/icons")).entryList({QStringLiteral("*.svg")}, QDir::Files);
    for (const QString& file : iconFiles) {
        auto* cell = new QVBoxLayout;
        auto* tool = new QToolButton(page);
        tool->setIcon(theme::icon(file.chopped(4)));
        tool->setIconSize(QSize(20, 20));
        cell->addWidget(tool, 0, Qt::AlignHCenter);
        cell->addWidget(label(file.chopped(4), "muted", page), 0, Qt::AlignHCenter);
        icons->addLayout(cell);
    }
    icons->addStretch(1);
    layout->addWidget(section(tr("Icons (Lucide)"), icons, page));
    layout->addStretch(1);

    scroll->setWidget(page);
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->addWidget(scroll);

    connect(themeButton_, &QPushButton::clicked, this, [this] {
        const theme::Mode next =
            theme::mode() == theme::Mode::Dark ? theme::Mode::Light : theme::Mode::Dark;
        theme::apply(*qApp, next);
        updateThemeButton();
    });
    updateThemeButton();
}

bool DesignGallery::saveScreenshot(const QString& path) {
    return page_->grab().save(path, "PNG");
}

void DesignGallery::updateThemeButton() {
    const bool dark = theme::mode() == theme::Mode::Dark;
    themeButton_->setIcon(theme::icon(dark ? QStringLiteral("sun") : QStringLiteral("moon")));
    themeButton_->setText(dark ? tr("Light theme") : tr("Dark theme"));
}

} // namespace lb::app
