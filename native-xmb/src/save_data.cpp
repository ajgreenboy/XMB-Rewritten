#include "save_data.h"
#include "assets.h"
#include "shell.h"
#ifdef XMB_USE_RUST_EDGE
#include "image_decode_rust.h"
#endif
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <algorithm>

namespace xmb {
namespace {
struct Utility { const char* kind; const char* title; const char* fallback; const char* texture; const char* container; };
// category_game.xml #seg_mcutility (305-319), #seg_sdps2 (321-338), #seg_sdps3
// (340-357), #seg_sdpsp (371-388). PS1 saves belong to Memory Card Utility, not a fifth row.
constexpr Utility utilities[]{
    {"memory","msg_memory_card_utility","Memory Card Utility","item_tex_ps12util","explore_plugin_full"},
    {"ps2","msg_savedata_utility_ps2","Saved Data Utility (PS2)","item_tex_ps3util","explore_plugin_full"},
    {"ps3","msg_savedata_utility_ps3","Saved Data Utility (PS3)","item_tex_ps3util","explore_plugin_full"},
    {"psp","msg_savedata_utility_minis_psp","Saved Data Utility (minis/PSP)","item_tex_psputil","explore_category_game"}};
const Utility& utility(const QString& kind) {
    for(const auto& spec:utilities)if(kind==QLatin1String(spec.kind))return spec;
    return utilities[2];
}
QHash<QString,QString> parseSaveSfo(const QString& path) {
    QFile file(path);
    QHash<QString,QString> values;
    if(!file.open(QIODevice::ReadOnly))return values;
    const QByteArray d=file.readAll();
#ifdef XMB_USE_RUST_EDGE
    image_rust::parseSfo(d,values);
    return values;
#else
    if(d.size()<20 || !d.startsWith(QByteArray("\0PSF",4)))return values;
    const auto u16=[&](qsizetype at){return at+2<=d.size()?quint32(quint8(d[at])|quint8(d[at+1])<<8):0u;};
    const auto u32=[&](qsizetype at){return at+4<=d.size()?quint32(u16(at)|u16(at+2)<<16):0u;};
    const quint32 keys=u32(8),data=u32(12),count=std::min<quint32>(u32(16),256);
    for(quint32 i=0;i<count;++i) {
        const qsizetype e=20+qsizetype(i)*16;
        const quint32 keyAt=keys+u16(e),format=u16(e+2),length=u32(e+4),dataAt=data+u32(e+12);
        if(keyAt>=quint32(d.size()) || dataAt+length>quint32(d.size()))continue;
        const QString key=QString::fromLatin1(d.constData()+keyAt);
        if(format==0x0204 || format==0x0004)values.insert(key,QString::fromUtf8(d.constData()+dataAt,int(length)).remove(QChar(0)));
        else if(format==0x0404)values.insert(key,QString::number(u32(dataAt)));
    }
    return values;
#endif
}
QString formatSaveBytes(qint64 bytes) {
    if(bytes>=1024ll*1024)return QStringLiteral("%1 MB").arg(double(bytes)/(1024.*1024.),0,'f',1);
    if(bytes>=1024)return QStringLiteral("%1 KB").arg((bytes+1023)/1024);
    return QStringLiteral("%1 B").arg(std::max<qint64>(0,bytes));
}
}
QString saveDataTitle(Assets& assets,const QString& kind) {
    const auto& spec=utility(kind);
    return assets.text("explore_category_game",spec.title,QString::fromUtf8(spec.fallback));
}
QVector<Entry> saveDataRows(Assets& assets,const QString& kind) {
    const auto& spec=utility(kind);
    const QString plugin=kind==QLatin1String("memory")?QStringLiteral("vmc_savedata_plugin"):QStringLiteral("ps3_savedata_plugin");
    const QString empty=assets.text(plugin,"msg_error_no_savedata",
        assets.text("explore_category_game","msg_error_no_savedata","There are no saved data items."));
    const QString reason=QObject::tr("No save data source is connected to this native recreation. "
        "PS3, PS2, PSP and internal memory card storage are not implemented. "
        "No saved data can be copied, deleted or inspected on this system.");
    QVector<Entry> rows;
    if(kind==QLatin1String("ps3") && !qEnvironmentVariableIsSet("XMB_DUMP_ENTRIES")) {
        const QDir homeDir(QDir(assets.hddRoot()).filePath(QStringLiteral("home")));
        if(homeDir.exists()) {
            const auto users=homeDir.entryInfoList(QDir::Dirs|QDir::NoDotAndDotDot,QDir::Name);
            for(const auto& userInfo:users) {
                const QDir saveRoot(QDir(userInfo.absoluteFilePath()).filePath(QStringLiteral("savedata")));
                if(!saveRoot.exists())continue;
                const auto saves=saveRoot.entryInfoList(QDir::Dirs|QDir::NoDotAndDotDot,QDir::Time);
                for(const auto& saveInfo:saves) {
                    if(rows.size()>=128)break;
                    const QDir dir(saveInfo.absoluteFilePath());
                    const QString sfoPath=dir.filePath(QStringLiteral("PARAM.SFO"));
                    if(!QFileInfo::exists(sfoPath))continue;
                    const auto sfo=parseSaveSfo(sfoPath);
                    if(sfo.isEmpty())continue;
                    const QString title=sfo.value(QStringLiteral("TITLE"),saveInfo.fileName()).trimmed();
                    const QString subTitle=sfo.value(QStringLiteral("SUB_TITLE")).trimmed();
                    const QString detailText=sfo.value(QStringLiteral("DETAIL")).trimmed();
                    qint64 totalBytes=0;
                    QDateTime latestModified=saveInfo.lastModified();
                    for(const auto& f:dir.entryInfoList(QDir::Files|QDir::NoDotAndDotDot)) {
                        totalBytes+=f.size();
                        if(f.lastModified().isValid() && (!latestModified.isValid() || f.lastModified()>latestModified))
                            latestModified=f.lastModified();
                    }
                    Entry row;
                    row.id=QStringLiteral("savedata:ps3:dir:")+saveInfo.fileName();
                    row.title=title.isEmpty()?saveInfo.fileName():title;
                    row.detail=subTitle.isEmpty()?formatSaveBytes(totalBytes):subTitle+QStringLiteral("  \u00B7  ")+formatSaveBytes(totalBytes);
                    row.container=QString::fromLatin1(spec.container);
                    row.texture=QString::fromLatin1(spec.texture);
                    row.action=Action::Information;
                    row.source=QStringLiteral("hdd");
                    const QString icon0=dir.filePath(QStringLiteral("ICON0.PNG"));
                    if(QFileInfo::exists(icon0)) {
                        QImage img(icon0);
                        if(!img.isNull()){row.artwork=std::move(img);row.gameTile=true;}
                    }
                    QStringList infoRows;
                    infoRows<<QObject::tr("Title: %1").arg(row.title);
                    if(!subTitle.isEmpty())infoRows<<QObject::tr("Subtitle: %1").arg(subTitle);
                    if(!detailText.isEmpty())infoRows<<QObject::tr("Details: %1").arg(detailText);
                    if(latestModified.isValid())infoRows<<QObject::tr("Updated: %1").arg(latestModified.toString(QStringLiteral("yyyy/MM/dd HH:mm")));
                    infoRows<<QObject::tr("Size: %1").arg(formatSaveBytes(totalBytes));
                    infoRows<<QObject::tr("User: %1").arg(userInfo.fileName());
                    infoRows<<QObject::tr("Directory: %1").arg(saveInfo.fileName());
                    infoRows<<QStringLiteral("Path: ")+QDir::toNativeSeparators(saveInfo.absoluteFilePath());
                    row.value=row.title+QStringLiteral("\n\n")+infoRows.join('\n');
                    rows.push_back(std::move(row));
                }
            }
        }
        if(!rows.isEmpty())return rows;
    }
    const auto add=[&](const QString& id,const QString& title,const QString& detail) {
        Entry row;
        row.id=QStringLiteral("savedata:")+kind+QLatin1Char(':')+id;
        row.title=title;row.detail=detail;
        row.container=QString::fromLatin1(spec.container);row.texture=QString::fromLatin1(spec.texture);
        row.action=Action::Unavailable;row.disabled=true;row.source=QStringLiteral("rco");
        rows.push_back(std::move(row));
    };
    // No fabricated ICON0 tiles, USB devices, cloud storage, counts or metadata. The first row is an
    // empty-state notice, not a save. Commands remain reachable only to explain the absent backend.
    // The firmware's list is provider-built; these command rows are a native empty-state affordance,
    // not a claim to reproduce the unobserved runtime list's exact geometry.
    add("empty",empty,reason);
    add("copy",assets.text("ps3_savedata_plugin","msg_copy","Copy"),reason);
    add("delete",assets.text("ps3_savedata_plugin","msg_del","Delete"),reason);
    add("information",assets.text("explore_category_game","msg_info","Information"),reason);
    return rows;
}
void Shell::openSaveData(const QString& kind) {
    Page page;page.title=saveDataTitle(assets_,kind);
    page.entries=saveDataRows(assets_,kind);
    pages_.push_back(std::move(page));
    emit changed();
}
}
