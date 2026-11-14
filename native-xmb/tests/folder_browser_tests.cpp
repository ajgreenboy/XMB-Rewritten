#include "folder_browser.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QTemporaryDir>
#include <cstdio>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

// The directory logic of the in-XMB folder browser: drive list, folder-only listing, natural order, hidden
// folders left out, parents (a drive root goes to This PC), unreadable or missing folders, the row cap,
// path elision and the start-folder choice. Only a scratch folder is touched (the first argument, or %TEMP%).
// Build with -DXMB_FOLDER_BROWSER_BUILD_TESTS=ON; exit code 0 = all pass.
namespace fb=xmb::folderbrowser;

static int failures=0;
static void check(bool ok,const char* what) {
    std::printf("%s  %s\n",ok?"PASS":"FAIL",what);
    if(!ok)++failures;
}
static QStringList names(const fb::Listing& l) {QStringList n;for(const auto& i:l.items)n<<i.name;return n;}

int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    QTemporaryDir temp(argc>1?QDir(QString::fromLocal8Bit(argv[1])).filePath(QStringLiteral("xmb-folder-browser-XXXXXX")):QString());
    if(!temp.isValid()){std::printf("FAIL  scratch folder\n");return 1;}
    const QString root=fb::clean(temp.path());
    for(const char* d:{"Disc 10","Disc 2","disc 1","Alpha","beta","Gamma"})QDir().mkpath(root+QLatin1Char('/')+QLatin1String(d));
    QFile file(root+QStringLiteral("/a-file.txt"));if(file.open(QIODevice::WriteOnly)){file.write("x");file.close();}
    QDir().mkpath(root+QStringLiteral("/Hidden One"));
    QDir().mkpath(root+QStringLiteral("/Alpha/Inner"));
#ifdef Q_OS_WIN
    SetFileAttributesW(reinterpret_cast<const wchar_t*>(QDir::toNativeSeparators(root+QStringLiteral("/Hidden One")).utf16()),FILE_ATTRIBUTE_HIDDEN);
#endif

    // drives
    const auto drives=fb::drives();
    check(!drives.isEmpty(),"the drive list is not empty");
    bool drivesOk=true;
    for(const auto& d:drives)drivesOk=drivesOk && d.path.size()==3 && d.path[1]==QLatin1Char(':') && d.path[2]==QLatin1Char('/') && !d.name.isEmpty();
    check(drivesOk,"every drive is a root like C:/ with a name");
    const auto top=fb::list(QString());
    check(top.path.isEmpty() && top.parent.isEmpty() && top.problem.isEmpty() && top.items.size()==drives.size(),"an empty path lists the drives");

    // listing
    const auto l=fb::list(root);
    check(l.problem.isEmpty() && !l.truncated,"a scratch folder lists without a problem");
    check(names(l)==QStringList({"Alpha","beta","disc 1","Disc 2","Disc 10","Gamma"}),"folders only, natural case-blind order, hidden left out");
    check(l.items.size()==6 && l.items[0].path==root+QStringLiteral("/Alpha"),"item paths are absolute with forward slashes");
    check(names(fb::list(root+QStringLiteral("/Alpha")))==QStringList({"Inner"}),"a sub-folder lists its own folders");
    check(fb::list(root+QStringLiteral("/Alpha/Inner")).items.isEmpty() && fb::list(root+QStringLiteral("/Alpha/Inner")).problem.isEmpty(),
        "an empty folder is empty, not a problem");

    // limit
    const auto cut=fb::list(root,3);
    check(cut.items.size()==3 && cut.truncated && cut.items[2].name==QStringLiteral("disc 1"),"a folder over the cap is cut and says so");
    check(!fb::list(root,6).truncated,"exactly the cap is not truncated");

    // problems
    const auto missing=fb::list(root+QStringLiteral("/does-not-exist"));
    check(!missing.problem.isEmpty() && missing.items.isEmpty(),"a missing folder reports a reason");
    const auto notDir=fb::list(root+QStringLiteral("/a-file.txt"));
    check(!notDir.problem.isEmpty() && notDir.items.isEmpty(),"a file is not a folder");

    // an access-denied folder (Windows only): list-folder denied to Everyone on a scratch folder
#ifdef Q_OS_WIN
    {
        const QString locked=root+QStringLiteral("/Locked");
        QDir().mkpath(locked+QStringLiteral("/inside"));
        const QString native=QDir::toNativeSeparators(locked);
        const bool denied=QProcess::execute(QStringLiteral("icacls"),{native,QStringLiteral("/deny"),QStringLiteral("*S-1-1-0:(RD)")})==0;
        if(denied) {
            const auto lockedListing=fb::list(locked);
            check(!lockedListing.problem.isEmpty() && lockedListing.items.isEmpty() && lockedListing.problem.contains(QStringLiteral("allow")),
                "an access-denied folder reports that Windows does not allow it, not an empty list");
            QProcess::execute(QStringLiteral("icacls"),{native,QStringLiteral("/remove:d"),QStringLiteral("*S-1-1-0")});
        } else std::printf("SKIP  access-denied folder (icacls could not set the deny)\n");
    }
#endif

    // parents
    check(fb::parentOf(root+QStringLiteral("/Alpha/Inner"))==root+QStringLiteral("/Alpha"),"parent of a nested folder");
    check(fb::parentOf(QStringLiteral("C:/Users"))==QStringLiteral("C:/"),"parent of a top folder is the drive root");
    check(fb::parentOf(QStringLiteral("C:/"))==QString(),"parent of a drive root is This PC");
    check(fb::parentOf(QStringLiteral("C:"))==QString(),"a bare drive letter is a drive root");
    check(fb::parentOf(QString())==QString(),"This PC has no parent");
    check(fb::clean(QStringLiteral("C:\\Users\\\\Al\\.\\Music\\"))==QStringLiteral("C:/Users/Al/Music"),"clean uses forward slashes and drops dots and trailing slashes");
    check(fb::list(QStringLiteral("D:\\x")).parent==QStringLiteral("D:/"),"the listing carries the parent");

    // elision
    const QString longPath=QStringLiteral("C:/Users/Someone/Music/Collections/A Very Long Folder Name That Goes On And On");
    for(int n:{8,12,20,30,40,56}) {
        const QString e=fb::elidePath(longPath,n);
        check(e.size()<=n,qPrintable(QStringLiteral("elided path fits %1 characters").arg(n)));
    }
    check(fb::elidePath(longPath,56).startsWith(QStringLiteral("C:\\...\\")),"an elided path keeps the drive and an ellipsis");
    check(fb::elidePath(longPath,80)==QDir::toNativeSeparators(longPath),"a path that fits is only given native separators");
    check(fb::elidePath(QStringLiteral("C:/Music"),40)==QStringLiteral("C:\\Music"),"a short path is untouched");
    check(fb::elidePath(longPath,4).size()<=8,"a tiny limit is raised to 8");

    // start folder
    check(fb::startPath(root+QStringLiteral("/Alpha"),QString(),QString())==root+QStringLiteral("/Alpha"),"the override wins");
    check(fb::startPath(QStringLiteral("roots"),root,QString()).isEmpty() && fb::startPath(QStringLiteral("THIS-PC"),root,QString()).isEmpty(),"roots starts at This PC");
    check(fb::startPath(QString(),root+QStringLiteral("/beta"),QString())==root+QStringLiteral("/beta"),"the current library folder is next");
    check(fb::startPath(QString(),root+QStringLiteral("/gone/deeper"),QString())==root,"a vanished folder walks up to the nearest existing one");
    check(fb::startPath(QString(),QString(),root+QStringLiteral("/Gamma"))==root+QStringLiteral("/Gamma"),"the fallback comes last");
    check(fb::startPath(root+QStringLiteral("/nope"),root+QStringLiteral("/beta"),QString())==root,"an override that is gone walks up before the current folder is tried");
    check(fb::startPath(QString(),QString(),QString()).isEmpty(),"nothing set starts at This PC");

    // file mode (Choose Wallpaper, Choose Theme)
    const QStringList images{QStringLiteral("*.png"),QStringLiteral("*.jpg"),QStringLiteral("*.jpeg"),QStringLiteral("*.bmp"),QStringLiteral("*.webp")};
    const QString pics=root+QStringLiteral("/Pics");
    QDir().mkpath(pics+QStringLiteral("/Zeta"));QDir().mkpath(pics+QStringLiteral("/alpha"));
    const char* fileNames[]={"img 10.png","img 2.PNG","Img 1.jpg","c.jpeg","d.bmp","e.webp","notes.txt","theme.p3t","f.png.bak","hidden.png","a.gif"};
    for(const char* n:fileNames){QFile f(pics+QLatin1Char('/')+QLatin1String(n));if(f.open(QIODevice::WriteOnly)){f.write(QByteArray(2500,'x'));f.close();}}
#ifdef Q_OS_WIN
    SetFileAttributesW(reinterpret_cast<const wchar_t*>(QDir::toNativeSeparators(pics+QStringLiteral("/hidden.png")).utf16()),FILE_ATTRIBUTE_HIDDEN);
#endif
    check(fb::matchesFilter(QStringLiteral("A.PNG"),images) && fb::matchesFilter(QStringLiteral("a.Jpeg"),images),"a filter ignores case");
    check(!fb::matchesFilter(QStringLiteral("a.png.bak"),images) && !fb::matchesFilter(QStringLiteral("png"),images) && !fb::matchesFilter(QStringLiteral("a.png"),{}),
        "a filter matches the whole name, and no pattern matches nothing");
    const auto fm=fb::list(pics,2000,images);
    check(fm.problem.isEmpty() && !fm.truncated,"a file-mode listing has no problem");
    check(names(fm)==QStringList({"alpha","Zeta","c.jpeg","d.bmp","e.webp","Img 1.jpg","img 2.PNG","img 10.png"}),
        "file mode: folders first, then matching files, each in natural case-blind order; hidden and non-matching left out");
    check(!fm.items[0].isFile && !fm.items[1].isFile && fm.items[2].isFile,"items say whether they are files");
    check(fm.items[2].path==pics+QStringLiteral("/c.jpeg") && fm.items[2].note==QStringLiteral("2 KB"),"a file item has its absolute path and a size line");
    check(names(fb::list(pics,2000,{QStringLiteral("*.p3t")}))==QStringList({"alpha","Zeta","theme.p3t"}),"a theme filter lists only .p3t files");
    check(names(fb::list(pics))==QStringList({"alpha","Zeta"}),"folder mode still lists no files");
    const auto fcut=fb::list(pics,3,images);
    check(fcut.items.size()==3 && fcut.truncated && fcut.items[2].isFile,"file mode: the cap counts folders and files together, folders first");
    check(!fb::list(pics,8,images).truncated && fb::list(pics,7,images).truncated,"file mode: exactly the cap is not truncated");
    check(fb::list(root+QStringLiteral("/Alpha/Inner"),2000,images).items.isEmpty() && fb::list(root+QStringLiteral("/Alpha/Inner"),2000,images).problem.isEmpty(),
        "file mode: a folder with nothing matching is empty, not a problem");
    check(!fb::list(root+QStringLiteral("/nope"),2000,images).problem.isEmpty(),"file mode: a missing folder reports a reason");
    check(fb::list(QString(),2000,images).items.size()==drives.size() && !fb::list(QString(),2000,images).items[0].isFile,"file mode: the top level is still the drive list");
    check(fb::startPath(QString(),pics+QStringLiteral("/c.jpeg"),QString())==pics,"the start folder of a chosen file is its folder");
    check(fb::startPath(QString(),pics+QStringLiteral("/gone/c.jpeg"),QString())==pics,"the start folder of a vanished file walks up to an existing folder");
    check(fb::fileSizeText(0)==QStringLiteral("0 B") && fb::fileSizeText(1023)==QStringLiteral("1023 B") && fb::fileSizeText(1024)==QStringLiteral("1 KB")
        && fb::fileSizeText(3*1024*1024+400000)==QStringLiteral("3.4 MB"),"file sizes read as B, KB or MB");

    std::printf("%s (%d failure%s)\n",failures?"FAILED":"ALL PASS",failures,failures==1?"":"s");
    return failures?1:0;
}
