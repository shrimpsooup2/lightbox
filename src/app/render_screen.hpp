#pragma once

#include <memory>

#include <QElapsedTimer>
#include <QWidget>

#include "render/renderer.hpp"

class QCheckBox;
class QComboBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QTimer;

namespace lb::app {

class ViewportWindow;
namespace ui {
class SegmentedControl;
}

// The render screen (ROADMAP 3.12, D-029): final images are made here, as in Lumion, instead of in
// the editing viewport. Pick a size, quality and speed, press Render and watch the image fill in;
// save it when it is done (or at any point). The path tracer renders the current view offscreen at
// the chosen size while the editing viewport rests, so covering or resizing the window never slows
// or changes the result.
class RenderScreen : public QWidget {
    Q_OBJECT

public:
    RenderScreen(render::Renderer& renderer, ViewportWindow& viewport, QWidget* parent = nullptr);
    ~RenderScreen() override;

    // When the screen opens: the project name is the start of saved file names.
    void prepare(const QString& projectName);
    // Stops a render in progress (keeping the image so far) and frees the GPU memory.
    void stop();

signals:
    // Back to editing (button or Esc).
    void closeRequested();

protected:
    void keyPressEvent(QKeyEvent* event) override;

private:
    class Preview;

    void start();
    void step();
    // Stops the render loop with a status line; `capture` reads the image once more first.
    void finish(const QString& message, bool capture);
    // Saves the image with the chosen DPI written into the file (for printing).
    void saveImage();
    QSize outputSize() const;
    // The editing view's size in pixels (the "Same as the view" preset).
    QSize viewportSize() const;
    // A size preset was chosen: fills in width and height (Custom keeps them).
    void applyPreset(int index);
    void setOutputSize(QSize size);
    // Width, height or preset changed: remember it, and restart a render in progress.
    void sizeChanged();
    render::FrameInput frameInput();
    void updateControls();

    render::Renderer& renderer_;
    ViewportWindow& viewport_;
    std::unique_ptr<render::Presenter> presenter_; // offscreen, only while rendering
    QTimer* timer_ = nullptr;
    QElapsedTimer sincePreview_;
    QString projectName_;
    bool rendering_ = false;

    Preview* preview_ = nullptr;
    QComboBox* sizePreset_ = nullptr;
    QSpinBox* width_ = nullptr;
    QSpinBox* height_ = nullptr;
    QCheckBox* keepProportions_ = nullptr;
    double aspect_ = 16.0 / 9.0; // kept while "Keep proportions" is on
    bool updatingSize_ = false;
    QSpinBox* dpi_ = nullptr;
    QLabel* sizeNote_ = nullptr; // megapixels and print size
    ui::SegmentedControl* quality_ = nullptr;
    ui::SegmentedControl* speed_ = nullptr;
    QPushButton* renderButton_ = nullptr;
    QProgressBar* progress_ = nullptr;
    QLabel* status_ = nullptr;
    QPushButton* saveButton_ = nullptr;
};

} // namespace lb::app
