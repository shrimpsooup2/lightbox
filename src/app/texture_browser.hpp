#pragma once

#include <filesystem>
#include <memory>
#include <vector>

#include <QDialog>
#include <QHash>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QUrl>

#include "core/scene/scene.hpp"
#include "core/scene/selection.hpp"

class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QNetworkAccessManager;
class QNetworkReply;
class QProgressBar;
class QPushButton;

namespace lb::app {

namespace ui {
class SegmentedControl;
}

// Online texture library: searches ambientCG and Poly Haven (both CC0), downloads a set at the chosen
// resolution into the local library (%LOCALAPPDATA%/LightBox/Library/Textures), turns it into a
// material at its real-world size and applies it to the selected objects. Sets downloaded before are
// reused without the network. Poly Haven's API terms ask that the source is named in the UI; both
// sources are, with a CC0 credit line.
class TextureBrowser : public QDialog {
    Q_OBJECT

public:
    TextureBrowser(Scene& scene, Selection& selection, QWidget* parent = nullptr);
    ~TextureBrowser() override;

signals:
    // A material was created (and applied to the selection, if any).
    void materialAdded(lb::ResourceId material, const QString& message);

private:
    enum class Source { AmbientCg, PolyHaven };

    struct Asset {
        QString id;
        QString name;
        QString category;
        QStringList keywords;     // tags and categories, lower case (Poly Haven local search)
        double widthMeters = 0.0; // 0 = unknown
        double heightMeters = 0.0;
        QUrl thumbnail;
        QHash<QString, QUrl> zips; // ambientCG: "2K-JPG" → download link
        int popularity = 0;
    };

    Source source() const;
    QString resolution() const; // "1K", "2K", "4K"
    void search();
    void searchAmbientCg(const QString& text);
    void searchPolyHaven(const QString& text);
    void showResults(std::vector<Asset> assets, int total);
    void loadThumbnail(int row);
    void updateActions();
    void setStatus(const QString& text, bool error = false);
    void setBusy(bool busy);
    void rebuildGrid(); // from results_ (e.g. after a download or a resolution change)

    void useSelected();
    void downloadAmbientCg(const Asset& asset);
    void downloadPolyHaven(const Asset& asset);
    void downloadNext();
    void cancelDownload();
    void createMaterial(const Asset& asset, const std::filesystem::path& source);

    QString libraryFolder() const; // per source
    QString downloadedPath(const Asset& asset) const; // zip (ambientCG) or folder (Poly Haven)
    void writeAssetInfo(const Asset& asset) const;
    QNetworkReply* get(const QUrl& url, bool preferCache = false);

    Scene& scene_;
    Selection& selection_;
    Selection::ListenerId selectionListener_ = 0;
    QNetworkAccessManager* network_ = nullptr;

    ui::SegmentedControl* sourceControl_ = nullptr;
    QLineEdit* search_ = nullptr;
    QComboBox* resolution_ = nullptr;
    QListWidget* grid_ = nullptr;
    QLabel* credit_ = nullptr;
    QLabel* status_ = nullptr;
    QProgressBar* progress_ = nullptr;
    QPushButton* cancel_ = nullptr;
    QPushButton* use_ = nullptr;

    std::vector<Asset> results_;
    std::vector<Asset> polyHavenIndex_; // all Poly Haven textures, loaded once
    QPointer<QNetworkReply> searchReply_;
    std::uint64_t searchSerial_ = 0;
    int total_ = 0; // matches before truncation to the shown count

    // Download in progress: the files still to fetch, then the material is created.
    struct Download {
        Asset asset;
        Source source = Source::AmbientCg;
        std::vector<std::pair<QUrl, QString>> files; // url → target file (in a ".part" location)
        std::size_t next = 0;
        QString finalPath;   // zip file or folder once complete
        QString partialPath; // where files are written meanwhile
    };
    std::unique_ptr<Download> download_;
    QPointer<QNetworkReply> downloadReply_;
    bool creating_ = false;
};

} // namespace lb::app
