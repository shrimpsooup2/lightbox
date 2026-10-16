#include "app/coming_soon.hpp"

#include <QGridLayout>
#include <QLabel>
#include <QVBoxLayout>
#include <QWidget>

#include "app/theme.hpp"
#include "app/ui_helpers.hpp"

namespace lb::app {

QWidget* makeComingSoonPage(const QString& icon, const QString& title, const QString& text,
                            const QStringList& bullets, QWidget* parent) {
    auto* page = new QWidget(parent);
    page->setProperty("panel", true);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(20, 28, 20, 20);
    layout->setSpacing(10);

    auto* image = new QLabel(page);
    image->setPixmap(theme::icon(icon).pixmap(QSize(36, 36), QIcon::Normal));
    layout->addWidget(image, 0, Qt::AlignLeft);
    layout->addWidget(ui::label(title, "heading", page));
    auto* body = ui::label(text, "muted", page);
    body->setWordWrap(true);
    layout->addWidget(body);
    if (!bullets.isEmpty()) {
        auto* list = new QGridLayout;
        list->setHorizontalSpacing(8);
        list->setVerticalSpacing(6);
        int row = 0;
        for (const QString& bullet : bullets) {
            list->addWidget(ui::label(QStringLiteral("•"), "muted", page), row, 0, Qt::AlignTop);
            auto* item = ui::label(bullet, nullptr, page);
            item->setWordWrap(true);
            list->addWidget(item, row, 1);
            ++row;
        }
        list->setColumnStretch(1, 1);
        layout->addLayout(list);
    }
    layout->addStretch(1);
    return page;
}

} // namespace lb::app
