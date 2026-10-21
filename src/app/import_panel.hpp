#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <memory>
#include <optional>
#include <vector>

#include <QElapsedTimer>
#include <QList>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QWidget>

#include "core/mesh.hpp"
#include "core/scene/scene.hpp"
#include "core/scene/selection.hpp"
#include "import/importer.hpp"

class QComboBox;
class QLabel;
class QListWidget;
class QProgressBar;
class QPushButton;

namespace lb::app {

class ViewportWindow;

namespace ui {
class Section;
class SegmentedControl;
} // namespace ui

// Import mode panel: brings models (glTF, OBJ, FBX) and PBR texture sets into the scene. Files are
// parsed on a worker thread one after another, with progress and cancel; finished models are
// listed for this session with placement controls (units, up axis, frame, place at origin, remove)
// and an import report. `scene`, `selection` and `viewport` must outlive the panel.
class ImportPanel : public QWidget {
    Q_OBJECT

public:
    ImportPanel(Scene& scene, Selection& selection, ViewportWindow& viewport,
                QWidget* parent = nullptr);
    ~ImportPanel() override;

    // Local model or image files among `urls` (what a drop would import), as native paths.
    static QStringList importablePaths(const QList<QUrl>& urls);
    static bool isImportable(const QString& path);
    static bool isModelPath(const QString& path);

    // Queues the model files of `paths` one after another and turns all images of `paths` into one
    // material. Unsupported files are skipped with a status message. With `switchMode`, a finished
    // texture material raises materialImported() so the window can show Materials mode.
    void importFiles(const QStringList& paths, bool switchMode = true);
    // File dialogs behind the panel buttons and the File menu.
    void chooseModels();
    void chooseTextureSet();
    bool busy() const { return current_.has_value(); }

signals:
    // One-line outcome for the status bar ("Imported House: 12 objects, 4 materials in 1.2 s").
    void statusMessage(const QString& text);
    // Work started/finished (for a status bar indicator); percent is 0..100 while busy.
    void busyChanged(bool busy);
    void progressChanged(int percent);
    // A texture set became a material (and was applied to the selection, if any).
    void materialImported();

private:
    enum class JobKind { Model, TextureSet };
    struct Job {
        JobKind kind = JobKind::Model;
        QStringList paths; // one model, or every image of a texture set
        bool switchMode = true;
        std::optional<glm::vec2> dropPoint; // models: where on the ground they were dropped
    };
    // A model imported this session. Resource lists hold what the import created, so Remove can
    // delete exactly that (minus anything the user has since reused elsewhere).
    struct Record {
        EntityId root;
        QString name;
        QString format;
        QString path;
        std::size_t objects = 0;
        std::size_t triangles = 0;
        std::size_t materialCount = 0;
        std::size_t textureCount = 0;
        double seconds = 0.0;
        double fileMetersPerUnit = 1.0;
        double metersPerUnit = 1.0;
        bool unitsFromFile = false;
        bool yUp = false; // a 90° turn about X was added for files exported Y-up as Z-up
        QStringList warnings;
        std::vector<ResourceId> meshes;
        std::vector<ResourceId> materials;
        std::vector<ResourceId> textures;
    };

    void startNext();
    void finishJob();
    void onProgress(std::uint64_t job, float fraction, const QString& stage);
    void cancel();
    void addModel(const Job& job, import::ImportedModel model);
    void addTextureSet(const Job& job, const import::ImportedModel& model);
    void showFailure(const Job& job, std::exception_ptr error);

    Record* selectedRecord();
    void refreshList();
    void refreshDetails();
    void setUnits(double metersPerUnit);
    void setYUp(bool yUp);
    void frameSelected();
    void placeAtOrigin();
    void removeSelected();

    Scene& scene_;
    Selection& selection_;
    ViewportWindow& viewport_;
    Scene::ListenerId listener_ = 0;
    bool updating_ = false;

    std::deque<Job> queue_;
    std::optional<Job> current_;
    std::uint64_t jobSerial_ = 0; // identifies the running job; stale progress is ignored
    std::shared_ptr<std::atomic<bool>> cancel_;
    QElapsedTimer jobTimer_;
    std::vector<Record> records_;

    QWidget* progressRow_ = nullptr;
    QLabel* progressStage_ = nullptr;
    QProgressBar* progressBar_ = nullptr;
    QPushButton* cancelButton_ = nullptr;

    ui::Section* modelsSection_ = nullptr;
    QListWidget* modelList_ = nullptr;
    QLabel* emptyHint_ = nullptr;

    ui::Section* placementSection_ = nullptr;
    QComboBox* units_ = nullptr;
    ui::SegmentedControl* upAxis_ = nullptr;

    ui::Section* reportSection_ = nullptr;
    QLabel* reportFormat_ = nullptr;
    QLabel* reportFile_ = nullptr;
    QLabel* reportStats_ = nullptr;
    QLabel* reportTime_ = nullptr;
    QLabel* reportUnits_ = nullptr;
    QLabel* reportWarnings_ = nullptr;
};

} // namespace lb::app
