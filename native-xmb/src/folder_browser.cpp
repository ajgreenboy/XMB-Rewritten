#include "folder_browser.h"
#include <QCollator>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStorageInfo>
#include <algorithm>
#include <filesystem>
#include <system_error>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace xmb::folderbrowser {
namespace {
bool isDriveRoot(const QString& path) {
    return path.size()==3 && path[1]==QLatin1Char(':') && path[2]==QLatin1Char('/');
}
QString sizeText(qint64 bytes) {
    const double gb=double(bytes)/(1024.0*1024.0*1024.0);
    if(gb>=1000.0)return QString::number(gb/1024.0,'f',1)+QStringLiteral(" TB");
    if(gb>=10.0)return QString::number(qint64(gb))+QStringLiteral(" GB");
    return QString::number(gb,'f',1)+QStringLiteral(" GB");
}
// "System" attribute folders ($Recycle.Bin, System Volume Information, Documents and Settings) are not media folders.
bool isSystemFolder(const QString& path) {
#ifdef Q_OS_WIN
    const DWORD attributes=GetFileAttributesW(reinterpret_cast<const wchar_t*>(QDir::toNativeSeparators(path).utf16()));
    return attributes!=INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_SYSTEM)!=0;
#else
    Q_UNUSED(path);return false;
#endif
}
// Opens the folder the way a listing would, so access-denied is told apart from empty.
QString openProblem(const QString& path) {
    std::error_code ec;
    std::filesystem::directory_iterator it(std::filesystem::path(path.toStdWString()),ec);
    if(!ec)return {};
    if(ec==std::errc::permission_denied)return QStringLiteral("Windows does not allow this application to open this folder.");
    if(ec==std::errc::no_such_file_or_directory || ec==std::errc::not_a_directory)return QStringLiteral("This folder no longer exists.");
    return QStringLiteral("This folder could not be read.");
}
}

QString clean(const QString& path) {
    if(path.trimmed().isEmpty())return {};
    QString c=QDir::cleanPath(QDir::fromNativeSeparators(path.trimmed()));
    if(c.size()==2 && c[1]==QLatin1Char(':'))c+=QLatin1Char('/');
    return c;
}

QVector<Item> drives() {
    QVector<Item> out;
    for(const QFileInfo& info:QDir::drives()) {
        const QString root=clean(info.absoluteFilePath());
        if(!isDriveRoot(root))continue;
        const QStorageInfo storage(root);
        const QString label=storage.isValid()?storage.name():QString();
        const QString letter=root.left(2);
        Item item;item.path=root;
        item.name=label.isEmpty()?QStringLiteral("Local Disk (%1)").arg(letter):QStringLiteral("%1 (%2)").arg(label,letter);
        if(storage.isValid() && storage.isReady() && storage.bytesTotal()>0)
            item.note=QStringLiteral("%1 free of %2").arg(sizeText(storage.bytesAvailable()),sizeText(storage.bytesTotal()));
        out.push_back(std::move(item));
    }
    return out;
}

QString parentOf(const QString& path) {
    const QString c=clean(path);
    if(c.isEmpty() || isDriveRoot(c) || c==QLatin1String("/"))return {};
    const int slash=c.lastIndexOf(QLatin1Char('/'));
    if(slash<0)return {};
    return clean(c.left(slash).size()==2?c.left(slash)+QLatin1Char('/'):c.left(slash));
}

bool matchesFilter(const QString& fileName,const QStringList& patterns) {
    for(const QString& pattern:patterns) {
        const QRegularExpression re(QRegularExpression::wildcardToRegularExpression(pattern.trimmed()),QRegularExpression::CaseInsensitiveOption);
        if(re.isValid() && re.match(fileName).hasMatch())return true;
    }
    return false;
}

QString fileSizeText(qint64 bytes) {
    if(bytes<0)bytes=0;
    if(bytes<1024)return QStringLiteral("%1 B").arg(bytes);
    if(bytes<1024*1024)return QStringLiteral("%1 KB").arg((bytes+512)/1024);
    if(bytes<1024LL*1024*1024)return QStringLiteral("%1 MB").arg(double(bytes)/(1024.0*1024.0),0,'f',1);
    return QStringLiteral("%1 GB").arg(double(bytes)/(1024.0*1024.0*1024.0),0,'f',1);
}

Listing list(const QString& path,int limit,const QStringList& filePatterns) {
    Listing listing;listing.path=clean(path);
    if(listing.path.isEmpty()){listing.items=drives();return listing;}
    listing.parent=parentOf(listing.path);
    listing.problem=openProblem(listing.path);
    if(!listing.problem.isEmpty())return listing;
    const QFileInfoList folders=QDir(listing.path).entryInfoList(QDir::Dirs|QDir::NoDotAndDotDot|QDir::NoSymLinks);
    QVector<Item> items;
    items.reserve(int(folders.size()));
    for(const QFileInfo& f:folders) {
        if(f.isHidden() || isSystemFolder(f.absoluteFilePath()))continue;
        Item item;item.name=f.fileName();item.path=clean(f.absoluteFilePath());
        items.push_back(std::move(item));
    }
    QCollator collator;collator.setNumericMode(true);collator.setCaseSensitivity(Qt::CaseInsensitive);
    std::sort(items.begin(),items.end(),[&collator](const Item& a,const Item& b){
        const int r=collator.compare(a.name,b.name);return r!=0?r<0:a.name<b.name;});
    if(!filePatterns.isEmpty()) {
        QVector<Item> files;
        for(const QFileInfo& f:QDir(listing.path).entryInfoList(QDir::Files|QDir::NoDotAndDotDot|QDir::NoSymLinks)) {
            if(f.isHidden() || !matchesFilter(f.fileName(),filePatterns))continue;
            const QString p=clean(f.absoluteFilePath());
            if(isSystemFolder(p))continue;
            Item item;item.name=f.fileName();item.path=p;item.isFile=true;item.note=fileSizeText(f.size());
            files.push_back(std::move(item));
        }
        std::sort(files.begin(),files.end(),[&collator](const Item& a,const Item& b){
            const int r=collator.compare(a.name,b.name);return r!=0?r<0:a.name<b.name;});
        for(auto& file:files)items.push_back(std::move(file));
    }
    if(limit>0 && items.size()>limit){items.resize(limit);listing.truncated=true;}
    listing.items=std::move(items);
    return listing;
}

QString elidePath(const QString& path,int maxChars) {
    maxChars=std::max(maxChars,8);
    const QString p=QDir::toNativeSeparators(path);
    if(p.size()<=maxChars)return p;
    const QString dots=QStringLiteral("...");
    const QChar sep=QDir::separator();
    const int firstSep=p.indexOf(sep),lastSep=p.lastIndexOf(sep);
    const QString lead=(firstSep>=0?p.left(firstSep+1):QString())+dots;   // "C:\..."
    QString tail=lastSep>firstSep?p.mid(lastSep):p.mid(firstSep>=0?firstSep:0);
    const int room=maxChars-int(lead.size());
    if(room<4)return dots+p.right(maxChars-3);
    if(tail.size()>room) {   // the last name alone is too long: keep the drive and the end of the name
        const QString head=firstSep>=0?p.left(firstSep+1):QString();
        return head+dots+p.right(maxChars-int(head.size())-3);
    }
    return lead+tail;
}

QString startPath(const QString& overridePath,const QString& current,const QString& fallback) {
    const QString o=overridePath.trimmed();
    if(o.compare(QStringLiteral("roots"),Qt::CaseInsensitive)==0 || o.compare(QStringLiteral("this-pc"),Qt::CaseInsensitive)==0)return {};
    const QStringList candidates{o,current.trimmed(),fallback.trimmed()};
    for(const QString& candidate:candidates) {
        QString c=clean(candidate);
        while(!c.isEmpty() && !QFileInfo(c).isDir())c=parentOf(c);
        if(!c.isEmpty())return c;
    }
    return {};
}

}
