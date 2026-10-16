#include "app/ui_helpers.hpp"

#include <QFrame>
#include <QLabel>
#include <QPainter>
#include <QPixmap>
#include <QVBoxLayout>

#include "app/theme.hpp"
#include "core/color.hpp"

namespace lb::app::ui {

QLabel* label(const QString& text, const char* role, QWidget* parent) {
    auto* l = new QLabel(text, parent);
    if (role != nullptr) {
        l->setProperty("role", role);
    }
    return l;
}

QWidget* sectionHeader(const QString& title, QWidget* parent) {
    auto* header = new QWidget(parent);
    auto* layout = new QVBoxLayout(header);
    layout->setContentsMargins(0, 12, 0, 4);
    layout->setSpacing(6);
    layout->addWidget(label(title.toUpper(), "section", header));
    auto* line = new QFrame(header);
    line->setProperty("role", "separator");
    layout->addWidget(line);
    return header;
}

QIcon colorSwatch(const QColor& color, QSize size) {
    const qreal ratio = 2.0;
    QPixmap pixmap(size * ratio);
    pixmap.setDevicePixelRatio(ratio);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(theme::palette().borderStrong, 1.0));
    painter.setBrush(color);
    painter.drawRoundedRect(QRectF(0.5, 0.5, size.width() - 1.0, size.height() - 1.0), 4.0, 4.0);
    painter.end();
    QIcon icon(pixmap);
    icon.addPixmap(pixmap, QIcon::Selected); // keep the true color on selected rows
    return icon;
}

QString count(int n, const QString& one, const QString& many) {
    return n == 1 ? one : many.arg(n);
}

QColor displayColor(const glm::vec3& linear) {
    const glm::vec3 srgb = linearToSrgb(linear);
    return QColor::fromRgbF(srgb.r, srgb.g, srgb.b);
}

glm::vec3 linearColor(const QColor& display) {
    return srgbToLinear(glm::vec3(static_cast<float>(display.redF()),
                                  static_cast<float>(display.greenF()),
                                  static_cast<float>(display.blueF())));
}

} // namespace lb::app::ui
