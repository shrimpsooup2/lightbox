#pragma once

#include <QString>
#include <QStringList>

class QWidget;

namespace lb::app {

// Calm empty state for a mode whose tools are not built yet: icon, title, one sentence and at most a
// few short bullets. Roadmap detail stays in docs/ROADMAP.md, not in the UI. Replace the page with the
// real panel when the feature lands.
QWidget* makeComingSoonPage(const QString& icon, const QString& title, const QString& text,
                            const QStringList& bullets, QWidget* parent = nullptr);

} // namespace lb::app
