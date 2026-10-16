#pragma once

#include <QColor>
#include <QIcon>
#include <QSize>
#include <QString>
#include <glm/vec3.hpp>

class QLabel;
class QWidget;

// Small building blocks shared by panels so they look and behave the same.
namespace lb::app::ui {

// QLabel with an optional theme role (see theme.cpp: title, heading, muted, section, axisX…).
QLabel* label(const QString& text, const char* role, QWidget* parent);
// Upper-case section caption with a hairline underneath, as used in Properties.
QWidget* sectionHeader(const QString& title, QWidget* parent);
// Rounded color chip for material swatches.
QIcon colorSwatch(const QColor& color, QSize size = QSize(36, 20));

// "1 object" / "3 objects": `many` contains %1. (Qt's %n plurals need translation files to read well.)
QString count(int n, const QString& one, const QString& many);
// Scene colors are linear; swatches and color pickers show sRGB.
QColor displayColor(const glm::vec3& linear);
glm::vec3 linearColor(const QColor& display);

} // namespace lb::app::ui
