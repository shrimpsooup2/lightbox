#include "app/workspace.hpp"

#include <array>

#include <QCoreApplication>

namespace lb::app {
namespace {

QString tr(const char* text) {
    return QCoreApplication::translate("Workspace", text);
}

const std::array<WorkspaceModeInfo, 6>& modes() {
    static const std::array<WorkspaceModeInfo, 6> table = {{
        {WorkspaceMode::Import, "import", "download", tr("Import"),
         tr("Bring in your models and plans"), QKeySequence(Qt::CTRL | Qt::Key_1)},
        {WorkspaceMode::Build, "build", "blocks", tr("Build"),
         tr("Arrange the scene and add furniture, people and props"),
         QKeySequence(Qt::CTRL | Qt::Key_2)},
        {WorkspaceMode::Materials, "materials", "palette", tr("Materials"),
         tr("Choose how surfaces look"), QKeySequence(Qt::CTRL | Qt::Key_3)},
        {WorkspaceMode::Site, "site", "trees", tr("Site"),
         tr("Shape the ground, plant and add water"), QKeySequence(Qt::CTRL | Qt::Key_4)},
        {WorkspaceMode::Light, "light", "sun", tr("Light"),
         tr("Set the sun and the mood"), QKeySequence(Qt::CTRL | Qt::Key_5)},
        {WorkspaceMode::Render, "render", "camera", tr("Render"),
         tr("Save images of your design"), QKeySequence(Qt::CTRL | Qt::Key_6)},
    }};
    return table;
}

} // namespace

std::span<const WorkspaceModeInfo> workspaceModes() {
    return modes();
}

const WorkspaceModeInfo& workspaceModeInfo(WorkspaceMode mode) {
    return modes()[static_cast<std::size_t>(mode)];
}

std::optional<WorkspaceMode> workspaceModeFromKey(const QString& key) {
    for (const WorkspaceModeInfo& info : modes()) {
        if (key == QLatin1String(info.key)) {
            return info.mode;
        }
    }
    return std::nullopt;
}

} // namespace lb::app
