#include "app/theme.hpp"

#include <memory>
#include <utility>

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFontDatabase>
#include <QHash>
#include <QIconEngine>
#include <QPainter>
#include <QPalette>
#include <QStyle>
#include <QStyleFactory>
#include <QSvgRenderer>
#include <QTemporaryDir>

#include "core/log.hpp"

namespace lb::app::theme {
namespace {

Palette makeDark() {
    Palette p;
    p.window = QColor(0x12, 0x13, 0x17);
    p.panel = QColor(0x19, 0x1B, 0x20);
    p.raised = QColor(0x22, 0x25, 0x2B);
    p.hover = QColor(0x2A, 0x2E, 0x35);
    p.pressed = QColor(0x32, 0x36, 0x3E);
    p.border = QColor(0x2A, 0x2D, 0x34);
    p.borderStrong = QColor(0x3A, 0x3F, 0x48);
    p.text = QColor(0xE8, 0xEA, 0xEE);
    p.textMuted = QColor(0x9A, 0xA0, 0xAB);
    p.textDisabled = QColor(0x5D, 0x63, 0x6E);
    p.textOnAccent = QColor(0xFF, 0xFF, 0xFF);
    p.accent = QColor(0x1F, 0xA7, 0xF0);
    p.accentHover = QColor(0x47, 0xB8, 0xF4);
    p.accentPressed = QColor(0x14, 0x89, 0xC9);
    p.selection = QColor(0x1F, 0xA7, 0xF0, 0x40);
    p.axisX = QColor(0xEF, 0x5B, 0x5B);
    p.axisY = QColor(0x6C, 0xC9, 0x5C);
    p.axisZ = QColor(0x4D, 0x9D, 0xF5);
    p.danger = QColor(0xF0, 0x56, 0x4B);
    return p;
}

Palette makeLight() {
    Palette p;
    p.window = QColor(0xE9, 0xEC, 0xF0);
    p.panel = QColor(0xF7, 0xF8, 0xFA);
    p.raised = QColor(0xFF, 0xFF, 0xFF);
    p.hover = QColor(0xE8, 0xEB, 0xF0);
    p.pressed = QColor(0xDC, 0xE0, 0xE7);
    p.border = QColor(0xDD, 0xE1, 0xE7);
    p.borderStrong = QColor(0xC4, 0xCA, 0xD3);
    p.text = QColor(0x1A, 0x1D, 0x24);
    p.textMuted = QColor(0x5E, 0x65, 0x73);
    p.textDisabled = QColor(0xA5, 0xAB, 0xB6);
    p.textOnAccent = QColor(0xFF, 0xFF, 0xFF);
    p.accent = QColor(0x0B, 0x93, 0xDE);
    p.accentHover = QColor(0x1F, 0xA7, 0xF0);
    p.accentPressed = QColor(0x0A, 0x7D, 0xBD);
    p.selection = QColor(0x0B, 0x93, 0xDE, 0x30);
    p.axisX = QColor(0xD6, 0x3C, 0x3C);
    p.axisY = QColor(0x3C, 0x9A, 0x2E);
    p.axisZ = QColor(0x1E, 0x72, 0xD8);
    p.danger = QColor(0xD9, 0x3A, 0x2F);
    return p;
}

struct State {
    Mode mode = Mode::Dark;
    Metrics metrics;
    bool fontsLoaded = false;
    std::unique_ptr<QTemporaryDir> assetDir; // recolored SVGs referenced from the stylesheet
};

State& state() {
    static State s;
    return s;
}

QString css(const QColor& color) {
    if (color.alpha() == 255) {
        return color.name(QColor::HexRgb);
    }
    return QStringLiteral("rgba(%1, %2, %3, %4)")
        .arg(color.red())
        .arg(color.green())
        .arg(color.blue())
        .arg(color.alpha());
}

QByteArray readResource(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        log::error("Missing UI resource {}", path.toStdString());
        return {};
    }
    return file.readAll();
}

QByteArray recolor(QByteArray svg, const QColor& color) {
    return svg.replace("currentColor", color.name(QColor::HexRgb).toLatin1());
}

// Stylesheet images cannot be recolored at paint time, so per-theme copies are written to a temp dir.
QString themedAsset(const QString& icon, const QColor& color) {
    State& s = state();
    if (!s.assetDir) {
        s.assetDir = std::make_unique<QTemporaryDir>();
    }
    const QString path = s.assetDir->filePath(
        QStringLiteral("%1-%2.svg").arg(icon, color.name(QColor::HexRgb).mid(1)));
    if (!QFile::exists(path)) {
        QFile file(path);
        if (file.open(QIODevice::WriteOnly)) {
            file.write(recolor(readResource(QStringLiteral(":/icons/%1.svg").arg(icon)), color));
        }
    }
    return QDir::fromNativeSeparators(path);
}

void loadFonts() {
    State& s = state();
    if (s.fontsLoaded) {
        return;
    }
    for (const char* file : {"Inter-Regular.ttf", "Inter-Medium.ttf", "Inter-SemiBold.ttf"}) {
        if (QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/%1").arg(file)) < 0) {
            log::warn("Could not load bundled font {}", file);
        }
    }
    s.fontsLoaded = true;
}

QPalette makeQPalette(const Palette& p) {
    QPalette q;
    q.setColor(QPalette::Window, p.window);
    q.setColor(QPalette::WindowText, p.text);
    q.setColor(QPalette::Base, p.raised);
    q.setColor(QPalette::AlternateBase, p.panel);
    q.setColor(QPalette::ToolTipBase, p.raised);
    q.setColor(QPalette::ToolTipText, p.text);
    q.setColor(QPalette::PlaceholderText, p.textMuted);
    q.setColor(QPalette::Text, p.text);
    q.setColor(QPalette::Button, p.raised);
    q.setColor(QPalette::ButtonText, p.text);
    q.setColor(QPalette::BrightText, p.textOnAccent);
    q.setColor(QPalette::Light, p.hover);
    q.setColor(QPalette::Midlight, p.raised);
    q.setColor(QPalette::Mid, p.borderStrong);
    q.setColor(QPalette::Dark, p.border);
    q.setColor(QPalette::Shadow, Qt::black);
    q.setColor(QPalette::Highlight, p.accent);
    q.setColor(QPalette::HighlightedText, p.textOnAccent);
    q.setColor(QPalette::Link, p.accent);
    q.setColor(QPalette::LinkVisited, p.accentPressed);
    for (QPalette::ColorRole role : {QPalette::Text, QPalette::WindowText, QPalette::ButtonText}) {
        q.setColor(QPalette::Disabled, role, p.textDisabled);
    }
    return q;
}

// Roles available to widgets through dynamic properties:
//   QLabel[role=title|heading|muted|section|axisX|axisY|axisZ|property], QFrame[role=separator],
//   QPushButton[accent=true], QPushButton[compact=true] (dense, checkable), QPushButton[segment=
//   first|middle|last] (segmented control), QToolButton[role=sectionToggle], QLineEdit[flat=true],
//   QWidget[panel=true]. Textures: QWidget[role=textureSlot] (+ [dropTarget=true] while a drop
//   hovers), QToolButton[role=textureThumb], QListView[role=textureGrid], QLabel[role=
//   texturePreview]. Start screen: QWidget[role=startSidebar], QPushButton[role=startAction],
//   QLabel[role=display] (large page title), QListView[role=projectGrid] (cards paint themselves).
constexpr const char* kStyleTemplate = R"QSS(
QMainWindow, QDialog { background: {window}; }
QMainWindow::separator { background: {window}; width: 3px; height: 3px; }
QMainWindow::separator:hover { background: {accent}; }

QDockWidget { color: {text}; }
QLabel[role="panelTitle"] { background: {panel}; color: {text}; font-size: 13px; font-weight: 600; padding: 10px 12px; border-bottom: 1px solid {border}; }
QWidget[panel="true"] { background: {panel}; }

QMenuBar { background: {window}; color: {text}; border-bottom: 1px solid {border}; padding: 2px 4px; }
QMenuBar::item { padding: 4px 10px; border-radius: {radiusSmall}px; background: transparent; }
QMenuBar::item:selected { background: {hover}; }
QMenu { background: {raised}; color: {text}; border: 1px solid {borderStrong}; border-radius: {radius}px; padding: 4px; }
QMenu::item { padding: 6px 28px 6px 26px; border-radius: {radiusSmall}px; }
QMenu::item:selected { background: {accent}; color: {textOnAccent}; }
QMenu::item:disabled { color: {textDisabled}; }
QMenu::separator { height: 1px; background: {border}; margin: 4px 8px; }
QMenu::indicator { width: 14px; height: 14px; left: 6px; }
QMenu::indicator:checked { image: url({checkText}); }

QStatusBar { background: {window}; color: {textMuted}; border-top: 1px solid {border}; }
QStatusBar::item { border: none; }
QStatusBar QLabel { color: {textMuted}; padding: 0 8px; }
QToolTip { background: {raised}; color: {text}; border: 1px solid {borderStrong}; border-radius: {radiusSmall}px; padding: 5px 8px; }

QToolBar { background: {panel}; border: none; border-bottom: 1px solid {border}; padding: 4px 6px; spacing: 2px; }
QToolBar::separator { background: {border}; width: 1px; margin: 4px 6px; }
QToolButton { background: transparent; color: {text}; border: 1px solid transparent; border-radius: {radiusSmall}px; padding: 4px; }
QToolButton:hover { background: {hover}; }
QToolButton:pressed { background: {pressed}; }
QToolButton:checked { background: {selection}; }
QToolButton:disabled { color: {textDisabled}; }
QToolBar#header { padding: 6px 10px; spacing: 4px; }
QToolBar#header QToolButton[mode="true"] { color: {textMuted}; padding: 5px 12px; border-radius: {radius}px; }
QToolBar#header QToolButton[mode="true"]:hover { color: {text}; background: {hover}; }
QToolBar#header QToolButton[mode="true"]:checked { color: {text}; background: {selection}; font-weight: 600; }

QPushButton { background: {raised}; color: {text}; border: 1px solid {borderStrong}; border-radius: {radius}px; padding: 5px 14px; min-height: 18px; }
QPushButton:hover { background: {hover}; }
QPushButton:pressed { background: {pressed}; }
QPushButton:disabled { color: {textDisabled}; border-color: {border}; background: {panel}; }
QPushButton[accent="true"] { background: {accent}; border-color: {accent}; color: {textOnAccent}; font-weight: 600; }
QPushButton[accent="true"]:hover { background: {accentHover}; border-color: {accentHover}; }
QPushButton[accent="true"]:pressed { background: {accentPressed}; border-color: {accentPressed}; }
QPushButton[accent="true"]:disabled { background: {selection}; border-color: transparent; color: {textDisabled}; }
QPushButton[compact="true"] { padding: 3px 8px; min-height: 16px; }
QPushButton:checked { background: {selection}; border-color: {accent}; }
QPushButton[segment] { padding: 3px 10px; min-height: 16px; border-radius: 0px; color: {textMuted}; }
QPushButton[segment="first"] { border-top-left-radius: {radiusSmall}px; border-bottom-left-radius: {radiusSmall}px; }
QPushButton[segment="middle"] { border-left: none; }
QPushButton[segment="last"] { border-left: none; border-top-right-radius: {radiusSmall}px; border-bottom-right-radius: {radiusSmall}px; }
QPushButton[segment]:checked { color: {text}; background: {selection}; border-color: {borderStrong}; font-weight: 600; }
QToolButton[role="sectionToggle"] { color: {text}; font-size: 12px; font-weight: 600; text-align: left; padding: 6px 4px; border: none; border-top: 1px solid {border}; border-radius: 0px; }
QToolButton[role="sectionToggle"]:hover { background: {hover}; }
QToolButton[role="sectionToggle"]:checked { background: transparent; }

QLineEdit, QAbstractSpinBox, QComboBox { background: {raised}; color: {text}; border: 1px solid {borderStrong}; border-radius: {radiusSmall}px; padding: 3px 8px; min-height: 20px; selection-background-color: {accent}; selection-color: {textOnAccent}; }
QLineEdit:hover, QAbstractSpinBox:hover, QComboBox:hover { border-color: {textDisabled}; }
QLineEdit:focus, QAbstractSpinBox:focus, QComboBox:focus { border-color: {accent}; }
QLineEdit:disabled, QAbstractSpinBox:disabled, QComboBox:disabled { color: {textDisabled}; background: {panel}; border-color: {border}; }
QLineEdit[flat="true"] { background: transparent; border-color: transparent; padding-left: 4px; }
QLineEdit[flat="true"]:hover { border-color: {borderStrong}; }
QLineEdit[flat="true"]:focus { background: {raised}; border-color: {accent}; }
QLineEdit[role="title"] { font-size: 15px; font-weight: 600; }
QAbstractSpinBox { padding-left: 4px; padding-right: 4px; }
QAbstractSpinBox::up-button, QAbstractSpinBox::down-button { width: 0px; border: none; }
QComboBox { padding-right: 24px; }
QComboBox::drop-down { border: none; width: 22px; subcontrol-origin: padding; subcontrol-position: center right; }
QComboBox::down-arrow { image: url({chevron}); width: 14px; height: 14px; }
QComboBox QAbstractItemView { background: {raised}; color: {text}; border: 1px solid {borderStrong}; selection-background-color: {selection}; selection-color: {text}; outline: 0; padding: 4px; }

QCheckBox, QRadioButton { color: {text}; spacing: 8px; }
QCheckBox:disabled, QRadioButton:disabled { color: {textDisabled}; }
QCheckBox::indicator { width: 14px; height: 14px; border-radius: 4px; border: 1px solid {borderStrong}; background: {raised}; }
QCheckBox::indicator:hover { border-color: {accent}; }
QCheckBox::indicator:checked { background: {accent}; border-color: {accent}; image: url({checkOnAccent}); }
QCheckBox::indicator:disabled { background: {panel}; border-color: {border}; }
QRadioButton::indicator { width: 14px; height: 14px; border-radius: 8px; border: 1px solid {borderStrong}; background: {raised}; }
QRadioButton::indicator:hover { border-color: {accent}; }
QRadioButton::indicator:checked { width: 6px; height: 6px; border: 5px solid {accent}; background: {textOnAccent}; }

QSlider::groove:horizontal { height: 4px; background: {borderStrong}; border-radius: 2px; }
QSlider::sub-page:horizontal { background: {accent}; border-radius: 2px; }
QSlider::handle:horizontal { width: 14px; height: 14px; margin: -6px 0; border-radius: 8px; background: {textOnAccent}; border: 1px solid {borderStrong}; }
QSlider::handle:horizontal:hover { border-color: {accent}; }

QProgressBar { background: {raised}; border: 1px solid {border}; border-radius: 4px; max-height: 8px; color: transparent; }
QProgressBar::chunk { background: {accent}; border-radius: 3px; }

QScrollBar:vertical { background: transparent; width: 10px; margin: 2px; }
QScrollBar::handle:vertical { background: {borderStrong}; border-radius: 3px; min-height: 28px; }
QScrollBar::handle:vertical:hover { background: {textDisabled}; }
QScrollBar:horizontal { background: transparent; height: 10px; margin: 2px; }
QScrollBar::handle:horizontal { background: {borderStrong}; border-radius: 3px; min-width: 28px; }
QScrollBar::handle:horizontal:hover { background: {textDisabled}; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0px; height: 0px; }
QScrollBar::add-page, QScrollBar::sub-page { background: none; }

QTreeView, QListView, QTableView { background: {panel}; color: {text}; border: none; outline: 0; selection-background-color: {selection}; selection-color: {text}; }
QTreeView::item, QListView::item { padding: 3px 4px; border-radius: {radiusSmall}px; }
QTreeView::item:hover, QListView::item:hover { background: {hover}; }
QTreeView::item:selected, QListView::item:selected { background: {selection}; color: {text}; }
QHeaderView::section { background: {panel}; color: {textMuted}; border: none; border-bottom: 1px solid {border}; padding: 4px 8px; }

QTabWidget::pane { border: 1px solid {border}; border-radius: {radius}px; top: -1px; background: {panel}; }
QTabBar::tab { background: transparent; color: {textMuted}; padding: 6px 14px; border: none; border-bottom: 2px solid transparent; }
QTabBar::tab:hover { color: {text}; }
QTabBar::tab:selected { color: {text}; border-bottom-color: {accent}; }

QGroupBox { border: 1px solid {border}; border-radius: {radius}px; margin-top: 16px; padding: 12px 8px 8px 8px; color: {textMuted}; }
QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 4px; }

QScrollArea { background: transparent; border: none; }
QLabel { color: {text}; background: transparent; }
QLabel[role="title"] { font-size: 18px; font-weight: 600; }
QLabel[role="heading"] { font-size: 14px; font-weight: 600; }
QLabel[role="muted"] { color: {textMuted}; }
QLabel[role="section"] { color: {textMuted}; font-size: 11px; font-weight: 600; }
QLabel[role="property"] { color: {textMuted}; }
QLabel[role="axisX"] { color: {axisX}; font-weight: 600; }
QLabel[role="axisY"] { color: {axisY}; font-weight: 600; }
QLabel[role="axisZ"] { color: {axisZ}; font-weight: 600; }
QFrame[role="separator"] { background: {border}; border: none; min-height: 1px; max-height: 1px; }
QSplitter::handle { background: {border}; }

QWidget[role="textureSlot"][dropTarget="true"] { background: {selection}; border-radius: {radiusSmall}px; }
QToolButton[role="textureThumb"] { background: {raised}; border: 1px solid {borderStrong}; border-radius: {radiusSmall}px; padding: 1px; }
QToolButton[role="textureThumb"]:hover { border-color: {accent}; }
QListView[role="textureGrid"]::item { padding: 4px 2px; }
QLabel[role="texturePreview"] { background: {window}; border: 1px solid {border}; border-radius: {radius}px; padding: 8px; }

QWidget[role="startSidebar"] { background: {panel}; border-right: 1px solid {border}; }
QPushButton[role="startAction"] { padding: 9px 14px; font-size: 13px; text-align: left; }
QLabel[role="display"] { font-size: 22px; font-weight: 600; }
QListView[role="projectGrid"] { background: transparent; }
QListView[role="projectGrid"]::item, QListView[role="projectGrid"]::item:hover, QListView[role="projectGrid"]::item:selected { background: transparent; padding: 0px; }
)QSS";

QString buildStyleSheet(const Palette& p, const Metrics& m) {
    QString qss = QString::fromUtf8(kStyleTemplate);
    const std::pair<const char*, QString> tokens[] = {
        {"window", css(p.window)},
        {"panel", css(p.panel)},
        {"raised", css(p.raised)},
        {"hover", css(p.hover)},
        {"pressed", css(p.pressed)},
        {"borderStrong", css(p.borderStrong)},
        {"border", css(p.border)},
        {"textMuted", css(p.textMuted)},
        {"textDisabled", css(p.textDisabled)},
        {"textOnAccent", css(p.textOnAccent)},
        {"text", css(p.text)},
        {"accentHover", css(p.accentHover)},
        {"accentPressed", css(p.accentPressed)},
        {"accent", css(p.accent)},
        {"selection", css(p.selection)},
        {"axisX", css(p.axisX)},
        {"axisY", css(p.axisY)},
        {"axisZ", css(p.axisZ)},
        {"radiusSmall", QString::number(m.radiusSmall)},
        {"radius", QString::number(m.radius)},
        {"chevron", themedAsset(QStringLiteral("chevron-down"), p.textMuted)},
        {"checkOnAccent", themedAsset(QStringLiteral("check"), p.textOnAccent)},
        {"checkText", themedAsset(QStringLiteral("check"), p.text)},
    };
    for (const auto& [name, value] : tokens) {
        qss.replace(QLatin1Char('{') + QLatin1String(name) + QLatin1Char('}'), value);
    }
    return qss;
}

// Renders a Lucide SVG in the current theme's colors each time it is painted, so icons follow live
// theme switches without widgets having to refresh them.
class ThemedSvgIconEngine final : public QIconEngine {
public:
    explicit ThemedSvgIconEngine(QByteArray svg) : svg_(std::move(svg)) {}

    QIconEngine* clone() const override { return new ThemedSvgIconEngine(svg_); }
    QString key() const override { return QStringLiteral("lightbox.themed-svg"); }
    QSize actualSize(const QSize& size, QIcon::Mode, QIcon::State) override { return size; }

    void paint(QPainter* painter, const QRect& rect, QIcon::Mode mode, QIcon::State state) override {
        const qreal scale = painter->device() ? painter->device()->devicePixelRatio() : 1.0;
        painter->drawPixmap(rect, scaledPixmap(rect.size(), mode, state, scale));
    }

    QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override {
        return scaledPixmap(size, mode, state, 1.0);
    }

    QPixmap scaledPixmap(const QSize& size, QIcon::Mode mode, QIcon::State state,
                         qreal scale) override {
        const QColor color = colorFor(mode, state);
        const QSize pixels = (QSizeF(size) * scale).toSize();
        const QString cacheKey = QStringLiteral("%1x%2-%3")
                                     .arg(pixels.width())
                                     .arg(pixels.height())
                                     .arg(color.rgba(), 8, 16, QLatin1Char('0'));
        if (const auto it = cache_.constFind(cacheKey); it != cache_.constEnd()) {
            return *it;
        }
        QPixmap pixmap(pixels);
        pixmap.fill(Qt::transparent);
        QSvgRenderer renderer(recolor(svg_, color));
        QPainter painter(&pixmap);
        renderer.render(&painter, QRectF(QPointF(0, 0), QSizeF(pixels)));
        painter.end();
        pixmap.setDevicePixelRatio(scale);
        if (cache_.size() > 32) {
            cache_.clear();
        }
        cache_.insert(cacheKey, pixmap);
        return pixmap;
    }

private:
    static QColor colorFor(QIcon::Mode mode, QIcon::State state) {
        const Palette& p = palette();
        if (mode == QIcon::Disabled) {
            return p.textDisabled;
        }
        if (state == QIcon::On) {
            return p.accent;
        }
        return mode == QIcon::Normal ? p.textMuted : p.text; // brighten on hover / selection
    }

    QByteArray svg_;
    QHash<QString, QPixmap> cache_;
};

} // namespace

Mode mode() {
    return state().mode;
}

const Palette& paletteFor(Mode m) {
    static const Palette dark = makeDark();
    static const Palette light = makeLight();
    return m == Mode::Dark ? dark : light;
}

const Palette& palette() {
    return paletteFor(state().mode);
}

const Metrics& metrics() {
    return state().metrics;
}

QString modeName(Mode m) {
    return m == Mode::Dark ? QStringLiteral("dark") : QStringLiteral("light");
}

std::optional<Mode> parseMode(const QString& name) {
    if (name.compare(QLatin1String("dark"), Qt::CaseInsensitive) == 0) {
        return Mode::Dark;
    }
    if (name.compare(QLatin1String("light"), Qt::CaseInsensitive) == 0) {
        return Mode::Light;
    }
    return std::nullopt;
}

void apply(QApplication& app, Mode m) {
    loadFonts();
    state().mode = m;
    const Palette& p = palette();

    if (!app.style() || app.style()->name().compare(QLatin1String("fusion"), Qt::CaseInsensitive)) {
        app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    }
    QFont font(QStringLiteral("Inter"));
    font.setPointSizeF(9.75);
    app.setFont(font);
    app.setPalette(makeQPalette(p));
    app.setStyleSheet(buildStyleSheet(p, metrics()));
}

QIcon icon(const QString& name) {
    return QIcon(new ThemedSvgIconEngine(readResource(QStringLiteral(":/icons/%1.svg").arg(name))));
}

QIcon appIcon() {
    QIcon result;
    for (int size : {16, 24, 32, 48, 64, 128, 256}) {
        result.addFile(QStringLiteral(":/branding/lightbox-icon-%1.png").arg(size), QSize(size, size));
    }
    return result;
}

} // namespace lb::app::theme
