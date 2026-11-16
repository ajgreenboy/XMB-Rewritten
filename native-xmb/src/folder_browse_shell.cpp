// The in-XMB folder browser (folder_browser.h; audit/FOLDER-BROWSER.md). It stands where the native Windows
// folder dialog used to open: Choose Media Folder, and the Video, Music and Photo Library rows while no folder
// is set. The page is an ordinary XMB list page, so the pad drives it like every other one.
//
//   first row        Use This Folder (not on This PC, not on a folder that cannot be read)
//   other rows       the drives of this PC at the top level, otherwise the sub-folders of the folder shown
//   Cross            on a drive or folder: go into it; on Use This Folder: choose it
//   Circle           up one folder; from a drive root to the drive list; from the drive list closes the page
//
// Choosing hands the folder to Shell::chooseLibrary(), exactly as the dialog's result went before: it is stored
// as library/<column> and the folder opens.
//
// File mode (Choose Wallpaper, Choose Theme; openFilePicker): the same page, with no Use row. The rows are the
// sub-folders and then the files the dialog's filter accepted; Cross on a file closes the page and hands it to
// Shell::chooseWallpaper() or Shell::chooseTheme(), exactly where the dialog's answer went.
#include "shell.h"
#include "folder_browser.h"
#include "glyphs.h"
#include <QDir>
#include <QStandardPaths>

namespace xmb {
namespace {
Entry browseRow(QString id,QString title,Action action,QString value={},QString detail={}) {
    Entry e;e.id=std::move(id);e.title=std::move(title);e.action=action;e.value=std::move(value);e.detail=std::move(detail);
    e.container=QStringLiteral("explore_plugin_full");e.texture=QStringLiteral("item_tex_plain_folder");e.source=QStringLiteral("native");
    return e;
}
const QStringList& filePatterns(const QString& picker) {
    // The filters the native dialogs had: "Images (*.png *.jpg *.jpeg *.bmp *.webp)" and "PS3 Themes (*.p3t)".
    static const QStringList wallpaper{QStringLiteral("*.png"),QStringLiteral("*.jpg"),QStringLiteral("*.jpeg"),QStringLiteral("*.bmp"),QStringLiteral("*.webp")};
    static const QStringList theme{QStringLiteral("*.p3t")};
    static const QStringList none;
    return picker==QLatin1String("wallpaper")?wallpaper:picker==QLatin1String("theme")?theme:none;
}
}

QVector<Entry> Shell::browseRows(const QString& path) const {
    const QString picker=pages_.isEmpty()?QString():pages_.last().browseFiles;
    const bool fileMode=!picker.isEmpty();
    const auto listing=folderbrowser::list(path,2000,filePatterns(picker));
    QVector<Entry> rows;
    if(path.isEmpty()) {
        for(const auto& drive:listing.items) {
            auto row=browseRow(QStringLiteral("browse:")+drive.path,drive.name,Action::BrowseOpen,drive.path,drive.note);
            row.artwork=glyphs::device(QStringLiteral("Computer"));row.iconSized=true;
            rows.push_back(std::move(row));
        }
        if(rows.isEmpty())rows.push_back(browseRow(QStringLiteral("browse:none"),tr("No Drives Found"),Action::Information,
            tr("Windows did not list any drive."),QString()));
        return rows;
    }
    if(listing.problem.isEmpty() && !fileMode) {
        auto use=browseRow(QStringLiteral("browse:use"),tr("Use This Folder"),Action::BrowseUse,listing.path,
            folderbrowser::elidePath(listing.path,56));
        use.artwork=glyphs::check();use.iconSized=true;
        rows.push_back(std::move(use));
    }
    else if(!listing.problem.isEmpty()) rows.push_back(browseRow(QStringLiteral("browse:problem"),tr("This Folder Cannot Be Read"),Action::Information,
        listing.problem+QLatin1Char('\n')+tr("Circle goes back up one folder."),listing.problem));
    for(const auto& folder:listing.items) {
        if(!folder.isFile){rows.push_back(browseRow(QStringLiteral("browse:")+folder.path,folder.name,Action::BrowseOpen,folder.path));continue;}
        auto row=browseRow(QStringLiteral("browse:")+folder.path,folder.name,Action::BrowseFile,folder.path,folder.note);
        row.artwork=picker==QLatin1String("theme")?glyphs::document():glyphs::picture();row.iconSized=true;
        rows.push_back(std::move(row));
    }
    if(fileMode && listing.problem.isEmpty() && listing.items.isEmpty())
        rows.push_back(browseRow(QStringLiteral("browse:empty"),tr("No Matching Files"),Action::Information,
            picker==QLatin1String("theme")?tr("This folder has no theme files (.p3t) and no folders inside it. Circle goes back up one folder.")
                :tr("This folder has no images (png, jpg, bmp or webp) and no folders inside it. Circle goes back up one folder."),QString()));
    else if(listing.problem.isEmpty() && listing.items.isEmpty())
        rows.push_back(browseRow(QStringLiteral("browse:empty"),tr("No Folders Inside"),Action::Information,
            tr("This folder has no folders inside it. Use This Folder chooses it as it is."),QString()));
    if(listing.truncated)
        rows.push_back(browseRow(QStringLiteral("browse:more"),fileMode?tr("More Entries Than Can Be Listed"):tr("More Folders Than Can Be Listed"),Action::Information,
            fileMode?tr("Only the first %1 folders and files are listed. Go up and choose a folder closer to the file.")
                :tr("Only the first %1 folders are listed. Go up and choose a folder closer to the media.").arg(listing.items.size()),QString()));
    return rows;
}

void Shell::browseGo(const QString& path,const QString& focusPath) {
    if(pages_.isEmpty() || !pages_.last().browse)return;
    auto rows=browseRows(path);
    int selection=0;
    if(!focusPath.isEmpty())for(int i=0;i<rows.size();++i)if(rows[i].value==focusPath && rows[i].action==Action::BrowseOpen){selection=i;break;}
    auto& page=pages_.last();
    page.browsePath=path;page.entries=std::move(rows);page.selection=selection;
    qInfo().noquote()<<QStringLiteral("folder browser: showing \"%1\" rows=%2")
        .arg(path.isEmpty()?QStringLiteral("This PC"):QDir::toNativeSeparators(path)).arg(page.entries.size());
}

void Shell::openFolderBrowser() {
    if(pages_.size()>=32){showMessage("The maximum folder depth has been reached.",MessageKind::Page);return;}
    const QString column=categories_[category_].id;
    const QString current=preferences_.value("library/"+column).toString();
    const QString fallback=QStandardPaths::writableLocation(column==QLatin1String("music")?QStandardPaths::MusicLocation
        :column==QLatin1String("photo")?QStandardPaths::PicturesLocation:QStandardPaths::MoviesLocation);
    // XMB_FOLDER_START is a test-only start folder for scripted runs ("roots" starts at the drive list).
    const QString start=folderbrowser::startPath(qEnvironmentVariable("XMB_FOLDER_START"),current,fallback);
    Page page;page.title=tr("Choose Media Folder");page.browse=true;page.browsePath=start;
    pages_.push_back(std::move(page));
    browseGo(start,QString());
}

// Choose Wallpaper and Choose Theme: the file picker page ("wallpaper" or "theme"). It opens where the current
// choice lives (its folder), otherwise Pictures or Downloads; XMB_FOLDER_START overrides it for scripted runs.
void Shell::openFilePicker(const QString& picker,bool replaceChoices) {
    // From a Theme or Background choice band the band is replaced, so it does not stay drawn beside the picker.
    if(replaceChoices && !pages_.isEmpty() && !pages_.last().browse)pages_.removeLast();
    if(pages_.size()>=32){showMessage("The maximum folder depth has been reached.",MessageKind::Page);return;}
    const bool theme=picker==QLatin1String("theme");
    const QString current=preferences_.value(theme?"appearance/theme":"appearance/wallpaper").toString();
    const QString fallback=QStandardPaths::writableLocation(theme?QStandardPaths::DownloadLocation:QStandardPaths::PicturesLocation);
    const QString start=folderbrowser::startPath(qEnvironmentVariable("XMB_FOLDER_START"),current,fallback);
    Page page;page.title=theme?tr("Choose Theme"):tr("Choose Wallpaper");page.browse=true;page.browsePath=start;page.browseFiles=picker;
    pages_.push_back(std::move(page));
    browseGo(start,QString());
}

bool Shell::browseUp() {
    if(pages_.isEmpty() || !pages_.last().browse || pages_.last().browsePath.isEmpty())return false;
    const QString from=pages_.last().browsePath;
    browseGo(folderbrowser::parentOf(from),from);
    return true;
}

bool Shell::activateBrowse(const Entry& entry) {
    if(pages_.isEmpty() || !pages_.last().browse)return true;
    if(entry.action==Action::BrowseOpen){browseGo(entry.value,QString());return true;}
    if(entry.action==Action::BrowseFile) {
        // A file in the picker: the page closes and the file goes where the dialog's answer went.
        const QString picker=pages_.last().browseFiles,chosen=entry.value;
        qInfo().noquote()<<QStringLiteral("file picker: chose \"%1\" for %2").arg(QDir::toNativeSeparators(chosen),picker);
        pages_.removeLast();
        noteActivity();
        if(picker==QLatin1String("theme"))chooseTheme(chosen);else chooseWallpaper(chosen);
        return true;
    }
    // Use This Folder: the page closes and the choice goes where the dialog's answer went.
    const QString chosen=entry.value;
    qInfo().noquote()<<QStringLiteral("folder browser: chose \"%1\"").arg(QDir::toNativeSeparators(chosen));
    pages_.removeLast();
    // Opened from a library page: that page is replaced by the reloaded one for the new folder.
    if(!pages_.isEmpty() && pages_.last().libraryRoot){library_.cancel();scanRequest_=0;loading_=false;pages_.removeLast();}
    chooseLibrary(chosen);
    return true;
}

}
