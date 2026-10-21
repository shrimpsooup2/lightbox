#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <unordered_map>
#include <unordered_set>

#include <QPointer>
#include <QWidget>

#include "app/property_widgets.hpp"
#include "core/scene/scene.hpp"
#include "core/scene/selection.hpp"

class QCheckBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace lb::app {

class TextureGrid;
class TextureSlotWidget;
class TextureThumbnails;
class TextureBrowser;

// Materials mode panel (D-019 style): actions (new, from a texture set, apply to the selection,
// select users, delete), every material with a thumbnail and its usage, the selected material's
// settings (color, surface, relief, occlusion and their texture maps, mapping) and the project's
// textures. Follows the selection: selecting an object shows its material. Refreshes from scene
// notifications, coalesced per event-loop turn and only for what changed, so it stays responsive
// with hundreds of materials and textures.
class MaterialsPanel : public QWidget {
    Q_OBJECT

public:
    MaterialsPanel(Scene& scene, Selection& selection, QWidget* parent = nullptr);
    ~MaterialsPanel() override;

protected:
    void showEvent(QShowEvent* event) override;

private:
    // What a pending refresh has to redo.
    enum Dirty : unsigned {
        kMaterialList = 1u << 0,  // materials added/removed/renamed: rebuild the list
        kMaterialItems = 1u << 1, // materials in changedMaterials_ changed: update their rows
        kUsage = 1u << 2,         // objects changed: recount material users
        kTextureGrid = 1u << 3,   // textures added/removed/renamed: rebuild the grid
        kTextureUsage = 1u << 4,  // material maps may have changed: recount texture users
        kEditor = 1u << 5,        // the shown material changed
    };

    void onSceneChange(const SceneChange& change);
    void markDirty(unsigned flags);
    void flush();

    void rebuildMaterialList();
    void updateMaterialItem(QTreeWidgetItem* item, const MaterialResource& resource);
    void updateMaterialUsage();
    void fitMaterialList();
    void selectCurrentItem();
    void rebuildTextures();
    void refreshTextureUsage();
    void updateTextureActions();

    void showMaterial(ResourceId id);
    void showMapping(const Material& material);
    void followSelection();
    void updateActions();

    void editMaterial(const std::function<void(Material&)>& change);
    void editSlot(TextureSlot Material::*map, const TextureSlot& slot);
    void editScale(int axis, double value);
    void applyName();
    void pickColor();
    void applyPreset(int index);
    void selectUsers();
    void applyToSelection();
    void createMaterial();
    void createFromTextures();
    void openTextureLibrary();
    void setImporting(bool importing);
    void deleteMaterial();
    void removeUnusedTextures();

    Scene& scene_;
    Selection& selection_;
    Scene::ListenerId sceneListener_ = 0;
    Selection::ListenerId selectionListener_ = 0;
    ResourceId current_; // material chosen in the list
    ResourceId shown_;   // material the editor shows (slot loads are tied to it)
    bool updating_ = false;
    unsigned dirty_ = 0;
    bool flushScheduled_ = false;
    std::unordered_set<ResourceId> changedMaterials_;
    std::unordered_map<ResourceId, QTreeWidgetItem*> materialItems_;
    std::unordered_map<ResourceId, int> materialUsage_; // objects per material
    std::unordered_map<ResourceId, int> textureUsage_;  // materials per texture
    std::unordered_set<ResourceId> textureIds_;
    std::shared_ptr<std::atomic<bool>> cancelImport_; // set while a texture set is loading

    TextureThumbnails* thumbnails_ = nullptr;

    QPushButton* library_ = nullptr;
    QPointer<TextureBrowser> browser_;
    QPushButton* create_ = nullptr;
    QPushButton* fromTextures_ = nullptr;
    QPushButton* apply_ = nullptr;
    QPushButton* selectUsers_ = nullptr;
    QPushButton* delete_ = nullptr;
    QProgressBar* progress_ = nullptr;
    QLabel* notice_ = nullptr;
    QTreeWidget* list_ = nullptr;
    QLabel* noMaterial_ = nullptr;

    ui::Section* base_ = nullptr;
    QLineEdit* name_ = nullptr;
    QToolButton* swatch_ = nullptr;
    TextureSlotWidget* baseColorMap_ = nullptr;

    ui::Section* surface_ = nullptr;
    ui::SliderField* roughness_ = nullptr;
    TextureSlotWidget* roughnessMap_ = nullptr;
    ui::SliderField* metalness_ = nullptr;
    TextureSlotWidget* metalnessMap_ = nullptr;

    ui::Section* relief_ = nullptr;
    TextureSlotWidget* normalMap_ = nullptr;
    ui::SliderField* normalStrength_ = nullptr;
    QCheckBox* normalFlipY_ = nullptr;

    ui::Section* occlusion_ = nullptr;
    TextureSlotWidget* occlusionMap_ = nullptr;
    TextureSlotWidget* heightMap_ = nullptr; // in the Relief section
    ui::SliderField* heightDepth_ = nullptr;

    ui::Section* mapping_ = nullptr;
    ui::SegmentedControl* mappingMode_ = nullptr;
    QDoubleSpinBox* scaleX_ = nullptr;
    QDoubleSpinBox* scaleY_ = nullptr;
    QToolButton* lockAspect_ = nullptr;
    QLabel* scaleLabel_ = nullptr;
    ui::PropertyRow quickSizes_; // one-click tile sizes (box mapping)
    ui::SliderField* rotation_ = nullptr;
    ui::SliderField* variation_ = nullptr;
    ui::Section* glass_ = nullptr;
    ui::SliderField* transmission_ = nullptr;
    ui::SliderField* ior_ = nullptr;
    QCheckBox* thinWalled_ = nullptr;
    ui::SliderField* emissionStrength_ = nullptr;
    QToolButton* glowSwatch_ = nullptr;
    QCheckBox* randomTiles_ = nullptr;
    QDoubleSpinBox* offsetX_ = nullptr;
    QDoubleSpinBox* offsetY_ = nullptr;

    TextureGrid* textures_ = nullptr;
    QLabel* textureCount_ = nullptr;
    QPushButton* removeUnused_ = nullptr;
};

} // namespace lb::app
