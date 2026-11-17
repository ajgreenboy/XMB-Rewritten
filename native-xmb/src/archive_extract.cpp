#include "archive_extract.h"
#include <QDir>
#include <QFileInfo>

namespace xmb {
QString findGameExecutable(const QString& folder, QString* error) {
    QList<QDir> queue{QDir(folder)};
    while(!queue.isEmpty()) {
        QDir dir=queue.takeFirst();
        const auto entries=dir.entryInfoList(QDir::Files|QDir::Dirs|QDir::NoDotAndDotDot|QDir::NoSymLinks,QDir::Name);
        for(const auto& entry:entries) {
            if(entry.isDir()) { queue.append(QDir(entry.absoluteFilePath())); continue; }
            if(entry.suffix().compare(QStringLiteral("exe"),Qt::CaseInsensitive)!=0)continue;
            const QString base=entry.completeBaseName().toLower();
            if(base.contains(QStringLiteral("unins")) || base.contains(QStringLiteral("setup")) ||
               base.contains(QStringLiteral("install")))continue;
            return entry.absoluteFilePath();
        }
    }
    if(error)*error=QStringLiteral("No executable found in the extracted folder");
    return {};
}
} // namespace xmb
