#include "archive_extract.h"
#include <QDir>
#include <QFileInfo>

namespace xmb {
QString findGameExecutable(const QString& folder, QString* error) {
    const QStringList candidates=findGameExecutables(folder);
    if(candidates.size()==1)return candidates.first();
    if(error)*error=candidates.isEmpty()?QStringLiteral("No executable found in the extracted folder"):
        QStringLiteral("More than one plausible game executable was found; choose one explicitly.");
    return {};
}
QStringList findGameExecutables(const QString& folder) {
    QStringList result;
    QList<QDir> queue{QDir(folder)};
    while(!queue.isEmpty()) {
        QDir dir=queue.takeFirst();
        const auto entries=dir.entryInfoList(QDir::Files|QDir::Dirs|QDir::NoDotAndDotDot|QDir::NoSymLinks,QDir::Name);
        for(const auto& entry:entries) {
            if(entry.isDir()) { queue.append(QDir(entry.absoluteFilePath())); continue; }
            if(entry.suffix().compare(QStringLiteral("exe"),Qt::CaseInsensitive)!=0)continue;
            const QString base=entry.completeBaseName().toLower();
            if(base.contains(QStringLiteral("unins")) || base.contains(QStringLiteral("setup")) ||
               base.contains(QStringLiteral("install")) || base.contains(QStringLiteral("redist")) ||
               base.contains(QStringLiteral("helper")) || base==QStringLiteral("vc_redist"))continue;
            result.append(entry.absoluteFilePath());
        }
    }
    return result;
}
} // namespace xmb
