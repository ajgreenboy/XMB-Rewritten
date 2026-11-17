// SteamCMD inside the XMB (audit/STEAMCMD.md).
//
//   Users > Steam Account > SteamCMD      the settings page: where steamcmd.exe is, the account it is signed in as,
//                                         the Steam library installs go to, and the actions that change them
//   Sign In                               User Name, Password (masked, paste works) and then whatever SteamCMD asks
//                                         for: a Steam Guard code on the keyboard, or "approve on your phone"
//   Install/Update/Validate with SteamCMD the item menu of a Steam title, and Cross on a title that is not installed
//   Download Management                   one row per SteamCMD job, with SteamCMD's own percentage and byte counts
//
// The password and the Steam Guard code are held in steamcmd::Secret (zeroed on destruction) from the moment the
// keyboard accepts them until steamcmd::Run writes them to the child's stdin. They are not settings, not log text,
// not command-line words and not environment variables; the account NAME is the only account fact that is stored.
#include "shell.h"
#include "diagnostics.h"
#include "setup.h"
#include "steamcmd.h"
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTimer>
#include <algorithm>
#include <iterator>

namespace xmb {

struct SteamCmdUi {
    // sign-in
    QString username;
    steamcmd::Secret password;
    int passwordChars = 0;
    enum class Sign { Idle, Working, NeedCode, Mobile, Failed } sign = Sign::Idle;
    QString signMessage;
    QString codePrompt;
    steamcmd::Run* signRun = nullptr;
    // jobs: SteamCMD runs one at a time; the rest wait
    struct Job {
        quint64 id = 0;
        QString appId, title, folder, library;
        int kind = 0;                                   // 0 install, 1 update, 2 validate
        enum class State { Queued, Running, Done, Failed, Cancelled, Interrupted } state = State::Queued;
        steamcmd::Progress progress;
        QString message;
        bool needsSignIn = false;
    };
    QList<Job> jobs;
    quint64 nextJob = 1;
    steamcmd::Run* jobRun = nullptr;
    quint64 runningJob = 0;
    QElapsedTimer lastJobPaint;
    // downloading SteamCMD itself
    QNetworkReply* reply = nullptr;
    QByteArray zip;
    QProcess* firstRun = nullptr;
    QString setupStatus;
    // the folder chooser
    QString browsePath;

    void loadBacklog(const QString& dataDirectory);
    bool saveBacklog(const QString& dataDirectory) const;
};

namespace {
constexpr const char* kPathKey = "steamcmd/path";
constexpr const char* kAccountKey = "steamcmd/account";
constexpr const char* kLibraryKey = "steamcmd/library";
constexpr qsizetype MaxJobs = 32;
constexpr qint64 MaxBacklogBytes = 512 * 1024;

QString backlogPath(const QString& dataDirectory) {
    return QDir(dataDirectory).filePath(QStringLiteral("steamcmd-jobs.json"));
}

bool isJobFolder(const QString& library, const QString& folder) {
    if (!QDir::isAbsolutePath(library) || !QDir::isAbsolutePath(folder) ||
        library.contains(QChar::Null) || folder.contains(QChar::Null)) return false;
    const QString common = QDir(library).filePath(QStringLiteral("steamapps/common"));
    const QFileInfo info(QDir::cleanPath(folder));
    return !info.fileName().isEmpty() && QDir::cleanPath(info.path()).compare(QDir::cleanPath(common), Qt::CaseInsensitive) == 0;
}

Entry makeRow(const QString& id, const QString& title, const QString& detail, const QString& value = {}) {
    Entry e;
    e.id = id; e.title = title; e.detail = detail;
    e.action = value.isEmpty() ? Action::Information : Action::SteamCmd;
    // An Information row with no text would open System Information; its own sentence is shown instead.
    e.value = value.isEmpty() ? (detail.isEmpty() ? title : detail) : value;
    e.container = QStringLiteral("explore_plugin_full");
    e.texture = QStringLiteral("item_tex_plain_folder");
    e.source = QStringLiteral("native");
    return e;
}
Entry fieldRow(const QString& id, const QString& title, const QString& detail, const QString& value, const QString& shown) {
    Entry e = makeRow(id, title, detail, value);
    e.container = QStringLiteral("qgl/icontex");
    e.texture = QStringLiteral("23n");
    e.fieldValue = shown;
    return e;
}
// A row that only informs: Cross opens the text as an information page ("Title", blank line, "Key: value" rows).
Entry infoRow(const QString& id, const QString& title, const QString& detail, const QString& text) {
    Entry e = makeRow(id, title, detail);
    e.action = Action::Information;
    e.value = title + QStringLiteral("\n\n") + text;
    return e;
}
QString sizeText(qint64 bytes) {
    static const char* const units[] = {"B", "KB", "MB", "GB", "TB"};
    double v = double(std::max<qint64>(0, bytes));
    int u = 0;
    while (v >= 1024.0 && u < 4) { v /= 1024.0; ++u; }
    return u == 0 ? QStringLiteral("%1 B").arg(qint64(v)) : QStringLiteral("%1 %2").arg(QString::number(v, 'f', u >= 2 ? 1 : 0), QString::fromLatin1(units[u]));
}
QString nativePath(const QString& p) { return QDir::toNativeSeparators(p); }
QString kindWord(int kind) { return kind == 0 ? QStringLiteral("Installing") : kind == 1 ? QStringLiteral("Updating") : QStringLiteral("Validating"); }
QString capitalised(QString s) { if (!s.isEmpty()) s[0] = s[0].toUpper(); return s; }
}   // namespace

void SteamCmdUi::loadBacklog(const QString& dataDirectory) {
    QFile file(backlogPath(dataDirectory));
    if (!file.open(QIODevice::ReadOnly) || file.size() > MaxBacklogBytes) return;
    const QJsonObject saved = QJsonDocument::fromJson(file.read(MaxBacklogBytes)).object();
    if (saved.value(QStringLiteral("version")).toInt() != 1) return;
    const auto entries = saved.value(QStringLiteral("jobs")).toArray();
    for (const auto& value : entries) {
        if (jobs.size() >= MaxJobs) break;
        const auto entry = value.toObject();
        Job job;
        job.appId = entry.value(QStringLiteral("appId")).toString();
        job.title = entry.value(QStringLiteral("title")).toString().left(256);
        job.library = entry.value(QStringLiteral("library")).toString();
        job.folder = entry.value(QStringLiteral("folder")).toString();
        job.kind = entry.value(QStringLiteral("kind")).toInt(-1);
        if (!steamcmd::validAppId(job.appId) || job.title.isEmpty() || job.kind < 0 || job.kind > 2 ||
            job.library.size() > 4096 || job.folder.size() > 4096 || !isJobFolder(job.library, job.folder)) continue;
        if (std::any_of(jobs.cbegin(), jobs.cend(), [&job](const Job& other) { return other.appId == job.appId; })) continue;
        job.id = nextJob++;
        job.state = Job::State::Interrupted;
        jobs.push_back(std::move(job));
    }
}

bool SteamCmdUi::saveBacklog(const QString& dataDirectory) const {
    QJsonArray entries;
    for (const auto& job : jobs) {
        if (job.state == Job::State::Done) continue;
        entries.append(QJsonObject{
            {QStringLiteral("appId"), job.appId}, {QStringLiteral("title"), job.title},
            {QStringLiteral("library"), job.library}, {QStringLiteral("folder"), job.folder},
            {QStringLiteral("kind"), job.kind},
        });
    }
    QSaveFile file(backlogPath(dataDirectory));
    const auto bytes = QJsonDocument(QJsonObject{{QStringLiteral("version"), 1}, {QStringLiteral("jobs"), entries}})
                           .toJson(QJsonDocument::Compact);
    if (bytes.size() <= MaxBacklogBytes && file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit())
        return true;
    qWarning("steamcmd: could not save job backlog");
    return false;
}

SteamCmdUi& Shell::steamCmdState() {
    if (!steamCmd_) {
        steamCmd_ = std::make_shared<SteamCmdUi>();
        steamCmd_->loadBacklog(dataDirectory_);
    }
    return *steamCmd_;
}

bool Shell::steamCmdReady() const {
    const QString exe = steamcmd::resolveExecutable(preferences_.value(kPathKey).toString());
    return !exe.isEmpty() && steamcmd::validAccountName(preferences_.value(kAccountKey).toString());
}

// ---- pages ---------------------------------------------------------------------------------------------------
namespace {
QString libraryChoice(const QString& stored, const QStringList& libraries) {
    for (const QString& l : libraries) if (l.compare(stored, Qt::CaseInsensitive) == 0) return l;
    return libraries.value(0);
}
}

static QVector<Entry> mainRows(const SteamCmdUi& ui, const QString& exe, const QString& account,
                               const QStringList& libraries, const QString& chosenLibrary, bool signedInBusy) {
    QVector<Entry> rows;
    const bool have = !exe.isEmpty();
    rows.push_back(infoRow(QStringLiteral("steamcmd:status"), QObject::tr("SteamCMD"),
        have ? nativePath(exe) : QObject::tr("Not set up"),
        have ? QObject::tr("Location: %1\nAccount: %2").arg(nativePath(exe), account.isEmpty() ? QObject::tr("not signed in") : account)
             : QObject::tr("SteamCMD is Valve's command-line Steam client. With it the XMB installs, updates and checks Steam games itself instead of handing each one to the Steam window.\n\nChoose its folder, let this program look for it, or download it from Valve.")));
    if (!ui.setupStatus.isEmpty()) {
        auto e = makeRow(QStringLiteral("steamcmd:busy"), QObject::tr("Setting Up SteamCMD"), ui.setupStatus);
        rows.push_back(std::move(e));
    }
    rows.push_back(makeRow(QStringLiteral("steamcmd:browse"), QObject::tr("Choose SteamCMD Folder"),
        QObject::tr("Browse to the folder that holds steamcmd.exe."), QStringLiteral("browse")));
    rows.push_back(makeRow(QStringLiteral("steamcmd:find"), QObject::tr("Look for SteamCMD"),
        QObject::tr("Looks in the usual folders for steamcmd.exe. Nothing is changed."), QStringLiteral("find")));
    {
        auto e = makeRow(QStringLiteral("steamcmd:download"), QObject::tr("Download SteamCMD"),
            QObject::tr("Asks first, then downloads Valve's SteamCMD."), QStringLiteral("download"));
        e.disabled = !ui.setupStatus.isEmpty();
        rows.push_back(std::move(e));
    }
    if (have) {
        rows.push_back(fieldRow(QStringLiteral("steamcmd:account"), account.isEmpty() ? QObject::tr("Sign In") : QObject::tr("Account"),
            account.isEmpty() ? QObject::tr("Sign in with your Steam account name and password. SteamCMD keeps its own sign-in afterwards.")
                              : QObject::tr("Cross signs in again, for example as another account."),
            QStringLiteral("signin"), account.isEmpty() ? QObject::tr("Not signed in") : account));
        if (!account.isEmpty()) {
            auto e = makeRow(QStringLiteral("steamcmd:signout"), QObject::tr("Sign Out of SteamCMD"),
                QObject::tr("Forgets the account here and removes SteamCMD's own saved sign-in. The Steam client is not touched."),
                QStringLiteral("signout"));
            e.disabled = signedInBusy;
            rows.push_back(std::move(e));
        }
        if (!libraries.isEmpty()) {
            rows.push_back(fieldRow(QStringLiteral("steamcmd:library"), QObject::tr("Install Location"),
                QObject::tr("Choose the Steam library for new installs."), QStringLiteral("library"),
                nativePath(chosenLibrary)));
        }
    }
    rows.push_back(infoRow(QStringLiteral("steamcmd:privacy"), QObject::tr("About Your Password"),
        QObject::tr("The XMB asks for it on its own keyboard and gives it to SteamCMD only."),
        QObject::tr("Password: typed on the XMB keyboard, shown as dots, held in memory only until SteamCMD has read it, then erased\n"
                    "Stored: only your account name; SteamCMD keeps its own sign-in token\n"
                    "Never: in a file, a log, a command line or an environment variable\n"
                    "Steam client: its sign-in is never read or changed\n"
                    "Games: still start in the Steam client")));
    return rows;
}

static QVector<Entry> steamCmdBrowseRows(const QString& here) {
    QVector<Entry> rows;
    const QString exe = steamcmd::resolveExecutable(here);
    auto use = makeRow(QStringLiteral("steamcmd:use"), here.isEmpty() ? QObject::tr("Drives") : QObject::tr("Use This Folder"),
        here.isEmpty() ? QObject::tr("Pick a drive below.")
                       : exe.isEmpty() ? QObject::tr("steamcmd.exe is not in %1.").arg(nativePath(here))
                                       : QObject::tr("Uses %1.").arg(nativePath(exe)),
        QStringLiteral("use|") + here);
    use.disabled = exe.isEmpty();
    if (here.isEmpty()) { use.action = Action::Information; use.value = use.detail; }
    use.fieldValue = here.isEmpty() ? QString() : nativePath(here);
    rows.push_back(std::move(use));
    if (!here.isEmpty())
        rows.push_back(makeRow(QStringLiteral("steamcmd:up"), QObject::tr("Up One Level"), QObject::tr("Goes to the folder above."),
                               QStringLiteral("cd|") + setup::parentOf(here)));
    rows.push_back(makeRow(QStringLiteral("steamcmd:type"), QObject::tr("Type a Path"), QObject::tr("Opens the on-screen keyboard."), QStringLiteral("type")));
    for (const QString& folder : setup::browseFolders(here)) {
        const QString label = here.isEmpty() ? nativePath(folder) : QFileInfo(folder).fileName();
        rows.push_back(makeRow(QStringLiteral("steamcmd:dir:") + folder, label, QObject::tr("Opens %1.").arg(nativePath(folder)), QStringLiteral("cd|") + folder));
    }
    return rows;
}

static QVector<Entry> steamCmdLibraryRows(const QStringList& libraries, const QString& selected) {
    QVector<Entry> rows;
    for (const QString& library : libraries) {
        rows.push_back(makeRow(QStringLiteral("steamcmd:library:") + library, nativePath(library),
                               library == selected ? QObject::tr("Current install location")
                                                   : QObject::tr("Use this library for new installs"),
                               QStringLiteral("select-library|") + library));
    }
    if (rows.isEmpty())
        rows.push_back(makeRow(QStringLiteral("steamcmd:no-libraries"), QObject::tr("No Steam Libraries"),
                               QObject::tr("Open Steam once to set up a library, then try again.")));
    return rows;
}

static QVector<Entry> signInRows(const SteamCmdUi& ui) {
    using Sign = SteamCmdUi::Sign;
    QVector<Entry> rows;
    const bool busy = ui.sign == Sign::Working || ui.sign == Sign::NeedCode || ui.sign == Sign::Mobile;
    rows.push_back(fieldRow(QStringLiteral("steamcmd:user"), QObject::tr("Steam Account Name"),
        QObject::tr("Cross to type the name you sign in to Steam with (not your email address or display name)."),
        QStringLiteral("field|user"), ui.username.isEmpty() ? QObject::tr("Not set") : ui.username));
    rows.push_back(fieldRow(QStringLiteral("steamcmd:password"), QObject::tr("Password"),
        QObject::tr("Cross to type your password. It is shown as dots."), QStringLiteral("field|password"),
        QString(ui.passwordChars, QChar(0x2022))));
    rows.last().disabled = busy;
    switch (ui.sign) {
    case Sign::Idle: break;
    case Sign::Working:
        rows.push_back(makeRow(QStringLiteral("steamcmd:state"), QObject::tr("Signing In..."), QObject::tr("Waiting for SteamCMD.")));
        break;
    case Sign::NeedCode: {
        auto e = fieldRow(QStringLiteral("steamcmd:code"), ui.codePrompt, QObject::tr("Cross to type the code Steam sent you."),
                          QStringLiteral("code"), QObject::tr("Needed"));
        rows.push_back(std::move(e));
        break;
    }
    case Sign::Mobile:
        rows.push_back(makeRow(QStringLiteral("steamcmd:state"), QObject::tr("Approve on Your Phone"),
            QObject::tr("Open the Steam app on your phone and approve this sign-in. Nothing to type here.")));
        break;
    case Sign::Failed:
        rows.push_back(makeRow(QStringLiteral("steamcmd:state"), QObject::tr("Could Not Sign In"), ui.signMessage));
        break;
    }
    if (!ui.signMessage.isEmpty() && ui.sign != Sign::Failed)
        rows.push_back(makeRow(QStringLiteral("steamcmd:note"), QObject::tr("SteamCMD Says"), ui.signMessage));
    {
        auto e = makeRow(QStringLiteral("steamcmd:submit"), QObject::tr("Sign In"), QObject::tr("Starts SteamCMD and signs in. The password goes to SteamCMD only."), QStringLiteral("submit"));
        e.container = QStringLiteral("explore_plugin_full");
        e.texture = QStringLiteral("tex_Sign_In");
        e.disabled = busy;
        rows.push_back(std::move(e));
    }
    rows.push_back(makeRow(QStringLiteral("steamcmd:cancel"), QObject::tr("Cancel"), QObject::tr("Leaves sign-in and stops SteamCMD."), QStringLiteral("cancel")));
    return rows;
}

void Shell::openSteamCmdPage() {
    if (pages_.size() >= 32) return;
    for (const auto& p : pages_) if (p.steamCmd == QLatin1String("main")) return;
    Page page;
    page.title = tr("SteamCMD");
    page.steamCmd = QStringLiteral("main");
    pages_.push_back(std::move(page));
    steamCmdRefreshJobRows();   // (also rebuilds this page through the shared refresher below)
    qInfo().noquote() << "steamcmd: page opened";
    emit changed();
}

// Rebuilds every SteamCMD page from the state, keeping the focused row, and the job rows of an open Download Management page.
void Shell::steamCmdRefreshJobRows() {
    auto& ui = steamCmdState();
    const QString exe = steamcmd::resolveExecutable(preferences_.value(kPathKey).toString());
    const QString account = preferences_.value(kAccountKey).toString();
    const QStringList libraries = steamcmd::steamLibraries(steamRoot_);
    const QString chosen = libraryChoice(preferences_.value(kLibraryKey).toString(), libraries);
    const bool signBusy = ui.signRun != nullptr;
    for (auto& page : pages_) {
        if (page.steamCmd.isEmpty()) {
            if (!page.downloads) continue;
            // Download Management: SteamCMD's rows first, the client's own rows as they were.
            const QString focused = page.entries.isEmpty() ? QString() : page.entries.value(page.selection).id;
            QVector<Entry> rows = steamCmdJobRows();
            for (const auto& e : page.entries) if (!e.id.startsWith(QLatin1String("download:steamcmd:"))) rows.push_back(e);
            page.entries = std::move(rows);
            for (int i = 0; i < page.entries.size(); ++i) if (page.entries[i].id == focused) { page.selection = i; break; }
            page.selection = std::clamp(page.selection, 0, std::max(0, int(page.entries.size()) - 1));
            continue;
        }
        const QString focused = page.entries.isEmpty() ? QString() : page.entries.value(page.selection).id;
        if (page.steamCmd == QLatin1String("main")) page.entries = mainRows(ui, exe, account, libraries, chosen, signBusy);
        else if (page.steamCmd == QLatin1String("signin")) page.entries = signInRows(ui);
        else if (page.steamCmd == QLatin1String("browse")) page.entries = steamCmdBrowseRows(ui.browsePath);
        else if (page.steamCmd == QLatin1String("library")) page.entries = steamCmdLibraryRows(libraries, chosen);
        for (int i = 0; i < page.entries.size(); ++i) if (page.entries[i].id == focused) { page.selection = i; break; }
        page.selection = std::clamp(page.selection, 0, std::max(0, int(page.entries.size()) - 1));
    }
}

QVector<Entry> Shell::steamCmdJobRows() {
    QVector<Entry> rows;
    for (const auto& job : steamCmdState().jobs) {
        using State = SteamCmdUi::Job::State;
        Entry e = makeRow(QStringLiteral("download:steamcmd:") + QString::number(job.id), job.title, {},
                          QStringLiteral("job|") + QString::number(job.id));
        e.container = QStringLiteral("explore_category_network");
        e.texture = QStringLiteral("tex_dl_dst_game");
        QString state, hint;
        switch (job.state) {
        case State::Queued: state = QObject::tr("Waiting for SteamCMD"); hint = QObject::tr("Cross removes it from the queue."); break;
        case State::Running:
            if (job.progress.valid && job.progress.total > 0)
                state = QStringLiteral("%1 %2% (%3 of %4)").arg(capitalised(job.progress.label)).arg(qRound(job.progress.percent))
                            .arg(sizeText(job.progress.done), sizeText(job.progress.total));
            else if (job.progress.valid) state = capitalised(job.progress.label);
            else state = kindWord(job.kind) + QStringLiteral("...");
            hint = QObject::tr("Cross stops it.");
            break;
        case State::Done: state = QObject::tr("Finished"); if (!job.message.isEmpty()) state += QStringLiteral(" - ") + job.message; hint = QObject::tr("Cross dismisses this."); break;
        case State::Failed: state = QObject::tr("Failed: %1").arg(job.message); hint = QObject::tr("Cross opens retry and removal options."); break;
        case State::Cancelled: state = QObject::tr("Stopped"); hint = QObject::tr("Cross opens retry and removal options."); break;
        case State::Interrupted: state = QObject::tr("Saved for Retry"); hint = QObject::tr("Cross opens retry and removal options."); break;
        }
        e.detail = QStringLiteral("SteamCMD - ") + state;
        e.setting = job.title + QStringLiteral("\n\nStatus: ") + state + QStringLiteral("\nTask: ") + kindWord(job.kind) + QStringLiteral(" with SteamCMD")
                    + QStringLiteral("\nSteam App ID: ") + job.appId + QStringLiteral("\nFolder: ") + nativePath(job.folder) + QStringLiteral("\n") + hint;
        rows.push_back(std::move(e));
    }
    return rows;
}

// ---- jobs ----------------------------------------------------------------------------------------------------
void Shell::steamCmdStartJob(const QString& appId, const QString& title, int kind) {
    auto& ui = steamCmdState();
    if (!steamcmd::validAppId(appId)) { showMessage(title, tr("This is not a Steam game this program can install."), MessageKind::Page); emit changed(); return; }
    if (!steamCmdReady()) { openSteamCmdPage(); return; }
    if (diagnostics::safeMode()) { showMessage(tr("SteamCMD"), tr("Safe mode does not use the network."), MessageKind::Page); emit changed(); return; }
    for (const auto& j : ui.jobs)
        if (j.appId == appId && j.state != SteamCmdUi::Job::State::Done) {
            showMessage(title, tr("This game already has a SteamCMD job in Download Management."), MessageKind::Page); emit changed(); return;
        }
    if (ui.jobs.size() >= MaxJobs) {
        showMessage(tr("SteamCMD"), tr("Download Management is full. Remove a job before adding another."), MessageKind::Page);
        emit changed();
        return;
    }
    SteamCmdUi::Job job;
    job.id = ui.nextJob++;
    job.appId = appId;
    job.title = title.isEmpty() ? QStringLiteral("Steam app ") + appId : title.left(256);
    job.kind = kind;
    QString installed;
    for (const auto& g : steamInstalled_.games) if (g.appId == appId) { installed = g.installDirectory; break; }
    if (!installed.isEmpty() && kind != 0) {
        job.folder = QDir::cleanPath(installed);
        // <library>/steamapps/common/<game>: the library is three levels up.
        QDir d(job.folder);
        d.cdUp(); d.cdUp(); d.cdUp();
        job.library = d.absolutePath();
    } else {
        const QStringList libraries = steamcmd::steamLibraries(steamRoot_);
        job.library = libraryChoice(preferences_.value(kLibraryKey).toString(), libraries);
        if (job.library.isEmpty()) {
            showMessage(tr("SteamCMD"), tr("The Steam client's library folders are not known, so there is nowhere to install to. Start Steam once, then try again."), MessageKind::Page);
            emit changed(); return;
        }
        job.folder = steamcmd::installFolderFor(job.library, steamcmd::safeFolderName(job.title, appId));
    }
    ui.jobs.push_back(job);
    if (!ui.saveBacklog(dataDirectory_)) {
        ui.jobs.removeLast();
        showMessage(tr("SteamCMD"), tr("Could not save the new job."), MessageKind::Page);
        emit changed();
        return;
    }
    qInfo().noquote() << QStringLiteral("steamcmd: job %1 queued (%2, app %3)").arg(job.id).arg(kindWord(kind), appId);
    // Show where it goes: the Download Management page, with the job on top.
    if ((pages_.isEmpty() || !pages_.last().downloads) && pages_.size() < 32) {
        Page page;
        page.title = tr("Download Management");
        page.downloads = true;
        pages_.push_back(std::move(page));
        QString error;
        pages_.last().entries = steamDownloadRows(error);
    }
    steamCmdRefreshJobRows();
    notify(tr("SteamCMD"), tr("%1 %2").arg(kindWord(kind), job.title));
    steamCmdPump();
    emit changed();
}

// Starts the next waiting job when SteamCMD is idle. SteamCMD runs one job at a time.
void Shell::steamCmdPump() {
    auto& ui = steamCmdState();
    if (ui.jobRun || diagnostics::safeMode()) return;
    const QStringList libraries = steamcmd::steamLibraries(steamRoot_);
    for (auto& j : ui.jobs) {
        if (j.state != SteamCmdUi::Job::State::Queued) continue;
        const QString exe = steamcmd::resolveExecutable(preferences_.value(kPathKey).toString());
        const QString account = preferences_.value(kAccountKey).toString();
        if (exe.isEmpty() || !steamcmd::validAccountName(account)) {
            j.state = SteamCmdUi::Job::State::Failed;
            j.message = tr("SteamCMD is not signed in. Sign in in Users > Steam Account > SteamCMD; this job then runs again.");
            j.needsSignIn = true;
            continue;
        }
        if (!libraries.contains(j.library, Qt::CaseInsensitive) || !isJobFolder(j.library, j.folder) ||
            !QDir().mkpath(j.folder)) {
            j.state = SteamCmdUi::Job::State::Failed;
            j.message = tr("The saved install folder is unavailable. Reconnect the Steam library or remove this job.");
            continue;
        }
        auto* run = new steamcmd::Run(this);
        ui.jobRun = run;
        ui.runningJob = j.id;
        j.state = SteamCmdUi::Job::State::Running;
        const quint64 id = j.id;
        connect(run, &steamcmd::Run::progressChanged, this, [this, run, id] {
            auto& u = steamCmdState();
            for (auto& x : u.jobs) if (x.id == id) x.progress = run->lastProgress();
            if (!u.lastJobPaint.isValid() || u.lastJobPaint.elapsed() > 250) { u.lastJobPaint.start(); steamCmdRefreshJobRows(); emit changed(); }
        });
        connect(run, &steamcmd::Run::finished, this, [this, run, id](int outcomeValue, QString message) {
            using O = steamcmd::Run::Outcome;
            using S = SteamCmdUi::Job::State;
            auto& u = steamCmdState();
            const auto outcome = O(outcomeValue);
            u.jobRun = nullptr;
            u.runningJob = 0;
            run->deleteLater();
            bool relist = false;
            for (auto& x : u.jobs) {
                if (x.id != id) continue;
                switch (outcome) {
                case O::JobDone: {
                    x.state = S::Done;
                    const auto link = steamcmd::integrateManifest(x.library, x.folder, x.appId);
                    x.message = link.kind == steamcmd::Integration::Kind::Linked ? tr("Steam lists it once it rescans")
                              : link.kind == steamcmd::Integration::Kind::AlreadyKnown ? QString() : link.detail;
                    qInfo().noquote() << QStringLiteral("steamcmd: job %1 finished, manifest result %2").arg(id).arg(int(link.kind));
                    notify(tr("SteamCMD"), tr("%1 finished").arg(x.title));
                    relist = true;
                    break;
                }
                case O::Cancelled: x.state = S::Cancelled; qInfo().noquote() << QStringLiteral("steamcmd: job %1 stopped").arg(id); break;
                case O::NeedsSignIn:
                    x.state = S::Failed;
                    x.message = tr("SteamCMD's saved sign-in is missing or has expired. Sign in again in Users > Steam Account > SteamCMD; this job then runs again.");
                    x.needsSignIn = true;
                    qInfo().noquote() << QStringLiteral("steamcmd: job %1 needs sign-in").arg(id);
                    break;
                case O::RateLimited: x.state = S::Failed; x.message = tr("Steam is limiting sign-ins right now (%1). Wait a while before trying again.").arg(message); break;
                default:
                    x.state = S::Failed;
                    x.message = message.isEmpty() ? tr("SteamCMD ended without finishing.") : message;
                    qInfo().noquote() << QStringLiteral("steamcmd: job %1 failed (outcome %2)").arg(id).arg(outcomeValue);
                    break;
                }
            }
            if (outcome == O::NeedsSignIn) {
                // The saved sign-in no longer works: say so, and make the next step the sign-in page.
                u.username = preferences_.value(kAccountKey).toString();
                preferences_.setValue(kAccountKey, QString());
            }
            if (!u.saveBacklog(dataDirectory_))
                notify(tr("SteamCMD"), tr("Could not update the saved jobs."));
            steamCmdRefreshJobRows();
            if (relist) QTimer::singleShot(1500, this, &Shell::refreshContent);
            QTimer::singleShot(0, this, &Shell::steamCmdPump);
            emit changed();
        });
        qInfo().noquote() << QStringLiteral("steamcmd: job %1 started").arg(j.id);
        run->start(steamcmd::Run::Mode::Job, exe, steamcmd::jobArguments(account, j.appId, j.folder, j.kind != 1));
        return;
    }
    steamCmdRefreshJobRows();
    emit changed();
}

// ---- sign-in -------------------------------------------------------------------------------------------------
void Shell::activateSteamCmd(const Entry& entry) {
    auto& ui = steamCmdState();
    const QString command = entry.value.section(QLatin1Char('|'), 0, 0);
    const QString argument = entry.value.section(QLatin1Char('|'), 1);
    const auto refresh = [this] { steamCmdRefreshJobRows(); emit changed(); };
    const QString exe = steamcmd::resolveExecutable(preferences_.value(kPathKey).toString());

    if (command == QLatin1String("open")) { openSteamCmdPage(); return; }
    if (command == QLatin1String("find")) {
        const QStringList found = steamcmd::candidateExecutables(preferences_.value(kPathKey).toString(), dataDirectory_);
        if (found.isEmpty()) {
            showMessage(tr("SteamCMD"), tr("No steamcmd.exe was found in the usual folders. Choose its folder, or download it."), MessageKind::Page);
        } else {
            preferences_.setValue(kPathKey, found.first());
            notify(tr("SteamCMD"), tr("Found %1").arg(nativePath(found.first())));
            qInfo().noquote() << "steamcmd: found" << nativePath(found.first());
        }
        refresh(); return;
    }
    if (command == QLatin1String("browse")) {
        ui.browsePath = exe.isEmpty() ? QString() : QFileInfo(exe).absolutePath();
        Page page;
        page.title = tr("Choose the SteamCMD Folder");
        page.steamCmd = QStringLiteral("browse");
        pages_.push_back(std::move(page));
        refresh(); return;
    }
    if (command == QLatin1String("cd")) { ui.browsePath = argument.isEmpty() ? QString() : QDir::cleanPath(argument); refresh(); return; }
    if (command == QLatin1String("type")) { emit requestJellyfinFieldEdit(QStringLiteral("steamcmd:path"), tr("SteamCMD Folder"), ui.browsePath, false); return; }
    if (command == QLatin1String("use")) {
        const QString resolved = steamcmd::resolveExecutable(argument);
        if (resolved.isEmpty()) { showMessage(tr("SteamCMD"), tr("steamcmd.exe is not in %1.").arg(nativePath(argument)), MessageKind::Page); emit changed(); return; }
        if (steamcmd::looksLikeSteamClientFolder(QFileInfo(resolved).absolutePath())) {
            showMessage(tr("SteamCMD"), tr("%1 also holds the Steam client. Use a separate SteamCMD folder.").arg(nativePath(QFileInfo(resolved).absolutePath())), MessageKind::Page);
            emit changed(); return;
        }
        preferences_.setValue(kPathKey, resolved);
        qInfo().noquote() << "steamcmd: folder chosen" << nativePath(resolved);
        for (int i = pages_.size() - 1; i >= 0; --i) if (pages_[i].steamCmd == QLatin1String("browse")) { pages_.removeAt(i); break; }
        refresh(); return;
    }
    if (command == QLatin1String("library")) {
        if (pages_.size() >= 32) return;
        const QStringList libraries = steamcmd::steamLibraries(steamRoot_);
        const QString selected = libraryChoice(preferences_.value(kLibraryKey).toString(), libraries);
        Page page;
        page.title = tr("Install Location");
        page.steamCmd = QStringLiteral("library");
        page.entries = steamCmdLibraryRows(libraries, selected);
        page.selection = std::max(0, int(libraries.indexOf(selected)));
        pages_.push_back(std::move(page));
        emit changed();
        return;
    }
    if (command == QLatin1String("select-library")) {
        if (pages_.isEmpty() || pages_.last().steamCmd != QLatin1String("library")) return;
        const QStringList libraries = steamcmd::steamLibraries(steamRoot_);
        if (!libraries.contains(argument, Qt::CaseInsensitive)) {
            showMessage(tr("Install Location"), tr("That Steam library is no longer available."), MessageKind::Page);
        } else if (!preferences_.setValue(kLibraryKey, argument)) {
            showMessage(tr("Install Location"), tr("Could not save the selected library."), MessageKind::Page);
        } else {
            pages_.removeLast();
        }
        refresh();
        return;
    }
    if (command == QLatin1String("signin")) {
        if (exe.isEmpty()) { showMessage(tr("SteamCMD"), tr("Set up SteamCMD first."), MessageKind::Page); emit changed(); return; }
        for (const auto& p : pages_) if (p.steamCmd == QLatin1String("signin")) return;
        ui.sign = SteamCmdUi::Sign::Idle;
        ui.signMessage.clear();
        ui.password.wipe();
        ui.passwordChars = 0;
        ui.username = preferences_.value(kAccountKey).toString();
        Page page;
        page.title = tr("Sign In to SteamCMD");
        page.steamCmd = QStringLiteral("signin");
        pages_.push_back(std::move(page));
        steamCmdRefreshJobRows();
        pages_.last().selection = ui.username.isEmpty() ? 0 : 1;
        qInfo() << "steamcmd: sign-in page opened";
        emit changed(); return;
    }
    if (command == QLatin1String("field")) {
        if (ui.signRun) return;
        if (argument == QLatin1String("user")) emit requestJellyfinFieldEdit(QStringLiteral("steamcmd:user"), tr("Steam Account Name"), ui.username, false);
        else emit requestJellyfinFieldEdit(QStringLiteral("steamcmd:password"), tr("Steam Password"), QString(), true);
        return;
    }
    if (command == QLatin1String("code")) {
        if (ui.sign == SteamCmdUi::Sign::NeedCode) emit requestJellyfinFieldEdit(QStringLiteral("steamcmd:code"), ui.codePrompt, QString(), true);
        return;
    }
    if (command == QLatin1String("cancel")) { steamCmdBack(); emit changed(); return; }
    if (command == QLatin1String("submit")) {
        using Sign = SteamCmdUi::Sign;
        if (ui.signRun || exe.isEmpty()) return;
        if (!steamcmd::validAccountName(ui.username)) {
            ui.sign = Sign::Failed;
            ui.signMessage = tr("Type your Steam account name first: letters, numbers and _ - . only, at least 3 characters.");
            refresh(); return;
        }
        if (ui.password.isEmpty()) { ui.sign = Sign::Failed; ui.signMessage = tr("Type your password first."); refresh(); return; }
        if (diagnostics::safeMode()) { ui.sign = Sign::Failed; ui.signMessage = tr("Safe mode does not use the network."); refresh(); return; }
        ui.sign = Sign::Working;
        ui.signMessage.clear();
        auto* run = new steamcmd::Run(this);
        ui.signRun = run;
        const QString user = ui.username;
        connect(run, &steamcmd::Run::promptRequested, this, [this, run](int promptValue) {
            auto& u = steamCmdState();
            const auto prompt = steamcmd::Prompt(promptValue);
            if (prompt == steamcmd::Prompt::Password) {
                qInfo() << "steamcmd: prompt: password";
                steamcmd::Secret secret = std::move(u.password);
                u.passwordChars = 0;
                const bool sent = run->provide(secret);
                qInfo().noquote() << (sent ? "steamcmd: sent" : "steamcmd: nothing to send");
            } else {
                qInfo().noquote() << (prompt == steamcmd::Prompt::GuardEmail ? "steamcmd: prompt: steam guard email code" : "steamcmd: prompt: steam guard app code");
                u.sign = SteamCmdUi::Sign::NeedCode;
                u.codePrompt = prompt == steamcmd::Prompt::GuardEmail ? tr("Steam Guard Code (from your email)") : tr("Steam Guard Code (from the Steam app)");
                u.signMessage = run->lastMessage().isEmpty() ? QString() : tr("That code was not accepted: %1").arg(run->lastMessage());
                steamCmdRefreshJobRows();
                emit requestJellyfinFieldEdit(QStringLiteral("steamcmd:code"), u.codePrompt, QString(), true);
            }
            emit changed();
        });
        connect(run, &steamcmd::Run::stateChanged, this, [this](int state) {
            auto& u = steamCmdState();
            if (steamcmd::Run::State(state) == steamcmd::Run::State::WaitingMobile) {
                qInfo() << "steamcmd: waiting for approval on the phone";
                u.sign = SteamCmdUi::Sign::Mobile;
                steamCmdRefreshJobRows();
                emit changed();
            } else if (steamcmd::Run::State(state) == steamcmd::Run::State::Working && u.sign != SteamCmdUi::Sign::Working && u.sign != SteamCmdUi::Sign::Failed) {
                u.sign = SteamCmdUi::Sign::Working;
                steamCmdRefreshJobRows();
                emit changed();
            }
        });
        connect(run, &steamcmd::Run::finished, this, [this, run, user](int outcomeValue, QString message) {
            using O = steamcmd::Run::Outcome;
            auto& u = steamCmdState();
            u.signRun = nullptr;
            u.password.wipe();
            u.passwordChars = 0;
            run->deleteLater();
            const auto outcome = O(outcomeValue);
            qInfo().noquote() << QStringLiteral("steamcmd: sign-in ended (outcome %1)").arg(outcomeValue);
            if (outcome == O::SignedIn) {
                preferences_.setValue(kAccountKey, user);
                u.sign = SteamCmdUi::Sign::Idle;
                u.signMessage.clear();
                for (int i = pages_.size() - 1; i >= 0; --i) if (pages_[i].steamCmd == QLatin1String("signin")) { pages_.removeAt(i); break; }
                notify(tr("SteamCMD"), tr("Signed in as %1").arg(user));
                steamCmdRefreshJobRows();
                // Jobs that stopped for want of a sign-in run again now.
                for (auto& j : u.jobs) if (j.state == SteamCmdUi::Job::State::Failed && j.needsSignIn) { j.state = SteamCmdUi::Job::State::Queued; j.needsSignIn = false; j.message.clear(); }
                steamCmdRefreshJobRows();
                QTimer::singleShot(0, this, &Shell::steamCmdPump);
                emit changed();
                return;
            }
            if (outcome == O::Cancelled) { u.sign = SteamCmdUi::Sign::Idle; steamCmdRefreshJobRows(); emit changed(); return; }
            u.sign = SteamCmdUi::Sign::Failed;
            switch (outcome) {
            case O::BadPassword: u.signMessage = tr("Steam did not accept the account name or password (%1). Type the password again.").arg(message); break;
            case O::BadGuardCode: u.signMessage = tr("Steam did not accept the code (%1). Sign in again for a new one.").arg(message); break;
            case O::RateLimited: u.signMessage = tr("Steam is limiting sign-ins right now (%1). Wait a while, then try once.").arg(message); break;
            case O::MobileTimedOut: u.signMessage = tr("The sign-in was not approved on the phone in time. Try again."); break;
            case O::NotStarted: u.signMessage = tr("SteamCMD could not be started: %1").arg(message); break;
            default: u.signMessage = message.isEmpty() ? tr("SteamCMD ended without signing in.") : message; break;
            }
            steamCmdRefreshJobRows();
            emit changed();
        });
        qInfo().noquote() << "steamcmd: sign-in started";
        run->start(steamcmd::Run::Mode::SignIn, exe, steamcmd::signInArguments(user));
        refresh(); return;
    }
    if (command == QLatin1String("signout")) {
        const QString dir = exe.isEmpty() ? QString() : QFileInfo(exe).absolutePath();
        const bool clientFolder = steamcmd::looksLikeSteamClientFolder(dir);
        const QString file = dir + QStringLiteral("/config/config.vdf");
        const QString body = clientFolder
            ? tr("This forgets the account name here. The folder also holds the Steam client, so its files are left alone: to remove SteamCMD's saved sign-in, sign out in SteamCMD itself.")
            : tr("This forgets the account name here and deletes SteamCMD's own saved sign-in file:\n%1\nSteamCMD recreates it the next time it runs. The Steam client's sign-in is not touched.").arg(nativePath(file));
        showConfirm(tr("Sign Out of SteamCMD?"), body, [this, clientFolder, file] {
            bool removed = true;
            if (!clientFolder && QFileInfo::exists(file)) removed = QFile::remove(file);
            preferences_.setValue(kAccountKey, QString());
            qInfo().noquote() << "steamcmd: signed out" << (clientFolder ? "(client folder: no file removed)" : removed ? "(saved sign-in file removed)" : "(saved sign-in file could not be removed)");
            if (!removed) showMessage(tr("SteamCMD"), tr("The account was forgotten here, but %1 could not be deleted. Delete it yourself to finish.").arg(nativePath(file)), MessageKind::Page);
            else notify(tr("SteamCMD"), tr("Signed out"));
            steamCmdRefreshJobRows();
            emit changed();
        });
        emit changed(); return;
    }
    if (command == QLatin1String("download")) {
        if (ui.reply || ui.firstRun) return;
        if (diagnostics::safeMode()) { showMessage(tr("SteamCMD"), tr("Safe mode does not use the network."), MessageKind::Page); emit changed(); return; }
        const QString dest = QDir::cleanPath(dataDirectory_ + QStringLiteral("/steamcmd"));
        showConfirm(tr("Download SteamCMD?"),
            tr("Source:\n%1\n\nDestination:\n%2\n\nValve publishes no checksum or signature for this file, so the program can only check that it is a zip that holds steamcmd.exe. SteamCMD updates itself from Valve the first time it runs. Nothing is downloaded unless you answer Yes.")
                .arg(QString::fromLatin1(steamcmd::kDownloadUrl), nativePath(dest)),
            [this, dest] {
                auto& u = steamCmdState();
                QNetworkRequest request{QUrl(QString::fromLatin1(steamcmd::kDownloadUrl))};
                request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
                request.setTransferTimeout(30000);
                u.zip.clear();
                u.reply = dlnaNetwork_->get(request);
                u.setupStatus = tr("Downloading from Valve...");
                qInfo().noquote() << "steamcmd: download started" << QString::fromLatin1(steamcmd::kDownloadUrl);
                auto* reply = u.reply;
                connect(reply, &QNetworkReply::readyRead, this, [this, reply] {
                    auto& v = steamCmdState();
                    v.zip += reply->readAll();
                    if (v.zip.size() > steamcmd::kMaxZipBytes) reply->abort();
                });
                connect(reply, &QNetworkReply::finished, this, [this, reply, dest] {
                    auto& v = steamCmdState();
                    v.reply = nullptr;
                    reply->deleteLater();
                    const auto fail = [&](const QString& why) {
                        v.setupStatus.clear();
                        v.zip.clear();
                        qWarning().noquote() << "steamcmd: download failed:" << why;
                        showMessage(tr("SteamCMD"), why, MessageKind::Page);
                        steamCmdRefreshJobRows();
                        emit changed();
                    };
                    if (reply->error() != QNetworkReply::NoError) { fail(tr("The download did not complete: %1").arg(reply->errorString())); return; }
                    v.zip += reply->readAll();
                    QString why;
                    if (!steamcmd::zipHoldsSteamcmd(v.zip, &why)) { fail(tr("The download was refused: %1").arg(why)); return; }
                    QDir().mkpath(dest);
                    const QString zipPath = dest + QStringLiteral("/steamcmd.zip");
                    QFile file(zipPath);
                    if (!file.open(QIODevice::WriteOnly) || file.write(v.zip) != v.zip.size()) { fail(tr("The download could not be saved in %1.").arg(nativePath(dest))); return; }
                    file.close();
                    v.zip.clear();
                    v.setupStatus = tr("Unpacking...");
                    steamCmdRefreshJobRows();
                    emit changed();
                    // Windows ships a zip-capable tar.exe (bsdtar); it also refuses entries that climb out of the folder.
                    const QString tar = qEnvironmentVariable("SystemRoot", QStringLiteral("C:/Windows")) + QStringLiteral("/System32/tar.exe");
                    auto* unpack = new QProcess(this);
                    connect(unpack, &QProcess::finished, this, [this, unpack, dest, zipPath](int code, QProcess::ExitStatus) {
                        unpack->deleteLater();
                        auto& w = steamCmdState();
                        QFile::remove(zipPath);
                        const QString exe2 = steamcmd::resolveExecutable(dest);
                        if (code != 0 || exe2.isEmpty()) {
                            w.setupStatus.clear();
                            showMessage(tr("SteamCMD"), tr("The zip could not be unpacked into %1.").arg(nativePath(dest)), MessageKind::Page);
                            steamCmdRefreshJobRows(); emit changed(); return;
                        }
                        // First run: SteamCMD brings itself up to date from Valve. No account is involved.
                        w.setupStatus = tr("SteamCMD is updating itself (first run)...");
                        steamCmdRefreshJobRows(); emit changed();
                        auto* first = new QProcess(this);
                        w.firstRun = first;
                        first->setProcessChannelMode(QProcess::MergedChannels);
                        first->setWorkingDirectory(QFileInfo(exe2).absolutePath());
                        connect(first, &QProcess::finished, this, [this, first, exe2](int c2, QProcess::ExitStatus) {
                            first->deleteLater();
                            auto& x = steamCmdState();
                            x.firstRun = nullptr;
                            x.setupStatus.clear();
                            preferences_.setValue(kPathKey, exe2);
                            qInfo().noquote() << "steamcmd: installed" << nativePath(exe2) << "first run exit" << c2;
                            if (c2 != 0) showMessage(tr("SteamCMD"), tr("SteamCMD was unpacked, but its first run ended with code %1. It is set up anyway; sign in to find out whether it works.").arg(c2), MessageKind::Page);
                            else notify(tr("SteamCMD"), tr("SteamCMD is ready"));
                            steamCmdRefreshJobRows(); emit changed();
                        });
                        first->start(exe2, {QStringLiteral("+quit")});
                    });
                    unpack->setWorkingDirectory(dest);
                    unpack->start(tar, {QStringLiteral("-xf"), QStringLiteral("steamcmd.zip")});
                });
                steamCmdRefreshJobRows();
                emit changed();
            });
        emit changed(); return;
    }
    if (command == QLatin1String("job") || command == QLatin1String("job-retry") || command == QLatin1String("job-remove")) {
        const quint64 id = argument.toULongLong();
        const auto position = std::find_if(ui.jobs.begin(), ui.jobs.end(), [id](const SteamCmdUi::Job& job) { return job.id == id; });
        if (position == ui.jobs.end()) return;
        auto& job = *position;
        using State = SteamCmdUi::Job::State;
        const bool canRetry = job.state == State::Failed || job.state == State::Cancelled || job.state == State::Interrupted;
        if (command == QLatin1String("job") && job.state == State::Running) {
            if (ui.jobRun) {
                const QString title = job.title;
                ui.jobRun->cancel();
                notify(tr("SteamCMD"), tr("Stopping %1").arg(title));
            }
        } else if (command == QLatin1String("job") && canRetry) {
            if (pages_.size() >= 32) return;
            Page options;
            options.title = job.title;
            options.steamCmd = QStringLiteral("job-options");
            options.entries.push_back(makeRow(QStringLiteral("steamcmd:retry"), tr("Retry"),
                tr("Run this job again in the same folder."), QStringLiteral("job-retry|") + argument));
            options.entries.push_back(makeRow(QStringLiteral("steamcmd:remove"), tr("Remove Job"),
                tr("Remove this job from the list and keep its files."), QStringLiteral("job-remove|") + argument));
            pages_.push_back(std::move(options));
        } else if (command == QLatin1String("job-retry") && canRetry) {
            if (diagnostics::safeMode()) {
                showMessage(tr("SteamCMD"), tr("Safe mode does not use the network."), MessageKind::Page);
            } else if (!steamCmdReady()) {
                showMessage(tr("SteamCMD"), tr("Sign in under Users > Steam Account > SteamCMD, then retry this job."), MessageKind::Page);
            } else {
                job.state = State::Queued;
                job.progress = {};
                job.message.clear();
                job.needsSignIn = false;
                if (!pages_.isEmpty() && pages_.last().steamCmd == QLatin1String("job-options"))
                    pages_.removeLast();
                steamCmdPump();
            }
        } else if ((command == QLatin1String("job-remove") && canRetry) ||
                   (command == QLatin1String("job") && (job.state == State::Queued || job.state == State::Done))) {
            const qsizetype index = std::distance(ui.jobs.begin(), position);
            const auto removed = ui.jobs.takeAt(index);
            if (!ui.saveBacklog(dataDirectory_)) {
                ui.jobs.insert(index, removed);
                showMessage(tr("SteamCMD"), tr("Could not remove the saved job."), MessageKind::Page);
            } else if (!pages_.isEmpty() && pages_.last().steamCmd == QLatin1String("job-options")) {
                pages_.removeLast();
            }
        }
        steamCmdRefreshJobRows();
        // A page with nothing left on it is closed rather than left empty.
        if (!pages_.isEmpty() && pages_.last().downloads && pages_.last().entries.isEmpty()) pages_.removeLast();
        emit changed(); return;
    }
}

void Shell::steamCmdField(const QString& field, const QString& value) {
    auto& ui = steamCmdState();
    if (field == QLatin1String("steamcmd:user")) {
        ui.username = value.trimmed();
        // The name is not a secret, but only its length is logged, like every other field here.
        qInfo().noquote() << QStringLiteral("steamcmd: account name set (%1 character(s))").arg(ui.username.size());
    } else if (field == QLatin1String("steamcmd:password")) {
        ui.password = steamcmd::Secret(value);
        ui.passwordChars = int(value.size());
        qInfo().noquote() << QStringLiteral("steamcmd: password set (%1 character(s), not logged)").arg(value.size());
    } else if (field == QLatin1String("steamcmd:code")) {
        if (ui.signRun && ui.sign == SteamCmdUi::Sign::NeedCode) {
            steamcmd::Secret secret(value);
            ui.sign = SteamCmdUi::Sign::Working;
            ui.signMessage.clear();
            const bool sent = ui.signRun->provide(secret);
            qInfo().noquote() << (sent ? "steamcmd: sent" : "steamcmd: nothing to send");
        }
    } else if (field == QLatin1String("steamcmd:path")) {
        const QString path = QDir::cleanPath(value.trimmed());
        if (path.isEmpty()) return;
        if (QFileInfo(path).isDir()) ui.browsePath = path;
        else showMessage(tr("Folder"), tr("%1 does not exist.").arg(nativePath(path)), MessageKind::Page);
    } else return;
    steamCmdRefreshJobRows();
    emit changed();
}

bool Shell::steamCmdBack() {
    if (pages_.isEmpty() || pages_.last().steamCmd != QLatin1String("signin")) return false;
    auto& ui = steamCmdState();
    if (ui.signRun) ui.signRun->cancel();   // its finished() handler leaves the state tidy
    ui.password.wipe();
    ui.passwordChars = 0;
    ui.sign = SteamCmdUi::Sign::Idle;
    ui.signMessage.clear();
    pages_.removeLast();
    steamCmdRefreshJobRows();
    return true;
}

}   // namespace xmb
