// Add a Game, controller-driven (audit/LOCAL-GAME-ADD.md). The Game > Manage hub's "Add a Game" row opens an
// ordinary XMB page of rows, so the pad drives it like every other page; no modal Qt dialog and no native file
// dialog is involved. The flow:
//
//   Executable      Cross opens the in-XMB folder browser in .exe mode (folder_browse_shell.cpp); choosing a
//                   file returns here with the path filled and the name defaulted to the file's base name
//   Name            the XMB keyboard (the one field-edit signal the shell already routes)
//   Artwork Path    the XMB keyboard, for an existing picture path (paste or type)
//   Choose Artwork  the in-XMB folder browser in image mode
//   Add Game        writes the registration into <data>/apps.json through ContentRegistry::upsert, then
//                   refreshes the Game column so the row appears and launches like any imported game
//   Cancel          (or Circle) leaves the page without writing anything
//
// The draft lives in memory (Shell::addGameDraft_); nothing is written until Add Game, and a cancelled or
// aborted browse leaves the draft exactly as it was. Saving reuses the existing local-game API: the saved
// app is the same ContentApp (kind "game", category "game") the column already lists and launches through
// Action::LaunchApp / Shell::startTracked.
#include "shell.h"
#include "folder_browser.h"
#include <QDir>
#include <QFileInfo>
#include <QUuid>
#include <algorithm>
#include <utility>

namespace xmb {
namespace {

Entry addGameRow(QString id,QString title,Action action,QString detail,QString value={}) {
    Entry e;
    e.id=std::move(id);e.title=std::move(title);e.action=action;e.detail=std::move(detail);e.value=std::move(value);
    e.container=QStringLiteral("explore_plugin_full");e.texture=QStringLiteral("item_tex_plain_folder");e.source=QStringLiteral("native");
    return e;
}

} // namespace

bool Shell::addGameOpen() const {
    for(const auto& page:pages_)if(page.addGame)return true;
    return false;
}

QVector<Entry> Shell::addGameRows() const {
    QVector<Entry> rows;
    const ContentApp& d=addGameDraft_;
    // A field is drawn like a settings row: the round bullet, the label, the value on the right (Entry::fieldValue).
    const auto field=[&](const QString& id,const QString& title,const QString& key,const QString& help,const QString& shown){
        auto e=addGameRow(id,title,Action::AddGameField,help,key);
        e.container=QStringLiteral("qgl/icontex");e.texture=QStringLiteral("23n");
        e.fieldValue=shown;rows.push_back(std::move(e));
    };
    {
        auto e=addGameRow(QStringLiteral("addgame:exe"),tr("Executable"),Action::AddGameChooseExe,
            tr("Cross to choose the game's .exe."));
        e.fieldValue=d.executable.isEmpty()?tr("Not chosen"):folderbrowser::elidePath(d.executable,44);
        rows.push_back(std::move(e));
    }
    field(QStringLiteral("addgame:title"),tr("Name"),QStringLiteral("title"),
        tr("Cross to rename the game."),d.title.isEmpty()?tr("Not set"):d.title);
    field(QStringLiteral("addgame:artwork"),tr("Artwork Path"),QStringLiteral("artwork"),
        tr("Cross to type or paste the path of a picture already on this PC."),
        d.artwork.isEmpty()?tr("None"):folderbrowser::elidePath(d.artwork,44));
    {
        rows.push_back(addGameRow(QStringLiteral("addgame:art"),tr("Choose Artwork"),Action::AddGameChooseArtwork,
            tr("Cross to browse for a picture (png, jpg, bmp or webp).")));
    }
    {
        auto e=addGameRow(QStringLiteral("addgame:save"),tr("Add Game"),Action::AddGameSave,
            tr("Adds the game to the Game column."));
        e.disabled=d.executable.isEmpty() || d.title.trimmed().isEmpty();
        rows.push_back(std::move(e));
    }
    {
        rows.push_back(addGameRow(QStringLiteral("addgame:cancel"),tr("Cancel"),Action::AddGameCancel,
            tr("Leaves without adding the game.")));
    }
    return rows;
}

void Shell::refreshAddGamePage() {
    for(auto& page:pages_) {
        if(!page.addGame)continue;
        const QString focused=page.entries.isEmpty()?QString{}:page.entries.value(page.selection).id;
        page.entries=addGameRows();
        page.selection=std::clamp(page.selection,0,std::max(0,int(page.entries.size())-1));
        for(int i=0;i<page.entries.size();++i)if(page.entries[i].id==focused){page.selection=i;break;}
    }
}

void Shell::openAddGamePage() {
    if(addGameOpen() || pages_.size()>=32)return;
    addGameDraft_=ContentApp{};
    addGameDraft_.id=QUuid::createUuid().toString(QUuid::WithoutBraces);
    addGameDraft_.kind=QStringLiteral("game");
    addGameDraft_.category=QStringLiteral("game");
    addGameTitleEdited_=false;
    Page page;page.title=tr("Add a Game");page.addGame=true;
    page.entries=addGameRows();
    pages_.push_back(std::move(page));
    qInfo().noquote()<<"add game: page opened (choose an executable, then name and add)";
    emit changed();
}

void Shell::setAddGameField(const QString& field,const QString& value) {
    if(!addGameOpen())return;
    if(field==QLatin1String("title")){addGameDraft_.title=value.trimmed();addGameTitleEdited_=true;}
    else if(field==QLatin1String("artwork"))addGameDraft_.artwork=value.trimmed();
    else return;
    // The field and its length only; a path is a user file and is not echoed to the log.
    qInfo().noquote()<<QStringLiteral("add game: %1 set (%2 character(s))").arg(field).arg(value.size());
    refreshAddGamePage();emit changed();
}

void Shell::chooseAddGameExe(const QString& file) {
    if(!addGameOpen())return;
    if(file.isEmpty())return;
    addGameDraft_.executable=QFileInfo(file).absoluteFilePath();
    // The name defaults to the executable's base name once; a name the user has since typed is kept.
    if(!addGameTitleEdited_)addGameDraft_.title=QFileInfo(file).completeBaseName();
    qInfo().noquote()<<"add game: executable chosen \""<<QDir::toNativeSeparators(addGameDraft_.executable)<<"\"";
    refreshAddGamePage();emit changed();
}

void Shell::chooseAddGameArtwork(const QString& file) {
    if(!addGameOpen())return;
    if(file.isEmpty())return;
    addGameDraft_.artwork=QFileInfo(file).absoluteFilePath();
    qInfo().noquote()<<"add game: artwork chosen \""<<QDir::toNativeSeparators(addGameDraft_.artwork)<<"\"";
    refreshAddGamePage();emit changed();
}

void Shell::closeAddGamePage() {
    for(int i=pages_.size()-1;i>=0;--i)if(pages_[i].addGame){pages_.removeAt(i);break;}
}

void Shell::saveAddGame() {
    if(!addGameOpen())return;
    ContentApp app=addGameDraft_;
    if(app.executable.trimmed().isEmpty() || app.title.trimmed().isEmpty()) {
        interactionAudio_.play(InteractionAudio::Cue::Error);
        showMessage(tr("Add a Game"),tr("Choose an executable and give the game a name first."),MessageKind::Page);
        emit changed();return;
    }
    QString error;
    if(!ContentRegistry::upsert(appRegistryPath(),app,error)) {
        qWarning().noquote()<<"add game: save failed:"<<error;
        showMessage(tr("Add a Game"),error,MessageKind::Page);emit changed();return;
    }
    qInfo().noquote()<<"add game: saved \""<<app.title<<"\" to apps.json ("<<QDir::toNativeSeparators(app.executable)<<")";
    closeAddGamePage();
    notify(tr("Add a Game"),tr("%1 added").arg(app.title));
    refreshContent();   // the column rebuilds so the new row appears and launches like any imported game
    emit changed();
}

void Shell::cancelAddGame() {
    if(!addGameOpen())return;
    qInfo().noquote()<<"add game: cancelled (nothing written)";
    closeAddGamePage();
    emit changed();
}

void Shell::activateAddGame(const Entry& entry) {
    switch(entry.action) {
    case Action::AddGame:openAddGamePage();break;
    case Action::AddGameChooseExe:openFilePicker(QStringLiteral("game"));break;
    case Action::AddGameField:
        if(entry.value==QLatin1String("title"))
            emit requestJellyfinFieldEdit(QStringLiteral("addgame:title"),tr("Game Name"),addGameDraft_.title,false);
        else if(entry.value==QLatin1String("artwork"))
            emit requestJellyfinFieldEdit(QStringLiteral("addgame:artwork"),tr("Artwork Path"),addGameDraft_.artwork,false);
        break;
    case Action::AddGameChooseArtwork:openFilePicker(QStringLiteral("artwork"));break;
    case Action::AddGameSave:saveAddGame();break;
    case Action::AddGameCancel:cancelAddGame();break;
    default:break;
    }
}

} // namespace xmb
