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
}

QVector<Entry> Shell::browseRows(const QString& path) const {
    const auto listing=folderbrowser::list(path);
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
    if(listing.problem.isEmpty()) {
        auto use=browseRow(QStringLiteral("browse:use"),tr("Use This Folder"),Action::BrowseUse,listing.path,
            folderbrowser::elidePath(listing.path,56));
        use.artwork=glyphs::check();use.iconSized=true;
        rows.push_back(std::move(use));
    }
    else rows.push_back(browseRow(QStringLiteral("browse:problem"),tr("This Folder Cannot Be Read"),Action::Information,
        listing.problem+QLatin1Char('\n')+tr("Circle goes back up one folder."),listing.problem));
    for(const auto& folder:listing.items)
        rows.push_back(browseRow(QStringLiteral("browse:")+folder.path,folder.name,Action::BrowseOpen,folder.path));
    if(listing.problem.isEmpty() && listing.items.isEmpty())
        rows.push_back(browseRow(QStringLiteral("browse:empty"),tr("No Folders Inside"),Action::Information,
            tr("This folder has no folders inside it. Use This Folder chooses it as it is."),QString()));
    if(listing.truncated)
        rows.push_back(browseRow(QStringLiteral("browse:more"),tr("More Folders Than Can Be Listed"),Action::Information,
            tr("Only the first %1 folders are listed. Go up and choose a folder closer to the media.").arg(listing.items.size()),QString()));
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

bool Shell::browseUp() {
    if(pages_.isEmpty() || !pages_.last().browse || pages_.last().browsePath.isEmpty())return false;
    const QString from=pages_.last().browsePath;
    browseGo(folderbrowser::parentOf(from),from);
    return true;
}

bool Shell::activateBrowse(const Entry& entry) {
    if(pages_.isEmpty() || !pages_.last().browse)return true;
    if(entry.action==Action::BrowseOpen){browseGo(entry.value,QString());return true;}
    // Use This Folder: the page closes and the choice goes where the dialog's answer went.
    const QString chosen=entry.value;
    qInfo().noquote()<<QStringLiteral("folder browser: chose \"%1\"").arg(QDir::toNativeSeparators(chosen));
    pages_.removeLast();
    chooseLibrary(chosen);
    return true;
}

}
