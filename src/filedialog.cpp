#include "filedialog.h"
#include "cachedfoldermodel.h"
#include "proxyfoldermodel.h"
#include "utilities.h"
#include "core/fileinfojob.h"
#include "ui_filedialog.h"

#include <QDialogButtonBox>
#include <QPushButton>
#include <QMimeType>
#include <QMimeDatabase>
#include <QMessageBox>
#include <QToolBar>
#include <QCompleter>
#include <QDebug>

namespace Fm {


FileDialog::FileDialog(QWidget* parent, FilePath path) :
    QDialog(parent),
    ui{new Ui::FileDialog()},
    folderModel_{nullptr},
    proxyModel_{nullptr},
    options_{0},
    viewMode_{FolderView::DetailedListMode},
    fileMode_{QFileDialog::AnyFile},
    acceptMode_{QFileDialog::AcceptOpen},
    modelFilter_{this} {

    ui->setupUi(this);

    // path bar
    connect(ui->location, &PathBar::chdir, this, &FileDialog::setDirectoryPath);

    // side pane
    ui->sidePane->setMode(Fm::SidePane::ModePlaces);
    connect(ui->sidePane, &SidePane::chdirRequested, [this](int type, const FilePath &path) {
        setDirectoryPath(path);
    });

    // folder view
    proxyModel_ = new ProxyFolderModel(this);
    proxyModel_->sort(FolderModel::ColumnFileName, Qt::AscendingOrder);
    proxyModel_->setThumbnailSize(64);
    proxyModel_->setShowThumbnails(true);

    proxyModel_->addFilter(&modelFilter_);

    ui->folderView->setViewMode(viewMode_);
    connect(ui->folderView, &FolderView::clicked, this, &FileDialog::onFileClicked);
    ui->folderView->setModel(proxyModel_);
    // set the completer
    QCompleter* completer = new QCompleter();
    completer->setModel(proxyModel_);
    ui->fileName->setCompleter(completer);
    // update selection mode for the view
    updateSelectionMode();

    // selection changes
    connect(ui->folderView->selectionModel(), &QItemSelectionModel::currentRowChanged, this, &FileDialog::onCurrentRowChanged);
    connect(ui->folderView->selectionModel(), &QItemSelectionModel::selectionChanged, this, &FileDialog::onSelectionChanged);

    // file type
    connect(ui->fileTypeCombo, &QComboBox::currentTextChanged, [this](const QString& text) {
        selectNameFilter(text);
    });
    ui->fileTypeCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLength);
    ui->fileTypeCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    ui->fileTypeCombo->setCurrentIndex(0);

    // setup toolbar buttons
    auto toolbar = new QToolBar(this);
    auto newFolderAction = toolbar->addAction(QIcon::fromTheme("folder-new"), tr("Create Folder"));
    connect(newFolderAction, &QAction::triggered, this, &FileDialog::onNewFolder);
    toolbar->addSeparator();

    auto viewModeGroup = new QActionGroup(this);
    iconViewAction_ = toolbar->addAction(style()->standardIcon(QStyle::SP_FileDialogContentsView), tr("Icon View"));
    iconViewAction_->setCheckable(true);
    connect(iconViewAction_, &QAction::toggled, this, &FileDialog::onViewModeToggled);
    viewModeGroup->addAction(iconViewAction_);
    thumbnailViewAction_ = toolbar->addAction(style()->standardIcon(QStyle::SP_FileDialogInfoView), tr("Thumbnail View"));
    thumbnailViewAction_->setCheckable(true);
    connect(thumbnailViewAction_, &QAction::toggled, this, &FileDialog::onViewModeToggled);
    viewModeGroup->addAction(thumbnailViewAction_);
    compactViewAction_ = toolbar->addAction(style()->standardIcon(QStyle::SP_FileDialogListView), tr("Compact View"));
    compactViewAction_->setCheckable(true);
    connect(compactViewAction_, &QAction::toggled, this, &FileDialog::onViewModeToggled);
    viewModeGroup->addAction(compactViewAction_);
    detailedViewAction_ = toolbar->addAction(style()->standardIcon(QStyle::SP_FileDialogDetailedView), tr("Detailed List View"));
    detailedViewAction_->setCheckable(true);
    connect(detailedViewAction_, &QAction::toggled, this, &FileDialog::onViewModeToggled);
    viewModeGroup->addAction(detailedViewAction_);
    ui->toolbarLayout->addWidget(toolbar);

    setViewMode(viewMode_);

    // setup the splitter
    // FIXME: make these sizes configurable
    QList<int> sizes;
    sizes.append(200);
    sizes.append(320);
    ui->splitter->setSizes(sizes);

    // browse to the directory
    if(path.isValid()) {
        setDirectoryPath(path);
        directoryPath_ = std::move(path);
    }
    ui->fileName->setFocus();
}

FileDialog::~FileDialog() {
}

void FileDialog::accept() {
    // handle selected filenames
    selectedFiles_.clear();
    // parse the file names from the text entry
    QStringList parsedNames;
    auto fileNames = ui->fileName->text();
    if(fileNames.isEmpty()) {
        // when selecting a dir and the name is not provided, just select current dir in the view
        if(fileMode_ == QFileDialog::Directory) {
            selectedFiles_.append(directory());
        }
        else {
            QMessageBox::critical(this, tr("Error"), tr("Please select a file"));
            return;
        }
    }
    else {
        // check if there are multiple file names (containing ")
        auto firstQuote = fileNames.indexOf('\"');
        auto lastQuote = fileNames.lastIndexOf('\"');
        if(firstQuote != -1 && lastQuote != -1) {
            // split the names
            QRegExp sep{"\"\\s+\""};  // separated with " "
            parsedNames = fileNames.mid(firstQuote + 1, lastQuote - firstQuote - 1).split(sep);
        }
        else {
            parsedNames << fileNames;
        }

        // get full paths for the filenames and convert them to URLs
        for(auto& name: parsedNames) {
            // add default filename extension as needed
            if(!defaultSuffix_.isEmpty() && name.lastIndexOf('.') == -1) {
                name += '.';
                name += defaultSuffix_;
            }
            auto fullPath = directoryPath_.child(name.toLocal8Bit().constData());
            selectedFiles_.append(QUrl::fromEncoded(fullPath.uri().get()));
        }
    }

    // check existence of the selected files and if their types are correct
    // async operation, call doAccept() in the callback.
    ui->buttonBox->button(QDialogButtonBox::Ok)->setEnabled(false);

    auto pathList = pathListFromQUrls(selectedFiles_);
    auto job = new FileInfoJob(pathList);
    job->setAutoDelete(true);
    connect(job, &Job::finished, this, &FileDialog::onFileInfoJobFinished);
    job->runAsync();
}

void FileDialog::reject() {
    QDialog::reject();
}

void FileDialog::setDirectory(const QUrl &directory) {
    auto path = Fm::FilePath::fromUri(directory.toEncoded().constData());
    setDirectoryPath(path);
}

// interface for QPlatformFileDialogHelper

void FileDialog::setDirectoryPath(FilePath directory) {
    ui->location->setPath(directory);
    ui->sidePane->chdir(directory);
    ui->folderView->model();

    auto oldModel = folderModel_;
    folderModel_ = Fm::CachedFolderModel::modelFromPath(directory);
    proxyModel_->setSourceModel(folderModel_);

    if(oldModel != nullptr) {
        // FIXME: is this correct?
        oldModel->unref();
    }
    directoryPath_ = std::move(directory);

    QUrl uri = QUrl::fromEncoded(directory.uri().get());
    Q_EMIT directoryEntered(uri);
}

void FileDialog::selectFilePath(const FilePath &path) {
    auto idx = proxyModel_->indexFromPath(path);

    // FIXME: add a method to Fm::FolderView to select files

    // FIXME: need to add this for detailed list
    QItemSelectionModel::SelectionFlags flags = QItemSelectionModel::Select;
    if(viewMode_ == QFileDialog::Detail) {
        flags |= QItemSelectionModel::Rows;
    }
    ui->folderView->selectionModel()->select(idx, flags);
}

void FileDialog::onCurrentRowChanged(const QModelIndex &current, const QModelIndex &previous) {
    // emit currentChanged signal
    QUrl currentUrl;
    if(current.isValid()) {
        // emit changed siangl for newly selected items
        auto fi = proxyModel_->fileInfoFromIndex(current);
        if(fi) {
            currentUrl = QUrl::fromEncoded(fi->path().uri().get());
        }
    }
    Q_EMIT currentChanged(currentUrl);
}

void FileDialog::onSelectionChanged(const QItemSelection &selected, const QItemSelection &deselected) {
    QString fileNames;
    auto selFiles = ui->folderView->selectedFiles();
    bool multiple(selFiles.size() > 1);
    for(auto& fileInfo: selFiles) {
        if(fileMode_ == QFileDialog::Directory) {
            // if we want to select dir, ignore selected files
            if(!fileInfo->isDir()) {
                continue;
            }
        }
        else if(fileInfo->isDir()) {
            // if we want to select files, ignore selected dirs
            continue;
        }

        auto baseName = fileInfo->path().baseName();
        if(multiple) {
            // support multiple selection
            if(!fileNames.isEmpty()) {
                fileNames += ' ';
            }
            // FIXME: use a more reliable way to quote file names.
            // otherwise names with embedded " will break.
            fileNames += '\"';
            fileNames += baseName.get();
            fileNames += '\"';
        }
        else {
            // support single selection only
            fileNames = baseName.get();
            break;
        }
    }
    ui->fileName->setText(fileNames);
}

void FileDialog::onFileClicked(int type, const std::shared_ptr<const FileInfo> &file) {
    bool canAccept = false;
    if(file && type == FolderView::ActivatedClick) {
        if(file->isDir()) {
            // chdir into the activated dir
            setDirectoryPath(file->path());

            if(fileMode_ == QFileDialog::Directory) {
                ui->fileName->clear();
            }
        }
        else if(fileMode_ != QFileDialog::Directory) {
            // select file(s) and a file item is activated
            canAccept = true;
        }
    }

    if(canAccept) {
        selectFilePath(file->path());
        accept();
    }
}

void FileDialog::onNewFolder() {
    createFileOrFolder(CreateNewFolder, directoryPath_, nullptr, this);
}

void FileDialog::onViewModeToggled(bool active) {
    if(active) {
        auto action = static_cast<QAction*>(sender());
        FolderView::ViewMode newMode;
        if(action == iconViewAction_) {
            newMode = FolderView::IconMode;
        }
        else if(action == thumbnailViewAction_) {
            newMode = FolderView::ThumbnailMode;
        }
        else if(action == compactViewAction_) {
            newMode = FolderView::CompactMode;
        }
        else if(action == detailedViewAction_) {
            newMode = FolderView::DetailedListMode;
        }
        else {
            return;
        }
        setViewMode(newMode);
    }
}

void FileDialog::updateSelectionMode() {
    // enable multiple selection?
    ui->folderView->childView()->setSelectionMode(fileMode_ == QFileDialog::ExistingFiles ? QAbstractItemView::ExtendedSelection : QAbstractItemView::SingleSelection);
}

void FileDialog::doAccept() {

    Q_EMIT filesSelected(selectedFiles_);

    if(selectedFiles_.size() == 1) {
        Q_EMIT fileSelected(selectedFiles_[0]);
    }

    QDialog::accept();
}

void FileDialog::onFileInfoJobFinished() {
    auto job = static_cast<FileInfoJob*>(sender());
    if(job->isCancelled()) {
        selectedFiles_.clear();
        reject();
    }
    else {
        QString error;
        // check if the files exist and their types are correct
        auto paths = job->paths();
        auto files = job->files();
        for(size_t i = 0; i < paths.size(); ++i) {
            const auto& path = paths[i];
            if(i >= files.size() || files[i]->path() != path) {
                // the file path is not found and does not have file info
                if(fileMode_ != QFileDialog::AnyFile) {
                    // if we do not allow non-existent file, this is an error.
                    error = tr("Path \"%1\" does not exist").arg(path.displayName().get());
                    break;
                }
                ++i; // skip the file
                continue;
            }

            // FIXME: currently, if a path is not found, FmFileInfoJob does not return its file info object.
            // This is bad API design. We may return nullptr for the failed file info query instead.
            const auto& file = files[i];
            // check if the file type is correct
            if(fileMode_ == QFileDialog::Directory) {
                if(!file->isDir()) {
                    // we're selecting dirs, but the selected file path does not point to a dir
                    error = tr("\"%1\" is not a directory").arg(path.displayName().get());
                    break;
                }
            }
            else if(file->isDir() || file->isShortcut()) {
                // we're selecting files, but the selected file path refers to a dir or shortcut (such as computer:///)
                error = tr("\"%1\" is not a file").arg(path.displayName().get());;
                break;
            }
        }

        if(error.isEmpty()) {
            // no error!
            doAccept();
        }
        else {
            QMessageBox::critical(this, tr("Error"), error);
            selectedFiles_.clear();
        }
    }
    ui->buttonBox->button(QDialogButtonBox::Ok)->setEnabled(true);
}

QUrl FileDialog::directory() const {
    QUrl url{directoryPath_.uri().get()};
    return url;
}

void FileDialog::selectFile(const QUrl& filename) {
    auto urlStr = filename.toEncoded();
    auto path = FilePath::fromUri(urlStr.constData());
    selectFilePath(path);
}

QList<QUrl> FileDialog::selectedFiles() {
    return selectedFiles_;
}


void FileDialog::selectNameFilter(const QString& filter) {
    if(filter != currentNameFilter_) {
        currentNameFilter_ = filter;
        ui->fileTypeCombo->setCurrentText(filter);

        modelFilter_.update();
        proxyModel_->invalidate();
        Q_EMIT filterSelected(filter);
    }
}


bool FileDialog::isSupportedUrl(const QUrl& url) {
    auto scheme = url.scheme().toLocal8Bit();
    // FIXME: this is not reliable due to the bug of gvfs.
    return Fm::isUriSchemeSupported(scheme.constData());
}


// options

void FileDialog::setFilter(QDir::Filters filters) {
    filters_ = filters;
    // TODO:
}

void FileDialog::setViewMode(FolderView::ViewMode mode) {
    viewMode_ = mode;
    ui->folderView->setViewMode(mode);
    switch(mode) {
    case FolderView::IconMode:
        iconViewAction_->setChecked(true);
        break;
    case FolderView::ThumbnailMode:
        thumbnailViewAction_->setChecked(true);
        break;
    case FolderView::CompactMode:
        compactViewAction_->setChecked(true);
        break;
    case FolderView::DetailedListMode:
        detailedViewAction_->setChecked(true);
        break;
    default:
        break;
    }
    // update selection mode for the view
    updateSelectionMode();
}


void FileDialog::setFileMode(QFileDialog::FileMode mode) {
    if(mode == QFileDialog::DirectoryOnly) {
        // directly only is deprecated and not allowed.
        mode = QFileDialog::Directory;
    }
    fileMode_ = mode;

    // enable multiple selection?
    updateSelectionMode();
}


void FileDialog::setAcceptMode(QFileDialog::AcceptMode mode) {
    acceptMode_ = mode;
    // TODO: open or save (default window title)
}

void FileDialog::setNameFilters(const QStringList& filters) {
    if(filters.isEmpty()) {
        // default filename pattern
        nameFilters_ = (QStringList() << tr("All Files (*)"));
    }
    else {
        nameFilters_ = filters;
    }
    ui->fileTypeCombo->clear();
    ui->fileTypeCombo->addItems(filters);
}

void FileDialog::setMimeTypeFilters(const QStringList& filters) {
    mimeTypeFilters_ = filters;

    QStringList nameFilters;
    QMimeDatabase db;
    for(const auto& filter: filters) {
        auto mimeType = db.mimeTypeForName(filter);
        auto nameFilter = mimeType.comment();
        if(!mimeType.suffixes().empty()) {
            nameFilter + " (";
            for(const auto& suffix: mimeType.suffixes()) {
                nameFilter += "*.";
                nameFilter += suffix;
                nameFilter += ' ';
            }
            nameFilter[nameFilter.length() - 1] = ')';
        }
        nameFilters << nameFilter;
    }
    setNameFilters(nameFilters);
}


void FileDialog::setLabelText(QFileDialog::DialogLabel label, const QString& text) {
    switch(label) {
    case QFileDialog::LookIn:
        ui->lookInLabel->setText(text);
        break;
    case QFileDialog::FileName:
        ui->fileNameLabel->setText(text);
        break;
    case QFileDialog::FileType:
        ui->fileTypeLabel->setText(text);
        break;
    case QFileDialog::Accept:
        ui->buttonBox->button(QDialogButtonBox::Ok)->setText(text);
        break;
    case QFileDialog::Reject:
        ui->buttonBox->button(QDialogButtonBox::Cancel)->setText(text);
        break;
    default:
        break;
    }
}

QString FileDialog::labelText(QFileDialog::DialogLabel label) const {
    QString text;
    switch(label) {
    case QFileDialog::LookIn:
        text = ui->lookInLabel->text();
        break;
    case QFileDialog::FileName:
        text = ui->fileNameLabel->text();
        break;
    case QFileDialog::FileType:
        text = ui->fileTypeLabel->text();
        break;
    case QFileDialog::Accept:
        ui->buttonBox->button(QDialogButtonBox::Ok)->text();
        break;
    case QFileDialog::Reject:
        ui->buttonBox->button(QDialogButtonBox::Cancel)->text();
        break;
    default:
        break;
    }
    return text;
}


bool FileDialog::FileDialogFilter::filterAcceptsRow(const ProxyFolderModel *model, const std::shared_ptr<const FileInfo> &info) const {
    if(dlg_->fileMode_ == QFileDialog::Directory) {
        // we only want to select directories
        if(!info->isDir()) { // not a dir
            // NOTE: here we ignore dlg_->options_& QFileDialog::ShowDirsOnly option.
            return false;
        }
    }
    else {
        // we want to select files, so all directories can be shown regardless of their names
        if(info->isDir()) {
            return true;
        }
    }

    bool nameMatched = false;
    auto& name = info->displayName();
    for(const auto& pattern: patterns_) {
        if(pattern.exactMatch(name)) {
            nameMatched = true;
            break;
        }
    }
    return nameMatched;
}

void FileDialog::FileDialogFilter::update() {
    // update filename pattersn
    patterns_.clear();
    QString nameFilter = dlg_->currentNameFilter_;
    // if the filter contains (...), only get the part between the parenthesis.
    auto left = nameFilter.indexOf('(');
    if(left != -1) {
        ++left;
        auto right = nameFilter.indexOf(')', left);
        if(right == -1) {
            right = nameFilter.length();
        }
        nameFilter = nameFilter.mid(left, right - left);
    }
    // parse the "*.ext1 *.ext2 *.ext3 ..." list into QRegExp objects
    auto globs = nameFilter.simplified().split(' ');
    for(const auto& glob: globs) {
        patterns_.emplace_back(QRegExp(glob, Qt::CaseInsensitive, QRegExp::Wildcard));
    }
}

} // namespace Fm