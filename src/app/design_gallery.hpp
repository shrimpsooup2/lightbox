#pragma once

#include <QWidget>

class QPushButton;

namespace lb::app {

// Developer window (`LightBox --gallery`) showing every styled control, color token and icon, with a
// live dark/light switch. Use it to review design-system changes; screenshots via `--screenshot`.
class DesignGallery : public QWidget {
    Q_OBJECT

public:
    explicit DesignGallery(QWidget* parent = nullptr);

    // Saves the whole gallery page (not just the scrolled viewport) as a PNG.
    bool saveScreenshot(const QString& path);

private:
    void updateThemeButton();

    QWidget* page_ = nullptr;
    QPushButton* themeButton_ = nullptr;
};

} // namespace lb::app
