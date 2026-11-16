#include "steamcmd.h"
#include "steam_vdf.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <algorithm>

namespace xmb::steamcmd {

// ---- Secret --------------------------------------------------------------------------------------------------
Secret::Secret(const QString& text) : bytes_(text.toUtf8()) { bytes_.detach(); }
Secret::Secret(Secret&& other) noexcept : bytes_(std::move(other.bytes_)) { other.bytes_.clear(); }
Secret& Secret::operator=(Secret&& other) noexcept {
    if (this != &other) { wipe(); bytes_ = std::move(other.bytes_); other.bytes_.clear(); }
    return *this;
}
void Secret::wipe() {
    if (bytes_.isEmpty()) return;
    bytes_.detach();                       // never zero a buffer someone else still shares
    volatile char* p = bytes_.data();
    for (qsizetype i = 0; i < bytes_.size(); ++i) p[i] = 0;
    bytes_.clear();
}

// ---- rules ---------------------------------------------------------------------------------------------------
bool validAccountName(const QString& name) {
    if (name.size() < 3 || name.size() > 64) return false;
    for (const QChar c : name) {
        const ushort u = c.unicode();
        const bool ok = (u >= '0' && u <= '9') || (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || u == '_' || u == '-' || u == '.';
        if (!ok) return false;
    }
    return true;
}
bool validAppId(const QString& appId) {
    if (appId.isEmpty() || appId.size() > 10) return false;
    for (const QChar c : appId) if (c.unicode() < '0' || c.unicode() > '9') return false;
    return true;
}
QString safeFolderName(const QString& title, const QString& appId) {
    QString out;
    for (const QChar c : title) {
        const ushort u = c.unicode();
        if (u < 0x20 || QStringLiteral("<>:\"/\\|?*").contains(c)) { out += QLatin1Char(' '); continue; }
        out += c;
    }
    out = out.simplified();
    while (out.endsWith(QLatin1Char('.')) || out.endsWith(QLatin1Char(' '))) out.chop(1);
    if (out.size() > 60) out = out.left(60).trimmed();
    static const QSet<QString> reserved{"con", "prn", "aux", "nul", "com1", "com2", "com3", "com4", "lpt1", "lpt2", "lpt3"};
    if (out.isEmpty() || out == QLatin1String(".") || out == QLatin1String("..") || reserved.contains(out.toLower()))
        out = QStringLiteral("app_") + appId;
    return out;
}

// ---- output scanner ------------------------------------------------------------------------------------------
namespace {
QString decode(const QByteArray& bytes) { return QString::fromUtf8(bytes); }
}
void OutputScanner::feed(const QByteArray& bytes) {
    for (const char raw : bytes) {
        const unsigned char c = static_cast<unsigned char>(raw);
        if (escape_ == 1) { escape_ = (c == '[') ? 2 : 0; continue; }
        if (escape_ == 2) { if (c >= 0x40 && c <= 0x7e) escape_ = 0; continue; }
        if (c == 0x1b) { escape_ = 1; continue; }
        if (c == 0) continue;
        if (c == '\n' || c == '\r') {
            // CRLF is one terminator; a bare CR is a redraw and ends the line too.
            if (c == '\n' && previousWasCr_) { previousWasCr_ = false; continue; }
            previousWasCr_ = (c == '\r');
            const QString line = decode(tailBytes_).trimmed();
            tailBytes_.clear();
            if (!line.isEmpty()) lines_.push_back(line);
            continue;
        }
        previousWasCr_ = false;
        tailBytes_.push_back(raw);
        if (tailBytes_.size() > 8192) tailBytes_ = tailBytes_.right(4096);
    }
}
QStringList OutputScanner::takeLines() { QStringList out; out.swap(lines_); return out; }
QString OutputScanner::tail() const { return decode(tailBytes_); }

Prompt OutputScanner::classifyTail(const QString& tail) {
    const QString t = tail.trimmed().toLower();
    if (t.isEmpty()) return Prompt::None;
    if (t.endsWith(QLatin1String("steam guard code:"))) return Prompt::GuardEmail;
    if (t.endsWith(QLatin1String("two-factor code:"))) return Prompt::GuardApp;
    if (t.endsWith(QLatin1String("password:"))) return Prompt::Password;
    return Prompt::None;
}

Line OutputScanner::classifyLine(const QString& line) {
    const QString l = line.toLower();
    const auto has = [&](const char* s) { return l.contains(QLatin1String(s)); };
    if (has("rate limit exceeded")) return Line::RateLimited;
    if (has("timed out waiting for confirmation") || has("wait for confirmation timed out")) return Line::MobileTimedOut;
    if (has("two-factor code mismatch") || has("invalid login auth code")) return Line::BadGuardCode;
    if (has("invalid password")) return Line::BadPassword;
    if (has("please confirm the login in the steam mobile app")) return Line::MobileConfirm;
    if (has("waiting for confirmation")) return Line::MobileWaiting;
    if (has("account logon denied")) return Line::GuardWanted;
    if (has("no subscription")) return Line::NoSubscription;
    if (has("logged in ok") || (has("logging in user") && l.endsWith(QLatin1String("ok"))) || (has("waiting for user info") && l.endsWith(QLatin1String("ok"))))
        return Line::LoginOk;
    if (has("nopromptforpassword") || has("no cached credentials") || has("no cached login")) return Line::NoCachedLogin;
    if (has("success! app") && (has("fully installed") || has("already up to date"))) return Line::JobOk;
    if (has("failed to install app") || (has("error! app") && has("state is") && has("after update job")) || has("failed to request app info"))
        return Line::JobFailed;
    if (has("login failure") || (has("logging in user") && has("failed")) || has("failed login")) return Line::LoginFailed;
    return Line::Other;
}

Progress OutputScanner::parseProgress(const QString& line) {
    static const QRegularExpression re(QStringLiteral(R"(Update state \(0x([0-9a-fA-F]+)\) ([A-Za-z ]+), progress: ([0-9]+(?:\.[0-9]+)?) \((\d+) / (\d+)\))"));
    Progress p;
    const auto m = re.match(line);
    if (!m.hasMatch()) return p;
    bool ok = false;
    p.stateCode = m.captured(1).toInt(&ok, 16);
    p.label = m.captured(2).trimmed();
    p.percent = std::clamp(m.captured(3).toDouble(), 0.0, 100.0);
    p.done = m.captured(4).toLongLong();
    p.total = m.captured(5).toLongLong();
    p.valid = true;
    return p;
}

QString OutputScanner::failureText(const QString& line) {
    static const QRegularExpression re(QStringLiteral(R"((?:FAILED|ERROR|Error!?|ERROR!)\s*\(([^)]*)\))"), QRegularExpression::CaseInsensitiveOption);
    const auto m = re.match(line);
    if (m.hasMatch() && !m.captured(1).trimmed().isEmpty()) return m.captured(1).trimmed();
    return line.trimmed();
}

// ---- command lines -------------------------------------------------------------------------------------------
QStringList signInArguments(const QString& username) {
    return {QStringLiteral("+@ShutdownOnFailedCommand"), QStringLiteral("1"),
            QStringLiteral("+login"), username, QStringLiteral("+quit")};
}
QStringList jobArguments(const QString& username, const QString& appId, const QString& forceInstallDir, bool validate) {
    QStringList a{QStringLiteral("+@ShutdownOnFailedCommand"), QStringLiteral("1"),
                  QStringLiteral("+@NoPromptForPassword"), QStringLiteral("1"),
                  QStringLiteral("+@sSteamCmdForcePlatformType"), QStringLiteral("windows"),
                  QStringLiteral("+force_install_dir"), QDir::toNativeSeparators(forceInstallDir),
                  QStringLiteral("+login"), username,
                  QStringLiteral("+app_update"), appId};
    if (validate) a << QStringLiteral("validate");
    a << QStringLiteral("+quit");
    return a;
}

// ---- finding steamcmd.exe ------------------------------------------------------------------------------------
bool looksLikeSteamClientFolder(const QString& folder) {
    return !folder.isEmpty() && QFileInfo::exists(QDir(folder).filePath(QStringLiteral("steam.exe")));
}
QString resolveExecutable(const QString& pathOrFolder) {
    const QString p = pathOrFolder.trimmed();
    if (p.isEmpty()) return {};
    const QFileInfo info(p);
    if (info.isDir()) {
        const QString exe = QDir(info.absoluteFilePath()).filePath(QStringLiteral("steamcmd.exe"));
        return QFileInfo(exe).isFile() ? QDir::cleanPath(exe) : QString();
    }
    if (info.isFile() && info.fileName().compare(QStringLiteral("steamcmd.exe"), Qt::CaseInsensitive) == 0)
        return QDir::cleanPath(info.absoluteFilePath());
    return {};
}
QStringList candidateExecutables(const QString& configured, const QString& dataDir, const QStringList& extraFolders) {
    QStringList folders;
    if (!configured.isEmpty()) folders << configured;
    if (!dataDir.isEmpty()) folders << QDir(dataDir).filePath(QStringLiteral("steamcmd"));
    folders << extraFolders;
    folders << QStringLiteral("C:/steamcmd") << QStringLiteral("C:/SteamCMD") << QStringLiteral("D:/SteamCMD")
            << QStringLiteral("C:/Program Files (x86)/SteamCMD") << QStringLiteral("C:/Program Files/SteamCMD")
            << QDir::home().filePath(QStringLiteral("steamcmd")) << QDir::home().filePath(QStringLiteral("SteamCMD"))
            << QDir::home().filePath(QStringLiteral("Downloads/steamcmd"));
    const QString onPath = QStandardPaths::findExecutable(QStringLiteral("steamcmd"));
    if (!onPath.isEmpty()) folders << onPath;
    QStringList out;
    for (const QString& f : folders) {
        const QString exe = resolveExecutable(f);
        if (exe.isEmpty() || out.contains(exe, Qt::CaseInsensitive)) continue;
        if (looksLikeSteamClientFolder(QFileInfo(exe).absolutePath())) continue;   // the full client's folder is never SteamCMD's
        out << exe;
    }
    return out;
}
QString findExecutable(const QString& configured, const QString& dataDir, const QStringList& extraFolders) {
    const auto all = candidateExecutables(configured, dataDir, extraFolders);
    return all.isEmpty() ? QString() : all.first();
}

// ---- zip check -----------------------------------------------------------------------------------------------
namespace {
quint32 le32(const QByteArray& b, qsizetype o) {
    return quint32(quint8(b[o])) | quint32(quint8(b[o + 1])) << 8 | quint32(quint8(b[o + 2])) << 16 | quint32(quint8(b[o + 3])) << 24;
}
quint16 le16(const QByteArray& b, qsizetype o) { return quint16(quint8(b[o]) | quint8(b[o + 1]) << 8); }
}
bool zipHoldsSteamcmd(const QByteArray& zip, QString* error) {
    const auto fail = [&](const QString& why) { if (error) *error = why; return false; };
    if (zip.size() < 100) return fail(QStringLiteral("The download is too small to be SteamCMD."));
    if (zip.size() > kMaxZipBytes) return fail(QStringLiteral("The download is larger than SteamCMD's installer should be."));
    if (!zip.startsWith(QByteArray("PK\x03\x04", 4))) return fail(QStringLiteral("The download is not a zip file."));
    qsizetype eocd = -1;
    for (qsizetype i = zip.size() - 22; i >= 0 && i >= zip.size() - 22 - 65535; --i)
        if (zip.mid(i, 4) == QByteArray("PK\x05\x06", 4)) { eocd = i; break; }
    if (eocd < 0) return fail(QStringLiteral("The zip has no directory."));
    const int entries = le16(zip, eocd + 10);
    qsizetype pos = qsizetype(le32(zip, eocd + 16));
    bool found = false;
    for (int i = 0; i < entries; ++i) {
        if (pos < 0 || pos + 46 > zip.size() || zip.mid(pos, 4) != QByteArray("PK\x01\x02", 4)) return fail(QStringLiteral("The zip directory is damaged."));
        const int nameLen = le16(zip, pos + 28), extraLen = le16(zip, pos + 30), commentLen = le16(zip, pos + 32);
        if (pos + 46 + nameLen > zip.size()) return fail(QStringLiteral("The zip directory is damaged."));
        const QString name = QString::fromUtf8(zip.mid(pos + 46, nameLen));
        // An entry that names a drive, an absolute path or a parent folder could be written outside the folder it is
        // unpacked into: the whole file is refused, not just that entry.
        if (name.startsWith(QLatin1Char('/')) || name.startsWith(QLatin1Char('\\')) || name.contains(QLatin1Char(':')) || name.contains(QStringLiteral("..")))
            return fail(QStringLiteral("The zip names a file outside its own folder."));
        if (name.compare(QStringLiteral("steamcmd.exe"), Qt::CaseInsensitive) == 0) found = true;
        pos += 46 + nameLen + extraLen + commentLen;
    }
    if (found) return true;
    return fail(QStringLiteral("The zip does not contain steamcmd.exe."));
}

// ---- libraries and manifests ---------------------------------------------------------------------------------
QStringList steamLibraries(const QString& steamRoot) {
    QStringList out;
    const auto add = [&](const QString& p) {
        const QString clean = QDir::cleanPath(p.trimmed());
        if (clean.isEmpty() || !QFileInfo(clean).isDir()) return;
        for (const QString& e : out) if (e.compare(clean, Qt::CaseInsensitive) == 0) return;
        out << clean;
    };
    if (steamRoot.isEmpty()) return out;
    add(steamRoot);
    QByteArray bytes;
    if (!steamvdf::readBounded(QDir(steamRoot).filePath(QStringLiteral("steamapps/libraryfolders.vdf")), 4 * 1024 * 1024, &bytes, nullptr, false)) return out;
    steamvdf::Options options;
    options.skipBom = true;
    options.maxDepth = 32;
    options.maxTokenBytes = 2 * 1024 * 1024;
    options.maxEntries = 20000;
    options.maxTokens = 100000;
    const auto doc = steamvdf::parse(bytes, options);
    if (!doc.error.isEmpty()) return out;
    const auto* folders = steamvdf::findEntry(doc.entries, QStringLiteral("libraryfolders"), Qt::CaseInsensitive);
    if (!folders || !folders->block) return out;
    for (const auto& f : folders->children) {
        const QString path = f.block ? steamvdf::stringValue(f.children, QStringLiteral("path"), Qt::CaseInsensitive) : f.value;
        add(path);
        if (out.size() >= 16) break;
    }
    return out;
}
QString installFolderFor(const QString& library, const QString& folderName) {
    return QDir::cleanPath(library + QStringLiteral("/steamapps/common/") + folderName);
}
Integration integrateManifest(const QString& library, const QString& forcedFolder, const QString& appId) {
    Integration r;
    if (!validAppId(appId)) { r.detail = QStringLiteral("Not a valid app id."); return r; }
    const QString name = QStringLiteral("appmanifest_") + appId + QStringLiteral(".acf");
    const QString inLibrary = QDir(library).filePath(QStringLiteral("steamapps/") + name);
    if (QFileInfo::exists(inLibrary)) { r.kind = Integration::Kind::AlreadyKnown; r.detail = QStringLiteral("The Steam library already has a manifest for this game."); return r; }
    const QString produced = QDir(forcedFolder).filePath(QStringLiteral("steamapps/") + name);
    QByteArray bytes;
    if (!steamvdf::readBounded(produced, 1024 * 1024, &bytes, nullptr, false)) {
        r.kind = Integration::Kind::NoManifest;
        r.detail = QStringLiteral("SteamCMD left no appmanifest_%1.acf, so the Steam client has nothing to recognise.").arg(appId);
        return r;
    }
    steamvdf::Options options;
    options.skipBom = true;
    const auto doc = steamvdf::parse(bytes, options);
    const auto* state = doc.error.isEmpty() ? steamvdf::findEntry(doc.entries, QStringLiteral("AppState"), Qt::CaseInsensitive) : nullptr;
    const QString installdir = state ? steamvdf::stringValue(state->children, QStringLiteral("installdir"), Qt::CaseInsensitive) : QString();
    if (installdir.isEmpty() || installdir.compare(QFileInfo(forcedFolder).fileName(), Qt::CaseInsensitive) != 0) {
        r.kind = Integration::Kind::Mismatch;
        r.detail = QStringLiteral("The manifest names the folder \"%1\" but the files are in \"%2\", so it was not added to the Steam library.")
                       .arg(installdir, QFileInfo(forcedFolder).fileName());
        return r;
    }
    const QString temp = inLibrary + QStringLiteral(".xmb-tmp");
    QFile::remove(temp);
    if (!QFile::copy(produced, temp) || !QFile::rename(temp, inLibrary)) {
        QFile::remove(temp);
        r.kind = Integration::Kind::CopyFailed;
        r.detail = QStringLiteral("The manifest could not be copied into the Steam library.");
        return r;
    }
    r.kind = Integration::Kind::Linked;
    r.detail = QStringLiteral("The manifest was copied into the Steam library; the Steam client reads it when it starts or rescans.");
    return r;
}

// ---- Run -----------------------------------------------------------------------------------------------------
Run::Run(QObject* parent) : QObject(parent) {
    process_.setProcessChannelMode(QProcess::MergedChannels);
    killTimer_.setSingleShot(true);
    connect(&process_, &QProcess::readyRead, this, &Run::onOutput);
    connect(&process_, &QProcess::finished, this, &Run::onFinished);
    connect(&killTimer_, &QTimer::timeout, this, [this] {
        if (process_.state() == QProcess::NotRunning) return;
        process_.kill();
    });
}
Run::~Run() {
    disconnect(&process_, nullptr, this, nullptr);
    if (process_.state() != QProcess::NotRunning) { process_.kill(); process_.waitForFinished(2000); }
    if (!held_.isEmpty()) { held_.detach(); volatile char* p = held_.data(); for (qsizetype i = 0; i < held_.size(); ++i) p[i] = 0; }
}

void Run::setState(State state) {
    if (state_ == state) return;
    state_ = state;
    emit stateChanged(int(state));
}

QString Run::scrub(const QString& line, const QByteArray& secret) {
    if (secret.isEmpty()) return line;
    const QString s = QString::fromUtf8(secret);
    if (s.isEmpty()) return line;
    QString out = line;
    out.replace(s, QStringLiteral("<hidden>"));
    return out;
}

bool Run::start(Mode mode, const QString& exe, const QStringList& arguments, const QString& workDir) {
    mode_ = mode;
    finishedOnce_ = cancelling_ = sawLoginOk_ = sawMobile_ = false;
    pending_ = lastAnswered_ = Prompt::None;
    lineOutcome_ = Outcome::None;
    lineMessage_.clear();
    message_.clear();
    progress_ = {};
    scanner_ = OutputScanner();
    setState(State::Starting);
    process_.setProgram(exe);
    process_.setArguments(arguments);
    process_.setWorkingDirectory(workDir.isEmpty() ? QFileInfo(exe).absolutePath() : workDir);
    process_.start(QIODevice::ReadWrite);
    if (!process_.waitForStarted(5000)) {
        finish(Outcome::NotStarted, process_.errorString());
        return false;
    }
    return true;
}

bool Run::provide(Secret& secret) {
    if (pending_ == Prompt::None || process_.state() != QProcess::Running || secret.isEmpty()) { secret.wipe(); return false; }
    held_ = secret.bytes();
    held_.detach();
    QByteArray line = secret.bytes();
    line.detach();
    line.push_back('\n');
    const qint64 written = process_.write(line);
    process_.waitForBytesWritten(2000);
    volatile char* p = line.data();
    for (qsizetype i = 0; i < line.size(); ++i) p[i] = 0;
    secret.wipe();
    if (written < 0) return false;
    lastAnswered_ = pending_;
    pending_ = Prompt::None;
    scanner_.clearTail();
    lineOutcome_ = Outcome::None;
    setState(State::Working);
    return true;
}

void Run::cancel() {
    if (state_ == State::Done || process_.state() == QProcess::NotRunning) { if (!finishedOnce_) finish(Outcome::Cancelled, QString()); return; }
    cancelling_ = true;
    process_.terminate();                 // a console child without windows ignores this on Windows ...
    killTimer_.start(1500);               // ... so it is killed if it is still there shortly after
}

void Run::onOutput() {
    scanner_.feed(process_.readAll());
    for (const QString& raw : scanner_.takeLines()) handleLine(scrub(raw, held_));
    checkTail();
}

void Run::handleLine(const QString& line) {
    emit lineSeen(line);
    const Progress p = OutputScanner::parseProgress(line);
    if (p.valid) {
        progress_ = p;
        if (state_ != State::Working) setState(State::Working);
        emit progressChanged();
        return;
    }
    switch (OutputScanner::classifyLine(line)) {
    case Line::LoginOk: sawLoginOk_ = true; if (state_ != State::Working) setState(State::Working); break;
    case Line::MobileConfirm: case Line::MobileWaiting:
        sawMobile_ = true;
        if (state_ != State::WaitingMobile) setState(State::WaitingMobile);
        break;
    case Line::MobileTimedOut: lineOutcome_ = Outcome::MobileTimedOut; lineMessage_ = line; killTimer_.start(4000); break;
    case Line::BadPassword: lineOutcome_ = mode_ == Mode::Job ? Outcome::NeedsSignIn : Outcome::BadPassword; lineMessage_ = line; killTimer_.start(4000); break;
    case Line::BadGuardCode: lineOutcome_ = Outcome::BadGuardCode; lineMessage_ = line; killTimer_.start(4000); break;
    case Line::RateLimited: lineOutcome_ = Outcome::RateLimited; lineMessage_ = line; killTimer_.start(4000); break;
    case Line::NoCachedLogin: case Line::LoginFailed:
        lineOutcome_ = mode_ == Mode::Job ? Outcome::NeedsSignIn : Outcome::Failed; lineMessage_ = line; killTimer_.start(4000); break;
    case Line::NoSubscription: case Line::JobFailed:
        lineOutcome_ = Outcome::Failed; lineMessage_ = line; break;
    case Line::JobOk: if (mode_ == Mode::Job) { lineOutcome_ = Outcome::JobDone; lineMessage_ = line; } break;
    case Line::GuardWanted: case Line::Other: break;
    }
}

void Run::checkTail() {
    const Prompt prompt = OutputScanner::classifyTail(scanner_.tail());
    if (prompt == Prompt::None || prompt == pending_) return;
    if (mode_ == Mode::Job) {
        // A job runs on SteamCMD's own cached login: any request for a secret means that cache is gone or stale.
        finish(Outcome::NeedsSignIn, QStringLiteral("SteamCMD asked for a password or code, so its saved sign-in is missing or has expired."));
        return;
    }
    // A new prompt after a failure line (a wrong code that SteamCMD asks again for) is not the end of the run.
    if (lineOutcome_ == Outcome::BadGuardCode || lineOutcome_ == Outcome::BadPassword) { message_ = lineMessage_; lineOutcome_ = Outcome::None; killTimer_.stop(); }
    pending_ = prompt;
    scanner_.clearTail();
    setState(prompt == Prompt::Password ? State::WantPassword : State::WantGuard);
    emit promptRequested(int(prompt));
}

void Run::onFinished(int exitCode, QProcess::ExitStatus status) {
    killTimer_.stop();
    // Anything still buffered.
    scanner_.feed(process_.readAll());
    for (const QString& raw : scanner_.takeLines()) handleLine(scrub(raw, held_));
    if (finishedOnce_) return;
    if (cancelling_) { finish(Outcome::Cancelled, QString()); return; }
    if (lineOutcome_ != Outcome::None && lineOutcome_ != Outcome::JobDone) { finish(lineOutcome_, OutputScanner::failureText(lineMessage_)); return; }
    if (mode_ == Mode::SignIn && sawLoginOk_ && exitCode == 0 && status == QProcess::NormalExit) { finish(Outcome::SignedIn, QString()); return; }
    if (mode_ == Mode::Job && lineOutcome_ == Outcome::JobDone && exitCode == 0) { finish(Outcome::JobDone, lineMessage_); return; }
    finish(Outcome::ExitedEarly, QStringLiteral("SteamCMD ended (exit code %1) without finishing.").arg(exitCode));
}

void Run::finish(Outcome outcome, const QString& message) {
    if (finishedOnce_) return;
    finishedOnce_ = true;
    killTimer_.stop();
    if (!held_.isEmpty()) { held_.detach(); volatile char* p = held_.data(); for (qsizetype i = 0; i < held_.size(); ++i) p[i] = 0; held_.clear(); }
    pending_ = Prompt::None;
    message_ = message;
    setState(State::Done);
    if (process_.state() != QProcess::NotRunning) { process_.kill(); process_.waitForFinished(2000); }
    emit finished(int(outcome), message);
}

}   // namespace xmb::steamcmd
