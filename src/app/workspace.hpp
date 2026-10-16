#pragma once

#include <optional>
#include <span>

#include <QKeySequence>
#include <QString>

namespace lb::app {

// The six workflow modes (docs/WORKFLOW.md). Order = mode bar order.
enum class WorkspaceMode { Import, Build, Materials, Site, Light, Render };

struct WorkspaceModeInfo {
    WorkspaceMode mode;
    const char* key;  // stable settings / command-line key, e.g. "materials"
    const char* icon; // Lucide icon name
    QString title;
    QString summary; // one plain sentence: what you do in this mode
    QKeySequence shortcut;
};

std::span<const WorkspaceModeInfo> workspaceModes();
const WorkspaceModeInfo& workspaceModeInfo(WorkspaceMode mode);
std::optional<WorkspaceMode> workspaceModeFromKey(const QString& key);

} // namespace lb::app
