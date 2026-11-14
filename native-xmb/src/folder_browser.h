#pragma once
// The directory logic behind the in-XMB folder browser (Choose Media Folder, and the Video, Music and Photo
// Library rows while no folder is set). It replaces the native Windows folder dialog, which a pad cannot drive.
// Nothing here draws or knows about the Shell: it lists drives and sub-folders, finds the parent of a folder
// and shortens long names, so the rules can be tested on their own (tests/folder_browser_tests.cpp).
//
// Rules:
//   - the top level ("This PC") is the list of drive roots; an empty path means that level
//   - only folders are listed; hidden and system folders are left out; names sort in natural, case-blind
//     order ("Disc 2" before "Disc 10")
//   - a folder that cannot be opened reports a reason instead of an empty list
//   - a very large folder is cut at `limit` rows and says so
#include <QString>
#include <QStringList>
#include <QVector>

namespace xmb::folderbrowser {

struct Item {
    QString name;   // the folder's own name (a drive's label for a root)
    QString path;   // absolute, forward slashes, no trailing slash except on a drive root ("C:/")
    QString note;   // a drive's free-space line, or a file's size; empty for a folder
    bool isFile=false;   // file mode only: a file that matched the filter
};

struct Listing {
    QString path;        // what was listed ("" = This PC)
    QString parent;      // where Circle goes: "" for This PC and for a drive root
    QVector<Item> items;
    QString problem;     // non-empty = the folder could not be read; items is empty
    bool truncated=false;
};

// Drive roots of this PC with their labels ("Local Disk (C:)") and free space.
QVector<Item> drives();

// Lists one level. An empty `path` is the drive list. Never throws and never blocks on a missing folder:
// an unreadable or vanished folder fills `problem` with a one-line reason.
// File mode (Choose Wallpaper, Choose Theme): a non-empty `filePatterns` ("*.png", case-blind) also lists the files
// that match, after the folders; hidden and system files are left out like folders, each group sorts naturally, and
// `limit` counts folders and files together (folders come first, so a cut drops files).
Listing list(const QString& path,int limit=2000,const QStringList& filePatterns={});

// True when `fileName` matches one of the wildcard `patterns` ("*.png"), ignoring case. No pattern matches nothing.
bool matchesFilter(const QString& fileName,const QStringList& patterns);

// "812 KB", "3.4 MB": the size line of a file row.
QString fileSizeText(qint64 bytes);

// The folder above `path`: "" when `path` is a drive root (or empty), so Circle at a drive root goes to This PC.
QString parentOf(const QString& path);

// Normalised form used for every path this module returns (forward slashes, drive roots keep one slash).
QString clean(const QString& path);

// `path` (shown with native separators) shortened to at most `maxChars`, keeping the drive and the end of the
// last name: "C:\...\Very Long Folder Name". Never longer than maxChars; maxChars below 8 is raised to 8.
QString elidePath(const QString& path,int maxChars);

// Where the browser opens: `overridePath` (the test-only XMB_FOLDER_START value; "roots" or "this-pc" means the
// drive list) when it names a folder; otherwise `current` (the folder already chosen) when it exists; otherwise
// `fallback`; each is walked up to its nearest existing parent. Nothing usable gives "" (the drive list).
QString startPath(const QString& overridePath,const QString& current,const QString& fallback);

}
