#include "contentmanager.h"

#include "kiwixapp.h"
#include <kiwix/manager.h>
#include <kiwix/tools.h>

#include <QDebug>
#include <QUrlQuery>
#include <QUrl>
#include <QDir>
#include <QMessageBox>
#include "contentmanagermodel.h"
#include <zim/error.h>
#include <zim/item.h>
#include <QHeaderView>
#include "contentmanagerdelegate.h"
#include "node.h"
#include "rownode.h"
#include "descriptionnode.h"
#include "kiwixmessagebox.h"
#include <QtConcurrent/QtConcurrentRun>
#include "contentmanagerheader.h"
#include <QDesktopServices>

#ifndef QT_NO_DEBUG
#define DBGOUT(X) qDebug().nospace() << "DBG: " << X
#else
#define DBGOUT(X)
#endif

namespace
{


SettingsManager* getSettingsManager()
{
    return KiwixApp::instance()->getSettingsManager();
}

// Opens the directory containing the input file path.
// parent is the widget serving as the parent for the error dialog in case of
// failure.
void openFileLocation(QString path, QWidget *parent = nullptr)
{
    QFileInfo fileInfo(path);
    QDir dir = fileInfo.absoluteDir();
    bool dirOpen = dir.exists() && dir.isReadable() && QDesktopServices::openUrl(dir.absolutePath());
    if (!dirOpen) {
        QString failedText = gt("couldnt-open-location-text");
        failedText = failedText.replace("{{FOLDER}}", "<b>" + dir.absolutePath() + "</b>");
        showInfoBox(gt("couldnt-open-location"), failedText, parent);
    }
}

} // unnamed namespace

ContentManager::ContentManager(Library* library)
    : DownloadManager(library),
      mp_library(library),
      mp_remoteLibrary(kiwix::Library::create()),
      m_remoteLibraryManager()
{
    // mp_view will be passed to the tab who will take ownership,
    // so, we don't need to delete it.
    mp_view = new ContentManagerView();
    managerModel = new ContentManagerModel(this);
    updateModel();
    auto treeView = mp_view->getView();
    treeView->setModel(managerModel);
    treeView->show();

    auto header = new ContentManagerHeader(Qt::Orientation::Horizontal, treeView);
    treeView->setHeader(header);
    header->setSectionResizeMode(0, QHeaderView::Fixed);
    header->setSectionResizeMode(1, QHeaderView::Stretch);
    header->setSectionResizeMode(2, QHeaderView::Fixed);
    header->setSectionResizeMode(3, QHeaderView::Fixed);
    header->setSectionResizeMode(4, QHeaderView::Fixed);
    header->setDefaultAlignment(Qt::AlignLeft);
    header->setStretchLastSection(false);
    header->setSectionsClickable(true);
    header->setHighlightSections(true);
    treeView->setWordWrap(true);
    treeView->resizeColumnToContents(4);
    treeView->setColumnWidth(0, 70);
    treeView->setColumnWidth(5, 120);
    // TODO: set width for all columns based on viewport
    treeView->setAllColumnsShowFocus(true);

    setCurrentLanguage(getSettingsManager()->getLanguageList());
    setCurrentCategoryFilter(getSettingsManager()->getCategoryList());
    setCurrentContentTypeFilter(getSettingsManager()->getContentType());
    connect(mp_library, &Library::booksChanged, this, [=]() {emit(this->booksChanged());});
    connect(this, &ContentManager::filterParamsChanged, this, &ContentManager::updateLibrary);
    connect(this, &ContentManager::booksChanged, this, [=]() {
        updateModel();
        setCategories();
        setLanguages();
    });
    connect(&m_remoteLibraryManager, &OpdsRequestManager::requestReceived, this, &ContentManager::updateRemoteLibrary);
    connect(mp_view->getView(), SIGNAL(customContextMenuRequested(const QPoint &)), this, SLOT(onCustomContextMenu(const QPoint &)));
    connect(this, &ContentManager::pendingRequest, mp_view, &ContentManagerView::showLoader);
    connect(treeView, &QTreeView::doubleClicked, this, &ContentManager::openBookWithIndex);
    connect(&m_remoteLibraryManager, &OpdsRequestManager::languagesReceived, this, &ContentManager::updateLanguages);
    connect(&m_remoteLibraryManager, &OpdsRequestManager::categoriesReceived, this, &ContentManager::updateCategories);
    setCategories();
    setLanguages();

    connect(this, &DownloadManager::downloadUpdated,
            this, &ContentManager::updateDownload);

    connect(this, &DownloadManager::downloadCancelled,
            this, &ContentManager::downloadWasCancelled);

    connect(this, &DownloadManager::downloadDisappeared,
            this, &ContentManager::downloadDisappeared);

    connect(this, &DownloadManager::error, this, &ContentManager::handleError);

    if ( DownloadManager::downloadingFunctionalityAvailable() ) {
        startDownloadUpdaterThread();
    }

    connect(&m_watcher, &QFileSystemWatcher::directoryChanged,
            this, &ContentManager::asyncUpdateLibraryFromDir);
}

void ContentManager::updateModel()
{
    const auto bookIds = getBookIds();
    BookInfoList bookList;
    QStringList keys = {"title", "tags", "date", "id", "size", "description", "favicon"};
    for (auto bookId : bookIds) {
        auto mp = getBookInfos(bookId, keys);
        bookList.append(mp);
    }

    const DownloadManager& downloadMgr = *this;
    managerModel->setBooksData(bookList, downloadMgr);
}

void ContentManager::onCustomContextMenu(const QPoint &point)
{
    QModelIndex index = mp_view->getView()->indexAt(point);
    if (!index.isValid())
        return;
    QMenu contextMenu("optionsMenu", mp_view->getView());
    auto bookNode = static_cast<RowNode*>(index.internalPointer());
    const auto id = bookNode->getBookId();

    QAction menuDeleteBook(gt("delete-book"), this);
    QAction menuOpenBook(gt("open-book"), this);
    QAction menuDownloadBook(gt("download-book"), this);
    QAction menuPauseBook(gt("pause-download"), this);
    QAction menuResumeBook(gt("resume-download"), this);
    QAction menuCancelBook(gt("cancel-download"), this);
    QAction menuOpenFolder(gt("open-folder"), this);
    QAction menuPreviewBook(gt("preview-book-in-web-browser"), this);

    const auto bookState = getBookState(id);
    switch ( bookState ) {
    case BookState::DOWNLOAD_PAUSED:
        if ( getDownloadState(id)->getStatus() == DownloadState::PAUSED ) {
            contextMenu.addAction(&menuResumeBook);
            contextMenu.addAction(&menuCancelBook);
        }
        contextMenu.addAction(&menuPreviewBook);
        break;

    case BookState::DOWNLOADING:
        if ( getDownloadState(id)->getStatus() == DownloadState::DOWNLOADING ) {
            contextMenu.addAction(&menuPauseBook);
            contextMenu.addAction(&menuCancelBook);
        }
        contextMenu.addAction(&menuPreviewBook);
        break;

    case BookState::AVAILABLE_LOCALLY_AND_HEALTHY:
    case BookState::ERROR_MISSING_ZIM_FILE:
    case BookState::ERROR_CORRUPTED_ZIM_FILE:
        {
            const auto book = mp_library->getBookById(id);
            auto bookPath = QString::fromStdString(book.getPath());
            if ( bookState == BookState::AVAILABLE_LOCALLY_AND_HEALTHY ) {
                contextMenu.addAction(&menuOpenBook);
            }
            contextMenu.addAction(&menuDeleteBook);
            contextMenu.addAction(&menuOpenFolder);
            connect(&menuOpenFolder, &QAction::triggered, [=]() {
                openFileLocation(bookPath, mp_view);
            });
            break;
        }

    case BookState::AVAILABLE_ONLINE:
        contextMenu.addAction(&menuDownloadBook);
        contextMenu.addAction(&menuPreviewBook);
        break;

    default: break;
    }

    connect(&menuDeleteBook, &QAction::triggered, [=]() {
        eraseBook(id);
    });
    connect(&menuOpenBook, &QAction::triggered, [=]() {
        openBook(id);
    });
    connect(&menuDownloadBook, &QAction::triggered, [=]() {
        downloadBook(id);
    });
    connect(&menuPauseBook, &QAction::triggered, [=]() {
        pauseBook(id, index);
    });
    connect(&menuCancelBook, &QAction::triggered, [=]() {
        cancelBook(id);
    });
    connect(&menuResumeBook, &QAction::triggered, [=]() {
        resumeBook(id, index);
    });
    connect(&menuPreviewBook, &QAction::triggered, [=]() {
        openBookPreview(id);
    });

    contextMenu.exec(mp_view->getView()->viewport()->mapToGlobal(point));
}

void ContentManager::setLocal(bool local) {
    if (local == m_local) {
        return;
    }
    m_local = local;
    emit(filterParamsChanged());
    setCategories();
    setLanguages();
}

QStringList ContentManager::getTranslations(const QStringList &keys)
{
    QStringList translations;

    for(auto& key: keys) {
        translations.append(KiwixApp::instance()->getText(key));
    }
    return translations;
}

void ContentManager::setCategories()
{
    QStringList categories;
    if (m_local) {
        auto categoryData = mp_library->getKiwixLibrary()->getBooksCategories();
        for (auto category : categoryData) {
            auto categoryName = QString::fromStdString(category);
            categories.push_back(categoryName);
        }
        m_categories = categories;
        emit(categoriesLoaded(m_categories));
        return;
    }
    m_remoteLibraryManager.getCategoriesFromOpds();
}

void ContentManager::setLanguages()
{
    LanguageList languages;
    if (m_local) {
        auto languageData = mp_library->getKiwixLibrary()->getBooksLanguages();
        for (auto language : languageData) {
            auto langCode = QString::fromStdString(language);
            auto selfName = QString::fromStdString(kiwix::getLanguageSelfName(language));
            languages.push_back({selfName, langCode});
        }
        m_languages = languages;
        emit(languagesLoaded(m_languages));
        return;
    }
    m_remoteLibraryManager.getLanguagesFromOpds();
}

namespace
{

QString getBookTags(const kiwix::Book& b)
{
    QStringList tagList = QString::fromStdString(b.getTags()).split(';');
    QMap<QString, bool> displayTagMap;
    for(auto tag: tagList) {
      if (tag[0] == '_') {
        auto splitTag = tag.split(":");
        displayTagMap[splitTag[0]] = splitTag[1] == "yes" ? true:false;
      }
    }
    QStringList displayTagList;
    if (displayTagMap["_videos"]) displayTagList << QObject::tr("Videos");
    if (displayTagMap["_pictures"]) displayTagList << QObject::tr("Pictures");
    if (!displayTagMap["_details"]) displayTagList << QObject::tr("Introduction only");
    return displayTagList.join(", ");
}

QString getFaviconUrl(const kiwix::Book& b)
{
    std::string url;
    try {
        auto item = b.getIllustration(48);
        url = item->url;
    } catch (...) {
    }
    return QString::fromStdString(url);
}

QByteArray getFaviconData(const kiwix::Book& b)
{
    QByteArray qdata;
    try {
        // Try to obtain favicons only from local books (otherwise
        // kiwix::Book::Illustration::getData() attempts to download the image
        // on its own, whereas we want that operation to be performed
        // asynchronously by ThumbnailDownloader).
        if ( b.isPathValid() ) {
            const auto illustration = b.getIllustration(48);
            const std::string data = illustration->getData();

            qdata = QByteArray::fromRawData(data.data(), data.size());
            qdata.detach(); // deep copy
        }
    } catch ( ... ) {
        return QByteArray();
    }

    return qdata;
}

QVariant getFaviconDataOrUrl(const kiwix::Book& b)
{
    const QByteArray data = getFaviconData(b);
    return !data.isNull() ? QVariant(data) : QVariant(getFaviconUrl(b));
}

QVariant getBookAttribute(const kiwix::Book& b, const QString& a)
{
    if ( a == "id" )          return QString::fromStdString(b.getId());
    if ( a == "path" )        return QString::fromStdString(b.getPath());
    if ( a == "title" )       return QString::fromStdString(b.getTitle());
    if ( a == "description" ) return QString::fromStdString(b.getDescription());
    if ( a == "date" )        return QString::fromStdString(b.getDate());
    if ( a == "url" )         return QString::fromStdString(b.getUrl());
    if ( a == "name" )        return QString::fromStdString(b.getName());
    if ( a == "favicon")      return getFaviconDataOrUrl(b);
    if ( a == "size" )        return QString::number(b.getSize());
    if ( a == "tags" )        return getBookTags(b);

    return QVariant();
}

ContentManager::BookState getStateOfLocalBook(const kiwix::Book& book)
{
    if ( !book.isPathValid() ) {
        return ContentManager::BookState::ERROR_MISSING_ZIM_FILE;
    }

    // XXX: When a book is detected to be corrupted, information about that
    // XXX: has to be recorded somewhere so that we can return
    // XXX: ERROR_CORRUPTED_ZIM_FILE here

    return ContentManager::BookState::AVAILABLE_LOCALLY_AND_HEALTHY;
}

} // unnamed namespace

ContentManager::BookInfo ContentManager::getBookInfos(QString id, const QStringList &keys)
{
    const kiwix::Book* b = nullptr;
    try {
        b = &mp_library->getBookById(id);
        if ( ! b->getDownloadId().empty() ) {
            // The book is still being downloaded and has been entered into the
            // local library for technical reasons only. Get the book info from
            // the remote library.
            b = nullptr;
        }
    } catch (...) {}

    if ( !b ) {
        try {
            QMutexLocker locker(&remoteLibraryLocker);
            b = &mp_remoteLibrary->getBookById(id.toStdString());
        } catch(...) {}
    }

    BookInfo values;
    for(auto& key: keys){
        values.insert(key, b ? getBookAttribute(*b, key) : "");
    }

    return values;
}

ContentManager::BookState ContentManager::getBookState(QString bookId)
{
    if ( const auto downloadState = DownloadManager::getDownloadState(bookId) ) {
        return downloadState->getStatus() == DownloadState::PAUSED
             ? BookState::DOWNLOAD_PAUSED
             : BookState::DOWNLOADING;
             // TODO: a download may be in error state
    }

    try {
        const kiwix::Book& b = mp_library->getBookById(bookId);
        return b.getDownloadId().empty()
             ? getStateOfLocalBook(b)
             : BookState::DOWNLOADING;
    } catch (...) {}

    try {
        QMutexLocker locker(&remoteLibraryLocker);
        const kiwix::Book& b = mp_remoteLibrary->getBookById(bookId.toStdString());
        return !b.getUrl().empty()
             ? BookState::AVAILABLE_ONLINE
             : BookState::METADATA_ONLY;
    } catch (...) {}

    return BookState::INVALID;
}

void ContentManager::openBookWithIndex(const QModelIndex &index)
{
    auto bookNode = static_cast<Node*>(index.internalPointer());
    const QString bookId = bookNode->getBookId();
    if ( getBookState(bookId) == BookState::AVAILABLE_LOCALLY_AND_HEALTHY ) {
        openBook(bookId);
    }
}

void ContentManager::openBook(const QString &id)
{
    QUrl url("zim://"+id+".zim/");
    try {
        KiwixApp::instance()->openUrl(url, true);
    } catch (const std::exception& e) {
        auto tabBar = KiwixApp::instance()->getTabWidget();
        tabBar->closeTab(1);
        auto text = gt("zim-open-fail-text");
        text = text.replace("{{ZIM}}", QString::fromStdString(mp_library->getBookById(id).getPath()));
        auto title = gt("zim-open-fail-title");
        KiwixApp::instance()->showMessage(text, title, QMessageBox::Warning);
        mp_library->removeBookFromLibraryById(id);
        tabBar->setCurrentIndex(0);
        emit(booksChanged());
    }
}

void ContentManager::openBookPreview(const QString &id)
{
    try {
        QMutexLocker locker(&remoteLibraryLocker);
        const std::string &downloadUrl =
            mp_remoteLibrary->getBookById(id.toStdString()).getUrl();
        locker.unlock();

        /* Extract the Zim name from the book's download URL */
        const auto zimNameStartIndex = downloadUrl.find_last_of('/') + 1;
        const std::string& zimName = downloadUrl.substr(
            zimNameStartIndex,
            downloadUrl.find(".zim", zimNameStartIndex) - zimNameStartIndex);

        const QUrl previewUrl = getRemoteLibraryUrl() + "/viewer#" + zimName.c_str();
        QDesktopServices::openUrl(previewUrl);
    } catch (...) {}
}

void ContentManager::removeDownload(QString bookId)
{
    DownloadManager::removeDownload(bookId);
    managerModel->setDownloadState(bookId, nullptr);
}

void ContentManager::downloadDisappeared(QString bookId)
{
    removeDownload(bookId);
    kiwix::Book bCopy;
    try {
        bCopy = mp_library->getBookById(bookId);
    } catch ( const std::out_of_range& ) {
        // If the download has disappeared as a result of some
        // obscure chain of events, the book may have disappeared too.
        return;
    }

    bCopy.setDownloadId("");
    mp_library->getKiwixLibrary()->addOrUpdateBook(bCopy);
    mp_library->save();
    emit(mp_library->booksChanged());
}

void ContentManager::downloadCompleted(QString bookId, QString path)
{
    removeDownload(bookId);
    kiwix::Book bCopy(mp_library->getBookById(bookId));
    bCopy.setPath(QDir::toNativeSeparators(path).toStdString());
    bCopy.setDownloadId("");
    bCopy.setPathValid(true);
    // removing book url so that download link in kiwix-serve is not displayed.
    bCopy.setUrl("");
    mp_library->getKiwixLibrary()->addOrUpdateBook(bCopy);
    mp_library->save();
    mp_library->bookmarksChanged();
    if (!m_local) {
        emit(oneBookChanged(bookId));
    } else {
        emit(mp_library->booksChanged());
    }
}

void ContentManager::updateDownload(QString bookId, const DownloadInfo& downloadInfo)
{
    const auto downloadState = DownloadManager::getDownloadState(bookId);
    if ( downloadState ) {
        const auto downloadPath = downloadInfo["path"].toString();
        if ( downloadInfo["status"].toString() == "completed" ) {
            downloadCompleted(bookId, downloadPath);
        } else {
            mp_library->updateBookBeingDownloaded(bookId, downloadPath);
            downloadState->update(downloadInfo);
            managerModel->updateDownload(bookId);
        }
    }
}

void ContentManager::handleError(QString errSummary, QString errDetails)
{
    showErrorBox(KiwixAppError(errSummary, errDetails), mp_view);
}

void ContentManager::downloadBook(const QString &id)
{
    kiwix::Book book = getRemoteOrLocalBook(id);
    const auto downloadPath = getSettingsManager()->getDownloadDir();

    try {
        DownloadManager::checkThatBookCanBeDownloaded(book, downloadPath);
    } catch ( const KiwixAppError& err ) {
        showErrorBox(err, mp_view);
        return;
    }

    mp_library->addBookBeingDownloaded(book, downloadPath);
    mp_library->save();

    DownloadManager::addRequest(DownloadState::START, id);
    const auto downloadState = DownloadManager::getDownloadState(id);
    managerModel->setDownloadState(id, downloadState);
}

// This function is called asynchronously in a worker thread processing all
// download operations. The call is initiated in downloadBook().
void ContentManager::startDownload(QString id)
{
    kiwix::Book book = getRemoteOrLocalBook(id);
    const auto downloadPath = getSettingsManager()->getDownloadDir();
    // downloadPath may be different from the value used in
    // downloadBook(). This may happen in the following scenario:
    //
    // 1. aria2c is stuck because of having to save to
    //    slow storage (and the fact that it is a single-threaded
    //    application). This may result in startDownload() being
    //    called with significant delay after downloadBook().
    //
    // 2. The user changes the download directory after starting
    //    a download.
    //
    // That's why the checkThatBookCanBeDownloaded() check is repeated here.

    std::string downloadId;
    try {
        DownloadManager::checkThatBookCanBeDownloaded(book, downloadPath);
        downloadId = DownloadManager::startDownload(book, downloadPath);
    } catch ( const KiwixAppError& err ) {
        emit error(err.summary(), err.details());
        return;
    }

    book.setDownloadId(downloadId);
    mp_library->addBookBeingDownloaded(book, downloadPath);
    mp_library->save();
    emit(oneBookChanged(id));
}

const kiwix::Book& ContentManager::getRemoteOrLocalBook(const QString &id)
{
    try {
        QMutexLocker locker(&remoteLibraryLocker);
        return mp_remoteLibrary->getBookById(id.toStdString());
    } catch (...) {
        return mp_library->getBookById(id);
    }
}

QString ContentManager::getRemoteLibraryUrl() const
{
    auto host = m_remoteLibraryManager.getCatalogHost();
    auto port = m_remoteLibraryManager.getCatalogPort();
    return port == 443 ? "https://" + host
                        : "http://" + host + ":" + QString::number(port);
}

static const char MSG_FOR_PREVENTED_RMSTAR_OPERATION[] = R"(
    BUG: Errare humanum est.
    BUG: Kiwix developers are human, but we try to ensure that our mistakes
    BUG: don't cause harm to our users.
    BUG: If we didn't detect this situation we could have erased a lot of files
    BUG: on your computer.
)";

void ContentManager::eraseBookFilesFromComputer(const std::string& bookPath, bool moveToTrash)
{
#if QT_VERSION < QT_VERSION_CHECK(5, 15, 0)
    Q_UNUSED(moveToTrash);
#endif
    const std::string dirPath = kiwix::removeLastPathElement(bookPath);
    const std::string fileGlob = kiwix::getLastPathElement(bookPath) + "*";

    if ( fileGlob == "*" ) {
        std::cerr << MSG_FOR_PREVENTED_RMSTAR_OPERATION << std::endl;
        return;
    }

    QDir dir(QString::fromStdString(dirPath), QString::fromStdString(fileGlob));
    for(const QString& file: dir.entryList()) {
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
        if (moveToTrash)
            QFile::moveToTrash(dir.filePath(file));
        else
#endif
        dir.remove(file); // moveToTrash will always be false here, no check required.
    }
}

QString formatText(QString text)
{
    QString finalText = "<br><br><i>";
    finalText += text;
    finalText += "</i>";
    return finalText;
}

void ContentManager::reallyEraseBook(const QString& id, bool moveToTrash)
{
    auto tabBar = KiwixApp::instance()->getTabWidget();
    tabBar->closeTabsByZimId(id);
    eraseBookFilesFromComputer(mp_library->getBookFilePath(id), moveToTrash);
    mp_library->removeBookFromLibraryById(id);
    mp_library->save();
    emit mp_library->bookmarksChanged();
    if (m_local) {
        emit(bookRemoved(id));
    } else {
        emit(oneBookChanged(id));
    }
    getSettingsManager()->deleteSettings(id);
    emit booksChanged();
}

void ContentManager::eraseBook(const QString& id)
{
    auto text = gt("delete-book-text");
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    const auto moveToTrash = getSettingsManager()->getMoveToTrash();
#else
    const auto moveToTrash = false; // we do not support move to trash functionality for qt versions below 5.15
#endif
    if (moveToTrash) {
        text += formatText(gt("move-files-to-trash-text"));
    } else {
        text += formatText(gt("perma-delete-files-text"));
    }
    text = text.replace("{{ZIM}}", QString::fromStdString(mp_library->getBookById(id).getTitle()));
    showConfirmBox(gt("delete-book"), text, mp_view, [=]() {
        reallyEraseBook(id, moveToTrash);
    });
}

void ContentManager::pauseBook(const QString& id, QModelIndex index)
{
    DownloadManager::addRequest(DownloadState::PAUSE, id);
    managerModel->triggerDataUpdateAt(index);
}

void ContentManager::resumeBook(const QString& id, QModelIndex index)
{
    DownloadManager::addRequest(DownloadState::RESUME, id);
    managerModel->triggerDataUpdateAt(index);
}

void ContentManager::cancelBook(const QString& id)
{
    auto text = gt("cancel-download-text");
    text = text.replace("{{ZIM}}", QString::fromStdString(mp_library->getBookById(id).getTitle()));
    showConfirmBox(gt("cancel-download"), text, mp_view, [=]() {
        DownloadManager::addRequest(DownloadState::CANCEL, id);
    });
}

void ContentManager::downloadWasCancelled(const QString& id)
{
    removeDownload(id);

    // incompleted downloaded file should be perma deleted
    eraseBookFilesFromComputer(mp_library->getBookFilePath(id), false);
    mp_library->removeBookFromLibraryById(id);
    mp_library->save();
    emit(oneBookChanged(id));
}

void ContentManager::setCurrentLanguage(FilterList langPairList)
{
    QStringList languageList;
    for (auto &langPair : langPairList) {
        languageList.append(langPair.second);
    }
    languageList.sort();
    for (auto &language : languageList) {
        if (language.length() == 2) {
          try {
            language = QString::fromStdString(
                         kiwix::converta2toa3(language.toStdString()));
          } catch (std::out_of_range&) {}
        }
    }
    auto newLanguage = languageList.join(",");
    if (m_currentLanguage == newLanguage)
        return;
    m_currentLanguage = newLanguage;
    getSettingsManager()->setLanguage(langPairList);
    emit(currentLangChanged());
    emit(filterParamsChanged());
}

void ContentManager::setCurrentCategoryFilter(QStringList categoryList)
{
    categoryList.sort();
    if (m_categoryFilter == categoryList.join(","))
        return;
    m_categoryFilter = categoryList.join(",");
    getSettingsManager()->setCategory(categoryList);
    emit(filterParamsChanged());
}

void ContentManager::setCurrentContentTypeFilter(FilterList contentTypeFiltersPairList)
{
    QStringList contentTypeFilters;
    for (auto &ctfPair : contentTypeFiltersPairList) {
        contentTypeFilters.append(ctfPair.second);
    }
    m_contentTypeFilters = contentTypeFilters;
    getSettingsManager()->setContentType(contentTypeFiltersPairList);
    emit(filterParamsChanged());
}

void ContentManager::updateLibrary() {
    if (m_local) {
        emit(pendingRequest(false));
        emit(booksChanged());
        return;
    }
    try {
        emit(pendingRequest(true));
        m_remoteLibraryManager.doUpdate(m_currentLanguage, m_categoryFilter);
    } catch (std::runtime_error&) {}
}

void ContentManager::updateRemoteLibrary(const QString& content) {
    (void) QtConcurrent::run([=]() {
        QMutexLocker locker(&remoteLibraryLocker);
        mp_remoteLibrary = kiwix::Library::create();
        kiwix::Manager manager(mp_remoteLibrary);
        manager.readOpds(content.toStdString(), getRemoteLibraryUrl().toStdString());
        emit(this->booksChanged());
        emit(this->pendingRequest(false));
    });
}

void ContentManager::updateLanguages(const QString& content) {
    auto languages = kiwix::readLanguagesFromFeed(content.toStdString());
    LanguageList tempLanguages;
    for (auto language : languages) {
        auto code = QString::fromStdString(language.first);
        auto title = QString::fromStdString(language.second);
        tempLanguages.push_back({title, code});
    }
    m_languages = tempLanguages;
    emit(languagesLoaded(m_languages));
}

void ContentManager::updateCategories(const QString& content) {;
    auto categories = kiwix::readCategoriesFromFeed(content.toStdString());
    QStringList tempCategories;
    for (auto catg : categories) {
        tempCategories.push_back(QString::fromStdString(catg));
    }
    m_categories = tempCategories;
    emit(categoriesLoaded(m_categories));
}

void ContentManager::setSearch(const QString &search)
{
    m_searchQuery = search;
    emit(booksChanged());
}

QStringList ContentManager::getBookIds()
{
    kiwix::Filter filter;
    std::vector<std::string> acceptTags, rejectTags;

    for (auto &contentTypeFilter : m_contentTypeFilters) {
        acceptTags.push_back(contentTypeFilter.toStdString());
    }

    filter.acceptTags(acceptTags);
    filter.rejectTags(rejectTags);
    filter.query(m_searchQuery.toStdString());
    if (m_currentLanguage != "")
        filter.lang(m_currentLanguage.toStdString());
    if (m_categoryFilter != "")
        filter.category(m_categoryFilter.toStdString());

    if (m_local) {
        filter.local(true);
        filter.valid(true);
        return mp_library->listBookIds(filter, m_sortBy, m_sortOrderAsc);
    } else {
        filter.remote(true);
        QMutexLocker locker(&remoteLibraryLocker);
        auto bookIds = mp_remoteLibrary->filter(filter);
        mp_remoteLibrary->sort(bookIds, m_sortBy, m_sortOrderAsc);
        QStringList list;
        for(auto& bookId:bookIds) {
            list.append(QString::fromStdString(bookId));
        }
        return list;
    }
}

void ContentManager::setSortBy(const QString& sortBy, const bool sortOrderAsc)
{
    if (sortBy == "unsorted") {
        m_sortBy = kiwix::UNSORTED;
    } else if (sortBy == "title") {
        m_sortBy = kiwix::TITLE;
    } else if (sortBy == "size") {
        m_sortBy = kiwix::SIZE;
    } else if (sortBy == "date") {
        m_sortBy = kiwix::DATE;
    }
    m_sortOrderAsc = sortOrderAsc;
    emit(booksChanged());
}

////////////////////////////////////////////////////////////////////////////////
// Directory monitoring stuff
////////////////////////////////////////////////////////////////////////////////

void ContentManager::setMonitoredDirectories(QStringSet dirList)
{
    for (auto path : m_watcher.directories()) {
        m_watcher.removePath(path);
    }
    m_knownZimsInDir.clear();
    MonitoredZimFileInfo libraryZimFileInfo;
    libraryZimFileInfo.status = MonitoredZimFileInfo::ADDED_TO_THE_LIBRARY;
    for (auto dir : dirList) {
        if (dir != "") {
            auto& zimsInDir = m_knownZimsInDir[dir];
            for ( const auto& fname : mp_library->getLibraryZimsFromDir(dir) ) {
                zimsInDir.insert(fname, libraryZimFileInfo);
            }
            m_watcher.addPath(dir);
            asyncUpdateLibraryFromDir(dir);
        }
    }
}

void ContentManager::asyncUpdateLibraryFromDir(QString dir)
{
    (void) QtConcurrent::run([=]() {
        updateLibraryFromDir(dir);
    });
}

void ContentManager::handleDisappearedZimFiles(const QString& dirPath, const QStringSet& fileNames)
{
    const auto kiwixLib = mp_library->getKiwixLibrary();
    auto& zimsInDir = m_knownZimsInDir[dirPath];
    for (const auto& file : fileNames) {
        const auto bookPath = QDir::toNativeSeparators(dirPath + "/" + file);
        try {
            DBGOUT("directory monitoring: file disappeared: " << bookPath);
            const auto book = kiwixLib->getBookByPath(bookPath.toStdString());
            if ( handleDisappearedBook(QString::fromStdString(book.getId())) ) {
                zimsInDir.remove(file);
            }
        } catch (const std::exception& err) {
            DBGOUT("directory monitoring: "
                   "error while dropping the disappeared book: " << err.what());
            // the book was removed by the user via the UI
            zimsInDir.remove(file);
        }
    }
}

size_t ContentManager::handleNewZimFiles(const QString& dirPath, const QStringSet& fileNames)
{
    size_t countOfSuccessfullyAddedZims = 0;
    for (const auto& file : fileNames) {
        const bool addedToLib = handleZimFileInMonitoredDirLogged(dirPath, file);
        countOfSuccessfullyAddedZims += addedToLib;
    }
    return countOfSuccessfullyAddedZims;
}

namespace
{

#ifndef QT_NO_DEBUG

// indexed by MonitoredZimFileInfo::ZimFileStatus enum
const char* monitoredDirZimFileHandlingMsgs[] = {
    "",
    "it is being downloaded by us, ignoring...",
    "the file was added to the library",
    "the file could not be added to the library",
    "it is an unchanged known bad zim file",
    "deferring the check of an updated bad zim file",
    "bad zim file was updated but a deferred request to check it is pending"
};

#endif

} // unnamed namespace

bool ContentManager::handleZimFileInMonitoredDirLogged(QString dir, QString fileName)
{
    DBGOUT("ContentManager::handleZimFileInMonitoredDir(" << dir << ", " << fileName << ")");
    const int status = handleZimFileInMonitoredDir(dir, fileName);
    DBGOUT("\t" << monitoredDirZimFileHandlingMsgs[status]);
    return status == MonitoredZimFileInfo::ADDED_TO_THE_LIBRARY;
}

bool ContentManager::MonitoredZimFileInfo::fileKeepsBeingModified() const
{
    // A file is considered stable if it has stayed unchanged for at least
    // this long.
    const qint64 FILE_STABILITY_DURATION_MS = 1000;

    const QDateTime now = QDateTime::currentDateTime();
    return this->lastModified > now.addMSecs(-FILE_STABILITY_DURATION_MS);
}

void ContentManager::MonitoredZimFileInfo::updateStatus(const MonitoredZimFileInfo& prevInfo)
{
    Q_ASSERT(prevInfo.status != ADDED_TO_THE_LIBRARY);

    if ( this->lastModified == prevInfo.lastModified ) {
        this->status = UNCHANGED_KNOWN_BAD_ZIM_FILE;
    } else if ( prevInfo.status == PROCESS_LATER ) {
        this->status = DEFERRED_PROCESSING_ALREADY_PENDING;
    } else if ( this->fileKeepsBeingModified() ) {
        this->status = PROCESS_LATER;
    } else {
        this->status = PROCESS_NOW;
    }
}

ContentManager::MonitoredZimFileInfo ContentManager::getMonitoredZimFileInfo(QString dir, QString fileName) const
{
    const auto bookPath = QDir::toNativeSeparators(dir + "/" + fileName);

    MonitoredZimFileInfo zimFileInfo;

    zimFileInfo.lastModified = QFileInfo(bookPath).lastModified();

    const auto& zimsInDir = m_knownZimsInDir[dir];
    const auto fileInfoEntry = zimsInDir.constFind(fileName);
    if ( fileInfoEntry != zimsInDir.constEnd() ) {
        zimFileInfo.updateStatus(fileInfoEntry.value());
    }

    return zimFileInfo;
}

int ContentManager::handleZimFileInMonitoredDir(QString dir, QString fileName)
{
    const auto bookPath = QDir::toNativeSeparators(dir + "/" + fileName);

    if ( mp_library->isBeingDownloadedByUs(bookPath) ) {
        return MonitoredZimFileInfo::BEING_DOWNLOADED_BY_US;
    }

    MonitoredZimFileInfo zfi = getMonitoredZimFileInfo(dir, fileName);
    if ( zfi.status == MonitoredZimFileInfo::PROCESS_LATER ) {
        deferHandlingOfZimFileInMonitoredDir(dir, fileName);
    } else if ( zfi.status == MonitoredZimFileInfo::PROCESS_NOW ) {
        kiwix::Manager manager(mp_library->getKiwixLibrary());
        const bool addedToLib = manager.addBookFromPath(bookPath.toStdString());
        zfi.status = addedToLib
                   ? MonitoredZimFileInfo::ADDED_TO_THE_LIBRARY
                   : MonitoredZimFileInfo::COULD_NOT_BE_ADDED_TO_THE_LIBRARY;
        m_knownZimsInDir[dir].insert(fileName, zfi);
    }
    return zfi.status;
}

ContentManager::QStringSet ContentManager::getLibraryZims(QString dirPath) const
{
    QStringSet zimFileNames;
    const auto& zimsInDir = m_knownZimsInDir[dirPath];
    for ( auto it = zimsInDir.begin(); it != zimsInDir.end(); ++it ) {
        if ( it.value().status == MonitoredZimFileInfo::ADDED_TO_THE_LIBRARY )
            zimFileNames.insert(it.key());
    }
    return zimFileNames;
}

void ContentManager::updateLibraryFromDir(QString dirPath)
{
    QMutexLocker locker(&m_updateFromDirMutex);
    const QDir dir(dirPath);
    const QStringSet zimsPresentInLib = getLibraryZims(dirPath);

    QStringSet zimsInDir;
    for (const auto &file : dir.entryList({"*.zim"})) {
        zimsInDir.insert(file);
    }

    const QStringSet zimsNotInLib = zimsInDir - zimsPresentInLib;
    const QStringSet removedZims = zimsPresentInLib - zimsInDir;
    handleDisappearedZimFiles(dirPath, removedZims);
    const auto countOfAddedZims = handleNewZimFiles(dirPath, zimsNotInLib);
    if (!removedZims.empty() || countOfAddedZims != 0) {
        mp_library->save();
        emit(booksChanged());
    }
}

void ContentManager::handleZimFileInMonitoredDirDeferred(QString dir, QString fileName)
{
    QMutexLocker locker(&m_updateFromDirMutex);
    DBGOUT("ContentManager::handleZimFileInMonitoredDirDeferred(" << dir << ", " << fileName << ")");
    m_knownZimsInDir[dir][fileName].status = MonitoredZimFileInfo::PROCESS_NOW;
    if ( handleZimFileInMonitoredDirLogged(dir, fileName) ) {
        mp_library->save();
        emit(booksChanged());
    }
}

void ContentManager::deferHandlingOfZimFileInMonitoredDir(QString dir, QString fname)
{
    const qint64 DEBOUNCING_DELAY_MILLISECONDS = 1000;

    m_knownZimsInDir[dir][fname].status = MonitoredZimFileInfo::PROCESS_LATER;

    QTimer::singleShot(DEBOUNCING_DELAY_MILLISECONDS, this, [=]() {
        handleZimFileInMonitoredDirDeferred(dir, fname);
    });
}

bool ContentManager::handleDisappearedBook(QString bookId)
{
    if ( KiwixApp::instance()->getTabWidget()->getTabZimIds().contains(bookId) )
        return false;

    mp_library->removeBookFromLibraryById(bookId);
    return true;
}
