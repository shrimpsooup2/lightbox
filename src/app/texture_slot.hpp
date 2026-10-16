#pragma once

#include <cstdint>
#include <deque>
#include <unordered_map>
#include <unordered_set>

#include <QImage>
#include <QListWidget>
#include <QObject>
#include <QPixmap>
#include <QString>
#include <QWidget>

#include "core/scene/scene.hpp"

class QCheckBox;
class QComboBox;
class QLabel;
class QMimeData;
class QToolButton;

// Texture UI used by the Materials panel: a shared thumbnail cache, the project texture grid, the
// preview dialog and the per-map texture slot editor.
namespace lb::app {

// Drag payload for a project texture (its ResourceId as decimal text); texture slots accept it.
inline constexpr const char* kTextureMimeType = "application/x-lightbox-texture";

// Square thumbnails of the scene's textures, built on the UI thread from TextureResource::image and
// dropped when the texture changes or is removed. Building is time-sliced (thumbnailLater), so lists
// of hundreds of 4K textures never stall the UI. Must outlive the widgets it is passed to.
class TextureThumbnails : public QObject {
    Q_OBJECT

public:
    static constexpr int kSize = 128; // pixels; views scale down smoothly

    explicit TextureThumbnails(Scene& scene, QObject* parent = nullptr);
    ~TextureThumbnails() override;

    // Cached thumbnail, built now if needed (fine for one texture). Null pixmap for unknown IDs.
    QPixmap thumbnail(ResourceId texture);
    // Cached thumbnail; otherwise a null pixmap now and thumbnailReady(texture) once it has been
    // built in a later event-loop turn.
    QPixmap thumbnailLater(ResourceId texture);

signals:
    void thumbnailReady(quint64 texture); // ResourceId::value

private:
    void buildQueued();

    Scene& scene_;
    Scene::ListenerId listener_ = 0;
    std::unordered_map<ResourceId, QPixmap> cache_;
    std::deque<ResourceId> queue_;
    std::unordered_set<ResourceId> queued_;
    bool scheduled_ = false;
};

// Icon showing `pixmap` unchanged in every state (item views otherwise tint selected icons).
QIcon thumbnailIcon(const QPixmap& pixmap);

// The image for display, scaled so its longer side is at most `maxSide` (never enlarged). Large
// images get a nearest-neighbor pre-pass so this stays fast on 4K textures.
QImage textureImage(const ImageData& image, int maxSide);

// Number of materials using each texture, in one pass over the materials.
std::unordered_map<ResourceId, int> textureUsage(const Scene& scene);

// Non-modal preview of a texture: the image, its name, pixel size, usage and source file.
void showTexturePreview(const Scene& scene, ResourceId texture, QWidget* parent);

// Icon grid of the project's textures sorted by name. Items carry their texture ID and can be dragged
// onto texture slots. Its height follows the content up to `maxRows` rows, then it scrolls.
class TextureGrid : public QListWidget {
    Q_OBJECT

public:
    enum class Detail {
        Name, // compact picker: name only
        Full, // name, pixel size and the number of materials using it
    };

    TextureGrid(TextureThumbnails& thumbnails, Detail detail, int maxRows, QWidget* parent);

    void rebuild(const Scene& scene);
    // Refreshes the usage lines (Detail::Full) without rebuilding the items.
    void updateUsage(const Scene& scene);
    // Makes the texture's item current (none = clear). Emits no click signals.
    void selectTexture(ResourceId texture);

    static ResourceId idOf(const QListWidgetItem* item);

protected:
    QStringList mimeTypes() const override;
    QMimeData* mimeData(const QList<QListWidgetItem*>& items) const override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void describe(QListWidgetItem* item, const TextureResource& texture, int users) const;
    void fitHeight();

    TextureThumbnails& thumbnails_;
    Detail detail_;
    int maxRows_;
    std::unordered_map<ResourceId, QListWidgetItem*> items_;
};

// Editor for one texture input of a material: thumbnail button (menu: choose image, use a project
// texture, show, clear), name and pixel size, a clear button and, for scalar maps, the channel to read
// and an invert switch. Image files and project textures can be dropped on it. Image files are decoded
// in the background, capped to import::kMaxTextureSize and added to the scene as new textures.
class TextureSlotWidget : public QWidget {
    Q_OBJECT

public:
    enum class Kind {
        Color,  // all channels (color and normal maps)
        Scalar, // one channel, optionally inverted (roughness, metalness, occlusion)
    };

    // `title` names the map in dialogs and tooltips ("Roughness map").
    TextureSlotWidget(Scene& scene, TextureThumbnails& thumbnails, Kind kind, const QString& title,
                      QWidget* parent);

    const lb::TextureSlot& slot() const { return slot_; }
    // Shows `slot`, re-reading the texture's name and thumbnail. Does not emit slotEdited.
    void setSlot(const lb::TextureSlot& slot);
    // Drops an image still loading in the background (e.g. another material is shown now).
    void cancelLoading();

signals:
    // The user picked, dropped, loaded or cleared a texture, or changed the channel or invert.
    void slotEdited(lb::TextureSlot slot);

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void showMenu();
    void chooseFile();
    void loadFile(const QString& path);
    void assign(ResourceId texture);
    void refresh();
    void updateName();
    void setDropHighlight(bool on);

    Scene& scene_;
    TextureThumbnails& thumbnails_;
    Kind kind_;
    QString title_;
    lb::TextureSlot slot_;
    QString fullName_;
    QString loadingName_; // file being decoded, empty when idle
    std::uint64_t loadGeneration_ = 0;

    QToolButton* thumb_ = nullptr;
    QLabel* name_ = nullptr;
    QLabel* info_ = nullptr;
    QComboBox* channel_ = nullptr;
    QCheckBox* invert_ = nullptr;
    QToolButton* clear_ = nullptr;
};

} // namespace lb::app
