#include "shell.h"
#include "rawg_client.h"
#include <QDir>
#include <QSet>

namespace xmb {
namespace {
Entry nativeGameRow(const QString& id,const QString& title,Action action) {
    Entry entry;entry.id=id;entry.title=title;entry.action=action;entry.container=QStringLiteral("explore_plugin_full");
    entry.texture=QStringLiteral("game_tex_default_ps3");entry.source=QStringLiteral("native");return entry;
}
QString nativeOptionCount(const GameResult& game) {
    QSet<QString> sources;for(const auto& option:game.options)sources.insert(option.source);
    if(sources.size()>1)return QObject::tr("%n source(s)",nullptr,int(sources.size()));
    return sources.isEmpty()?QObject::tr("No download sources"):*sources.constBegin();
}
}

void Shell::openGameDetails(const GameResult& game,const QImage& artwork) {
    if(pages_.size()>=32)return;
    Page page;page.title=game.name;page.gameDetails=true;page.identity=++pageIdentityCounter_;page.downloadResults={game};
    Entry download=nativeGameRow(QStringLiteral("game:detail:download"),tr("Download"),Action::GameDetailsDownload);
    download.detail=nativeOptionCount(game);download.gameTile=true;download.artwork=artwork;download.artworkUrl=game.coverUrl;page.entries.append(download);
    page.entries.append(nativeGameRow(QStringLiteral("game:detail:description"),tr("Description"),Action::Unavailable));
    page.entries.append(nativeGameRow(QStringLiteral("game:detail:genre"),tr("Genre"),Action::Unavailable));
    page.entries.append(nativeGameRow(QStringLiteral("game:detail:year"),tr("Release Year"),Action::Unavailable));
    page.entries.append(nativeGameRow(QStringLiteral("game:detail:developer"),tr("Developer"),Action::Unavailable));
    page.entries.append(nativeGameRow(QStringLiteral("game:detail:publisher"),tr("Publisher"),Action::Unavailable));
    page.entries.append(nativeGameRow(QStringLiteral("game:detail:platform"),tr("Platform"),Action::Unavailable));
    pages_.push_back(std::move(page));requestGameDetails(game.name);emit changed();
}

void Shell::requestGameDetails(const QString& title) {
    gameDetailDepth_=pages_.size(); gameDetailTitle_=title;
    rawgDetailPageIdentity_=pages_.isEmpty()?0:pages_.last().identity;
    const quint64 request=++rawgDetailRequest_;
    if(!RawgClient::hasApiKey())return;
    rawgDetails_.reset(new RawgClient(QDir(dataDirectory_).filePath(QStringLiteral("cache/release-art")),this));
    connect(rawgDetails_.get(),&RawgClient::searchReady,this,[this,request](const RawgGameList& games) {
        if(request!=rawgDetailRequest_ || pages_.isEmpty() || !pages_.last().gameDetails || pages_.last().identity!=rawgDetailPageIdentity_ || pages_.size()!=gameDetailDepth_)return;
        const int id=resolveRawgGameId(games,gameDetailTitle_); if(id>0)rawgDetails_->fetchDetails(id);
    });
    connect(rawgDetails_.get(),&RawgClient::detailsReady,this,[this,request](const RawgGameDetails& details) {
        if(request!=rawgDetailRequest_ || pages_.isEmpty() || !pages_.last().gameDetails || pages_.last().identity!=rawgDetailPageIdentity_ || pages_.size()!=gameDetailDepth_)return;
        if(pages_.last().title!=gameDetailTitle_)return;
        pages_.last().gameDetailsData=details;
        const auto set=[&](const QString& id,const QString& value){for(auto& row:pages_.last().entries)if(row.id==id){row.detail=value;row.disabled=value.isEmpty();}};
        set(QStringLiteral("game:detail:description"),details.description.isEmpty()?tr("Not available"):details.description);
        set(QStringLiteral("game:detail:genre"),details.genres.isEmpty()?tr("Not available"):details.genres.join(QStringLiteral(", ")));
        set(QStringLiteral("game:detail:year"),details.releaseYear?QString::number(details.releaseYear):tr("Not available"));
        set(QStringLiteral("game:detail:developer"),details.developers.isEmpty()?tr("Not available"):details.developers.join(QStringLiteral(", ")));
        set(QStringLiteral("game:detail:publisher"),details.publishers.isEmpty()?tr("Not available"):details.publishers.join(QStringLiteral(", ")));
        set(QStringLiteral("game:detail:platform"),details.platforms.isEmpty()?tr("Not available"):details.platforms.join(QStringLiteral(", ")));
        emit changed();
    });
    rawgDetails_->search(title);
}
}
