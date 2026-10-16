#pragma once

#include <optional>

#include <QColor>
#include <QIcon>
#include <QString>

class QApplication;

// LightBox design system: color tokens, metrics, typography (Inter), the generated application
// stylesheet and themed icons. Widgets never hard-code colors; they use these tokens or the QSS roles
// documented in theme.cpp. See docs/ARCHITECTURE.md §12.
namespace lb::app::theme {

enum class Mode { Dark, Light };

struct Palette {
    QColor window;       // application background, gaps between panels
    QColor panel;        // dock/panel surfaces
    QColor raised;       // inputs, buttons, popups
    QColor hover;        // hovered controls and rows
    QColor pressed;      // pressed controls
    QColor border;       // hairlines between surfaces
    QColor borderStrong; // control outlines
    QColor text;
    QColor textMuted;    // labels, secondary text
    QColor textDisabled;
    QColor textOnAccent;
    QColor accent; // brand azure (logo top face)
    QColor accentHover;
    QColor accentPressed;
    QColor selection; // translucent accent behind selected rows/toggles
    QColor axisX;     // red / green / blue axis labels, matching the viewport grid
    QColor axisY;
    QColor axisZ;
    QColor danger;
};

struct Metrics {
    int radius = 6;      // buttons, panels, popups
    int radiusSmall = 4; // inputs, tool buttons, list rows
    int spacing = 8;     // base spacing unit
    int iconSize = 18;   // toolbar icons (logical px)
};

Mode mode();
const Palette& palette();
const Metrics& metrics();
const Palette& paletteFor(Mode mode);

QString modeName(Mode mode); // "dark" / "light", used in settings and on the command line
std::optional<Mode> parseMode(const QString& name);

// Loads the bundled fonts on first use, then applies palette, font and stylesheet. Call again at any
// time to switch the theme live; themed icons repaint in the new colors automatically.
void apply(QApplication& app, Mode mode);

// Icon from the bundled Lucide set (":/icons/<name>.svg"), drawn in the current theme's text color
// (accent when checked, muted when disabled) and recolored whenever the theme changes.
QIcon icon(const QString& name);

QIcon appIcon();

} // namespace lb::app::theme
