#include "steam_switch.h"
#include "steam_accounts.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QPair>
#include <QProcess>
#include <QSaveFile>
#include <QSettings>
#include <algorithm>
#include <utility>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <tlhelp32.h>
#endif

namespace xmb {
namespace {

constexpr qsizetype MaximumFileBytes = 1024 * 1024;
constexpr qsizetype MaximumTokens = 20000;
constexpr int MaximumDepth = 8;
constexpr qulonglong SteamIdBase = 76561197960265728ULL;

// ---- A small span-keeping reader for the KeyValues text ------------------------------------------
// The shared parser (steam_vdf.h) returns decoded values and no positions; an in-place rewrite needs the
// byte span of every token, so this reader keeps them. The dialect is the same: whitespace, // comments,
// quoted strings with only \" and \\ as escapes, bare words that end at whitespace, a brace or //.
struct Tok {
    enum Kind { Str, Open, Close } kind = Str;
    qsizetype start = 0;
    qsizetype end = 0; // one past the last byte
    bool quoted = false;
    QByteArray text; // decoded
};

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f'; }

bool lex(const QByteArray& b, QVector<Tok>* out, QString* error) {
    qsizetype i = 0;
    if (b.startsWith("\xEF\xBB\xBF")) i = 3;
    const qsizetype n = b.size();
    while (i < n) {
        const char c = b.at(i);
        if (isSpace(c)) { ++i; continue; }
        if (c == '/' && i + 1 < n && b.at(i + 1) == '/') {
            while (i < n && b.at(i) != '\n') ++i;
            continue;
        }
        if (out->size() >= MaximumTokens) { *error = QStringLiteral("the file has too many tokens"); return false; }
        Tok t;
        t.start = i;
        if (c == '{' || c == '}') {
            t.kind = c == '{' ? Tok::Open : Tok::Close;
            t.end = ++i;
        } else if (c == '"') {
            t.quoted = true;
            ++i;
            bool closed = false;
            while (i < n) {
                const char d = b.at(i);
                if (d == '\\' && i + 1 < n && (b.at(i + 1) == '"' || b.at(i + 1) == '\\')) {
                    t.text.append(b.at(i + 1));
                    i += 2;
                } else if (d == '"') {
                    closed = true;
                    ++i;
                    break;
                } else {
                    t.text.append(d);
                    ++i;
                }
            }
            if (!closed) { *error = QStringLiteral("a quoted string is not closed"); return false; }
            t.end = i;
        } else {
            while (i < n) {
                const char d = b.at(i);
                if (isSpace(d) || d == '{' || d == '}' || (d == '/' && i + 1 < n && b.at(i + 1) == '/')) break;
                t.text.append(d);
                ++i;
            }
            t.end = i;
        }
        out->append(std::move(t));
    }
    return true;
}

struct Node {
    int key = -1;   // token index of the key
    int value = -1; // token index of the value (non-block)
    int open = -1;  // token index of '{' (block)
    int close = -1; // token index of '}' (block)
    bool block = false;
    QVector<Node> kids;
};

bool parseEntries(const QVector<Tok>& toks, int* pos, int depth, bool inBlock, QVector<Node>* out,
                  QString* error) {
    if (depth > MaximumDepth) { *error = QStringLiteral("blocks are nested too deeply"); return false; }
    while (*pos < toks.size()) {
        const Tok& t = toks.at(*pos);
        if (t.kind == Tok::Close) {
            if (!inBlock) { *error = QStringLiteral("a closing brace has no block"); return false; }
            return true; // the caller consumes it
        }
        if (t.kind == Tok::Open) { *error = QStringLiteral("a block has no key"); return false; }
        Node node;
        node.key = (*pos)++;
        if (*pos >= toks.size()) { *error = QStringLiteral("a key has no value"); return false; }
        const Tok& v = toks.at(*pos);
        if (v.kind == Tok::Str) {
            node.value = (*pos)++;
        } else if (v.kind == Tok::Open) {
            node.block = true;
            node.open = (*pos)++;
            if (!parseEntries(toks, pos, depth + 1, true, &node.kids, error)) return false;
            if (*pos >= toks.size() || toks.at(*pos).kind != Tok::Close) {
                *error = QStringLiteral("a block is not closed");
                return false;
            }
            node.close = (*pos)++;
        } else {
            *error = QStringLiteral("a key has no value");
            return false;
        }
        out->append(std::move(node));
    }
    if (inBlock) { *error = QStringLiteral("a block is not closed"); return false; }
    return true;
}

bool parseDocument(const QByteArray& bytes, QVector<Tok>* toks, QVector<Node>* root, QString* error) {
    if (!lex(bytes, toks, error)) return false;
    int pos = 0;
    return parseEntries(*toks, &pos, 0, false, root, error);
}

bool truthy(const QByteArray& value) {
    const QByteArray v = value.trimmed();
    return v == "1" || v.toLower() == "true";
}

struct Edit {
    qsizetype pos = 0;
    qsizetype remove = 0;
    QByteArray insert;
};

bool allBlank(const QByteArray& b, qsizetype from, qsizetype to) {
    for (qsizetype i = from; i < to; ++i)
        if (b.at(i) != ' ' && b.at(i) != '\t') return false;
    return true;
}

qsizetype lineStart(const QByteArray& b, qsizetype pos) {
    if (pos <= 0) return 0;
    const qsizetype nl = b.lastIndexOf('\n', pos - 1);
    return nl < 0 ? 0 : nl + 1;
}

QByteArray quoteAs(bool quoted, const QByteArray& text) {
    return quoted ? QByteArray("\"") + text + QByteArray("\"") : text;
}

const Node* findKid(const Node& block, const QVector<Tok>& toks, const char* name, int which = 0) {
    int seen = 0;
    for (const Node& kid : block.kids) {
        if (kid.block) continue;
        if (toks.at(kid.key).text == name && seen++ == which) return &kid;
    }
    return nullptr;
}

// Builds the text that adds `pairs` as new entries at the end of `block`, in the block's own style.
Edit insertionFor(const QByteArray& bytes, const QVector<Tok>& toks, const Node& block,
                  const QVector<QPair<QByteArray, QByteArray>>& pairs) {
    const Tok& close = toks.at(block.close);
    const QByteArray eol = bytes.contains("\r\n") ? QByteArray("\r\n") : QByteArray("\n");
    const Node* model = nullptr;
    for (qsizetype i = block.kids.size() - 1; i >= 0; --i) {
        if (!block.kids.at(i).block) { model = &block.kids.at(i); break; }
    }
    const qsizetype closeLine = lineStart(bytes, close.start);
    const bool ownLine = closeLine > 0 && allBlank(bytes, closeLine, close.start);

    Edit edit;
    if (!ownLine) {
        // Compact block ("{ "a" "b" }" or "{...}" on one line): add inside the brace.
        QByteArray text;
        const bool needSpace = close.start > 0 && !isSpace(bytes.at(close.start - 1));
        if (needSpace) text.append(' ');
        const bool keyQuoted = model ? toks.at(model->key).quoted : true;
        const bool valueQuoted = model ? toks.at(model->value).quoted : true;
        for (const auto& pair : pairs)
            text += quoteAs(keyQuoted, pair.first) + ' ' + quoteAs(valueQuoted, pair.second) + ' ';
        edit.pos = close.start;
        edit.insert = text;
        return edit;
    }
    QByteArray indent;
    QByteArray separator("\t\t");
    bool keyQuoted = true;
    bool valueQuoted = true;
    if (model) {
        const Tok& key = toks.at(model->key);
        const Tok& value = toks.at(model->value);
        const qsizetype keyLine = lineStart(bytes, key.start);
        if (allBlank(bytes, keyLine, key.start)) indent = bytes.mid(keyLine, key.start - keyLine);
        else indent = bytes.mid(closeLine, close.start - closeLine) + '\t';
        const QByteArray gap = bytes.mid(key.end, value.start - key.end);
        if (!gap.isEmpty() && !gap.contains('\n') && allBlank(gap, 0, gap.size())) separator = gap;
        keyQuoted = key.quoted;
        valueQuoted = value.quoted;
    } else {
        indent = bytes.mid(closeLine, close.start - closeLine) + '\t';
    }
    QByteArray text;
    for (const auto& pair : pairs)
        text += indent + quoteAs(keyQuoted, pair.first) + separator + quoteAs(valueQuoted, pair.second) + eol;
    edit.pos = closeLine;
    edit.insert = text;
    return edit;
}

void failureOf(LoginUsersRewrite& r, const QString& why) {
    r.ok = false;
    r.changed = false;
    r.error = why;
}

} // namespace

LoginUsersRewrite rewriteLoginUsersForSwitch(const QByteArray& original, const QString& steamId64,
                                             bool requireRemembered) {
    LoginUsersRewrite r;
    r.bytes = original;
    if (original.size() > MaximumFileBytes) { failureOf(r, QStringLiteral("the account file is too large")); return r; }
    QVector<Tok> toks;
    QVector<Node> root;
    QString error;
    if (!parseDocument(original, &toks, &root, &error)) {
        failureOf(r, QStringLiteral("the account file could not be parsed: %1").arg(error));
        return r;
    }
    const Node* users = nullptr;
    for (const Node& n : root) {
        if (n.block && toks.at(n.key).text == "users") { users = &n; break; }
    }
    if (!users) { failureOf(r, QStringLiteral("the account file has no users block")); return r; }

    const QByteArray wanted = steamId64.trimmed().toUtf8();
    const Node* target = nullptr;
    QVector<const Node*> accounts;
    for (const Node& u : users->kids) {
        if (!u.block) continue;
        accounts.append(&u);
        if (toks.at(u.key).text.trimmed() == wanted) {
            if (target) { failureOf(r, QStringLiteral("the account appears twice in the account file")); return r; }
            target = &u;
        }
    }
    r.usersSeen = static_cast<int>(accounts.size());
    if (wanted.isEmpty() || !target) {
        failureOf(r, QStringLiteral("the account is not in the account file"));
        return r;
    }
    const Node* name = findKid(*target, toks, "AccountName");
    if (!name || toks.at(name->value).text.trimmed().isEmpty()) {
        failureOf(r, QStringLiteral("the account has no login name in the account file"));
        return r;
    }
    r.accountName = QString::fromUtf8(toks.at(name->value).text).trimmed();
    const Node* remember = findKid(*target, toks, "RememberPassword");
    r.rememberPassword = remember && truthy(toks.at(remember->value).text);
    const Node* offline = findKid(*target, toks, "WantsOfflineMode");
    r.wantsOfflineMode = offline && truthy(toks.at(offline->value).text);
    if (requireRemembered && !r.rememberPassword) {
        failureOf(r, QStringLiteral("the account has no saved sign-in"));
        return r;
    }

    const auto anyAccountHas = [&](const char* key) {
        for (const Node* a : accounts)
            if (findKid(*a, toks, key)) return true;
        return false;
    };
    const bool mirrorOffline = anyAccountHas("WantsOfflineMode");
    const bool mirrorSkipWarning = anyAccountHas("SkipOfflineModeWarning");
    // Current Steam clients mark the auto-sign-in account with "AutoLogin" (and have no MostRecent at all);
    // older ones use MostRecent and AllowAutoLogin. The target gets all that the task needs, and AutoLogin is kept
    // exclusive to it whenever the file uses that key.
    const bool usesAutoLogin = anyAccountHas("AutoLogin");

    QVector<Edit> edits;
    // A value is replaced in place, keeping its quoting; a missing key is inserted once per block.
    const auto setValue = [&](const Node& account, const char* key, const char* value,
                              QVector<QPair<QByteArray, QByteArray>>* missing) {
        bool found = false;
        for (int which = 0;; ++which) {
            const Node* kid = findKid(account, toks, key, which);
            if (!kid) break;
            found = true;
            const Tok& v = toks.at(kid->value);
            if (v.text == value) continue;
            Edit e;
            e.pos = v.start;
            e.remove = v.end - v.start;
            e.insert = quoteAs(v.quoted, value);
            edits.append(e);
        }
        if (!found && missing) missing->append({QByteArray(key), QByteArray(value)});
    };

    for (const Node* account : accounts) {
        QVector<QPair<QByteArray, QByteArray>> missing;
        if (account == target) {
            setValue(*account, "MostRecent", "1", &missing);
            setValue(*account, "AllowAutoLogin", "1", &missing);
            if (usesAutoLogin) setValue(*account, "AutoLogin", "1", &missing);
            if (mirrorOffline && !findKid(*account, toks, "WantsOfflineMode"))
                missing.append({QByteArray("WantsOfflineMode"), QByteArray("0")});
            if (mirrorSkipWarning && !findKid(*account, toks, "SkipOfflineModeWarning"))
                missing.append({QByteArray("SkipOfflineModeWarning"), QByteArray("0")});
        } else {
            // Only a truthy MostRecent or AutoLogin is rewritten; a missing or already-false one is left alone.
            for (const char* exclusive : {"MostRecent", "AutoLogin"}) {
                for (int which = 0;; ++which) {
                    const Node* kid = findKid(*account, toks, exclusive, which);
                    if (!kid) break;
                    const Tok& v = toks.at(kid->value);
                    if (!truthy(v.text)) continue;
                    Edit e;
                    e.pos = v.start;
                    e.remove = v.end - v.start;
                    e.insert = quoteAs(v.quoted, "0");
                    edits.append(e);
                }
            }
        }
        if (!missing.isEmpty()) edits.append(insertionFor(original, toks, *account, missing));
    }

    std::sort(edits.begin(), edits.end(), [](const Edit& a, const Edit& b) { return a.pos > b.pos; });
    QByteArray out = original;
    for (const Edit& e : edits) out.replace(e.pos, e.remove, e.insert);
    r.changed = out != original;
    r.bytes = out;

    // The result must still be a well-formed document that names the same target (belt and braces).
    QVector<Tok> toks2;
    QVector<Node> root2;
    QString error2;
    if (!parseDocument(out, &toks2, &root2, &error2)) {
        r.bytes = original;
        failureOf(r, QStringLiteral("the rewritten account file would not parse: %1").arg(error2));
        return r;
    }
    r.ok = true;
    return r;
}

// ---- Registry --------------------------------------------------------------------------------------
qulonglong steamActiveUserId(const QString& registryKey) {
    QSettings active(registryKey + QStringLiteral("\\ActiveProcess"), QSettings::NativeFormat);
    if (active.status() != QSettings::NoError) return 0;
    bool valid = false;
    const qulonglong id = active.value(QStringLiteral("ActiveUser")).toULongLong(&valid);
    return valid && id <= 0xffffffffULL ? id : 0;
}

// ---- Processes --------------------------------------------------------------------------------------
namespace {

class SystemProcessControl : public SteamProcessControl {
public:
    explicit SystemProcessControl(QStringList startArguments) : startArguments_(std::move(startArguments)) {}
    bool steamRunning(QString* which) override {
#ifdef Q_OS_WIN
        const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE) return false;
        PROCESSENTRY32W entry;
        entry.dwSize = sizeof(entry);
        bool found = false;
        if (Process32FirstW(snapshot, &entry)) {
            do {
                const QString name = QString::fromWCharArray(entry.szExeFile);
                if (name.compare(QStringLiteral("steam.exe"), Qt::CaseInsensitive) == 0 ||
                    name.compare(QStringLiteral("steamwebhelper.exe"), Qt::CaseInsensitive) == 0) {
                    if (which) *which = name.toLower();
                    found = true;
                    break;
                }
            } while (Process32NextW(snapshot, &entry));
        }
        CloseHandle(snapshot);
        return found;
#else
        Q_UNUSED(which);
        return false;
#endif
    }
    bool requestExit(const QString& steamExe, QString* error) override {
        if (QProcess::startDetached(steamExe, steamShutdownArguments(), QFileInfo(steamExe).absolutePath()))
            return true;
        if (error) *error = QStringLiteral("steam.exe -shutdown could not be started");
        return false;
    }
    bool launch(const QString& steamExe, QString* error) override {
        if (QProcess::startDetached(steamExe, steamRelaunchArguments() + startArguments_, QFileInfo(steamExe).absolutePath()))
            return true;
        if (error) *error = QStringLiteral("steam.exe could not be started");
        return false;
    }

private:
    QStringList startArguments_;
};

class NullProcessControl : public SteamProcessControl {
public:
    // Steam is "running" only after the (logged, not performed) launch, so the verify step can be exercised.
    bool steamRunning(QString*) override { return launched_; }
    bool requestExit(const QString&, QString*) override { return true; }
    bool launch(const QString& steamExe, QString*) override {
        qInfo().noquote() << "steam: XMB_STEAM_SWITCH_NOPROC: would start" << steamExe << "(nothing started)";
        launched_ = true;
        return true;
    }

private:
    bool launched_ = false;
};

} // namespace

std::unique_ptr<SteamProcessControl> makeSystemSteamProcessControl(const QStringList& startArguments) {
    return std::make_unique<SystemProcessControl>(startArguments);
}
std::unique_ptr<SteamProcessControl> makeNullSteamProcessControl() {
    return std::make_unique<NullProcessControl>();
}

// ---- The switch -------------------------------------------------------------------------------------
SteamAccountSwitcher::SteamAccountSwitcher(SteamSwitchOptions options,
                                           std::unique_ptr<SteamProcessControl> process, QObject* parent)
    : QObject(parent), options_(std::move(options)), process_(std::move(process)) {
    qRegisterMetaType<xmb::SteamSwitchResult>();
    timer_.setSingleShot(false);
    connect(&timer_, &QTimer::timeout, this, &SteamAccountSwitcher::tick);
}

SteamAccountSwitcher::~SteamAccountSwitcher() = default;

QString SteamAccountSwitcher::loginUsersPath() const {
    return QDir(options_.steamRoot).filePath(QStringLiteral("config/loginusers.vdf"));
}
QString SteamAccountSwitcher::registryKey() const {
    return options_.registryKey.isEmpty() ? steamRegistryKeyPath() : options_.registryKey;
}

void SteamAccountSwitcher::finish(SteamSwitchResult::Outcome outcome, bool ok, const QString& message) {
    if (stage_ == Stage::Done) return;
    timer_.stop();
    stage_ = Stage::Done;
    result_.outcome = outcome;
    result_.ok = ok;
    result_.message = message;
    qInfo().noquote() << QStringLiteral("steam: switch finished outcome=%1 ok=%2 restored=%3")
                             .arg(static_cast<int>(outcome)).arg(ok ? 1 : 0).arg(result_.restored ? 1 : 0)
                      << "-" << message;
    emit finished(result_);
}

void SteamAccountSwitcher::start() {
    if (stage_ != Stage::Idle) return;
    clock_.start();
    const QString label = options_.personaName.isEmpty() ? options_.accountName : options_.personaName;

    // Preflight, before anything is closed: the file must name this account, with a saved sign-in.
    LoginUsersRewrite plan;
    if (options_.select) {
        QFile file(loginUsersPath());
        QByteArray bytes;
        bool readable = file.open(QIODevice::ReadOnly) && file.size() <= MaximumFileBytes;
        if (readable) {
            bytes = file.readAll();
            readable = file.error() == QFileDevice::NoError;
        }
        if (!readable) {
            finish(SteamSwitchResult::Outcome::Failed, false,
                   QObject::tr("Steam's account file could not be read, so nothing was changed."));
            return;
        }
        file.close();
        plan = rewriteLoginUsersForSwitch(bytes, options_.steamId64, true);
        if (!plan.ok || plan.accountName.compare(options_.accountName, Qt::CaseInsensitive) != 0) {
            qWarning().noquote() << "steam: switch preflight refused:"
                                 << (plan.ok ? QStringLiteral("login name mismatch") : plan.error);
            finish(SteamSwitchResult::Outcome::Failed, false,
                   QObject::tr("%1 cannot be selected automatically (%2). Nothing was changed.")
                       .arg(label, plan.ok ? QObject::tr("the login name does not match") : plan.error));
            return;
        }
    }
    if (options_.hold) {
        qInfo().noquote() << QStringLiteral("steam: switch held (XMB_STEAM_SWITCH_HOLD): select=%1 file=%2 changed=%3 "
                                            "AutoLoginUser=%4 key=%5; nothing was changed")
                                 .arg(options_.select ? 1 : 0).arg(loginUsersPath()).arg(plan.changed ? 1 : 0)
                                 .arg(options_.accountName, registryKey());
        finish(SteamSwitchResult::Outcome::Held, true,
               QObject::tr("Held: the plan was logged and nothing was changed."));
        return;
    }
    if (options_.steamExe.isEmpty() || !QFileInfo(options_.steamExe).isFile()) {
        finish(SteamSwitchResult::Outcome::Failed, false,
               QObject::tr("steam.exe was not found at %1. Nothing was changed.")
                   .arg(options_.steamExe.isEmpty() ? QObject::tr("(no path)") : options_.steamExe));
        return;
    }

    QString which;
    if (!process_->steamRunning(&which)) {
        qInfo().noquote() << "steam: switch: Steam is not running, the close step is skipped";
        stage_ = Stage::Settling;
        emit progress(QObject::tr("Switching to %1").arg(label));
        timer_.start(0);
        return;
    }
    QString error;
    if (!process_->requestExit(options_.steamExe, &error)) {
        finish(SteamSwitchResult::Outcome::Failed, false,
               QObject::tr("Steam could not be asked to close: %1. Nothing was changed.").arg(error));
        return;
    }
    qInfo().noquote() << "steam: switch: close requested; waiting for" << which << "to exit";
    stage_ = Stage::Closing;
    phaseStart_ = clock_.elapsed();
    emit progress(QObject::tr("Closing Steam"));
    timer_.start(std::max(50, options_.pollMs));
}

void SteamAccountSwitcher::tick() {
    switch (stage_) {
    case Stage::Closing: pollClosing(); break;
    case Stage::Settling: timer_.stop(); writeSelection(); break;
    case Stage::Verifying: pollVerify(); break;
    case Stage::Idle:
    case Stage::Done: timer_.stop(); break;
    }
}

void SteamAccountSwitcher::pollClosing() {
    QString which;
    if (!process_->steamRunning(&which)) {
        qInfo().noquote() << "steam: switch: Steam has exited after" << (clock_.elapsed() - phaseStart_) << "ms";
        stage_ = Stage::Settling;
        timer_.start(std::max(0, options_.settleMs));
        return;
    }
    if (clock_.elapsed() - phaseStart_ >= options_.exitTimeoutMs) {
        qWarning().noquote() << "steam: switch: Steam did not exit in time;" << which << "is still running";
        finish(SteamSwitchResult::Outcome::Failed, false,
               QObject::tr("Steam did not close within %1 seconds (%2 is still running), so nothing was "
                           "changed. Close Steam from its tray icon and try again.")
                   .arg(options_.exitTimeoutMs / 1000).arg(which));
    }
}

void SteamAccountSwitcher::pruneBackups() {
    const QDir dir(QFileInfo(loginUsersPath()).absolutePath());
    const QFileInfoList backups =
        dir.entryInfoList({QStringLiteral("loginusers.vdf.xmb-backup-*")}, QDir::Files, QDir::Name | QDir::Reversed);
    for (int i = std::max(1, options_.keptBackups); i < backups.size(); ++i) QFile::remove(backups.at(i).absoluteFilePath());
}

void SteamAccountSwitcher::rollback(const QString& why) {
    bool fileOk = true;
    bool registryOk = true;
    if (fileWritten_) {
        QSaveFile save(loginUsersPath());
        fileOk = save.open(QIODevice::WriteOnly) && save.write(originalBytes_) == originalBytes_.size() && save.commit();
        if (!fileOk && !result_.backupPath.isEmpty()) {
            QFile::remove(loginUsersPath());
            fileOk = QFile::copy(result_.backupPath, loginUsersPath());
        }
    }
    if (registryWritten_) {
        QSettings steam(registryKey(), QSettings::NativeFormat);
        if (hadAutoLogin_) steam.setValue(QStringLiteral("AutoLoginUser"), oldAutoLogin_);
        else steam.remove(QStringLiteral("AutoLoginUser"));
        if (hadRemember_) steam.setValue(QStringLiteral("RememberPassword"), oldRemember_);
        else steam.remove(QStringLiteral("RememberPassword"));
        steam.sync();
        registryOk = steam.status() == QSettings::NoError;
    }
    result_.restored = fileOk && registryOk;
    result_.changed = false;
    qWarning().noquote() << QStringLiteral("steam: switch: failed (%1); restored file=%2 registry=%3")
                                .arg(why).arg(fileOk ? 1 : 0).arg(registryOk ? 1 : 0);
    QString message = QObject::tr("The switch failed: %1.").arg(why);
    message += result_.restored ? QObject::tr(" Steam's account settings were put back as they were.")
                                : QObject::tr(" Putting Steam's account settings back also failed; the original file "
                                              "copy is %1.").arg(result_.backupPath);
    finish(SteamSwitchResult::Outcome::Failed, false, message);
}

void SteamAccountSwitcher::writeSelection() {
    const QString label = options_.personaName.isEmpty() ? options_.accountName : options_.personaName;
    if (!options_.select) {
        // No saved sign-in: nothing is written. Steam is started again and shows its own window.
        emit progress(QObject::tr("Starting Steam"));
        QString launchError;
        if (!process_->launch(options_.steamExe, &launchError)) {
            finish(SteamSwitchResult::Outcome::Failed, false,
                   QObject::tr("Steam could not be started again: %1").arg(launchError));
            return;
        }
        finish(SteamSwitchResult::Outcome::Started, true,
               QObject::tr("Steam is starting and will show its own sign-in window. Sign in as %1 there once "
                           "and tick Remember my password; after that the XMB can switch to %1 by itself. "
                           "The XMB changed nothing.").arg(label));
        return;
    }
    const QString path = loginUsersPath();
    QFile in(path);
    bool readable = in.open(QIODevice::ReadOnly) && in.size() <= MaximumFileBytes;
    if (readable) {
        originalBytes_ = in.readAll();
        readable = in.error() == QFileDevice::NoError;
    }
    if (!readable) {
        finish(SteamSwitchResult::Outcome::Failed, false,
               QObject::tr("Steam's account file could not be read (%1). Nothing was changed.").arg(in.errorString()));
        return;
    }
    in.close();
    const LoginUsersRewrite rewrite = rewriteLoginUsersForSwitch(originalBytes_, options_.steamId64, true);
    if (!rewrite.ok || rewrite.accountName.compare(options_.accountName, Qt::CaseInsensitive) != 0) {
        finish(SteamSwitchResult::Outcome::Failed, false,
               QObject::tr("The account can no longer be selected (%1). Nothing was changed.")
                   .arg(rewrite.ok ? QObject::tr("the login name changed") : rewrite.error));
        return;
    }
    emit progress(QObject::tr("Selecting the account"));

    // The backup comes first; if it cannot be made, nothing is written.
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
    QString backup = path + QStringLiteral(".xmb-backup-") + stamp;
    for (int n = 2; QFileInfo::exists(backup) && n < 100; ++n)
        backup = path + QStringLiteral(".xmb-backup-") + stamp + QLatin1Char('-') + QString::number(n);
    if (!QFile::copy(path, backup)) {
        finish(SteamSwitchResult::Outcome::Failed, false,
               QObject::tr("A backup copy of Steam's account file could not be made (is the Steam folder "
                           "writable?). Nothing was changed."));
        return;
    }
    result_.backupPath = backup;
    qInfo().noquote() << "steam: switch: backup written" << backup;

    if (rewrite.changed) {
        QSaveFile save(path);
        if (!save.open(QIODevice::WriteOnly)) {
            rollback(QObject::tr("Steam's account file could not be opened for writing (%1)").arg(save.errorString()));
            return;
        }
        if (save.write(rewrite.bytes) != rewrite.bytes.size() || !save.commit()) {
            rollback(QObject::tr("Steam's account file could not be replaced (%1)").arg(save.errorString()));
            return;
        }
        fileWritten_ = true;
        QFile check(path);
        if (!check.open(QIODevice::ReadOnly) || check.readAll() != rewrite.bytes) {
            rollback(QObject::tr("Steam's account file did not read back as written"));
            return;
        }
        qInfo().noquote() << "steam: switch: loginusers.vdf rewritten for account" << rewrite.accountName;
    } else {
        qInfo().noquote() << "steam: switch: loginusers.vdf already selects the account; not rewritten";
    }

    {
        QSettings steam(registryKey(), QSettings::NativeFormat);
        hadAutoLogin_ = steam.contains(QStringLiteral("AutoLoginUser"));
        oldAutoLogin_ = steam.value(QStringLiteral("AutoLoginUser")).toString();
        hadRemember_ = steam.contains(QStringLiteral("RememberPassword"));
        oldRemember_ = steam.value(QStringLiteral("RememberPassword")).toInt();
        registryWritten_ = true; // from here a failure restores whatever was there
        steam.setValue(QStringLiteral("AutoLoginUser"), rewrite.accountName);
        steam.setValue(QStringLiteral("RememberPassword"), 1);
        steam.sync();
        if (steam.status() != QSettings::NoError) {
            rollback(QObject::tr("the Steam registry values could not be written (access denied?)"));
            return;
        }
    }
    {
        const QSettings check(registryKey(), QSettings::NativeFormat);
        if (check.value(QStringLiteral("AutoLoginUser")).toString().compare(rewrite.accountName, Qt::CaseInsensitive) != 0 ||
            check.value(QStringLiteral("RememberPassword")).toInt() != 1) {
            rollback(QObject::tr("the Steam registry values did not read back as written"));
            return;
        }
    }
    result_.changed = true;
    qInfo().noquote() << QStringLiteral("steam: switch: registry AutoLoginUser=%1 RememberPassword=1 written under %2")
                             .arg(rewrite.accountName, registryKey());
    pruneBackups();

    emit progress(QObject::tr("Starting Steam"));
    QString error;
    if (!process_->launch(options_.steamExe, &error)) {
        rollback(error.isEmpty() ? QObject::tr("Steam could not be started again") : error);
        return;
    }
    qInfo().noquote() << "steam: switch: Steam started with no account argument; waiting for ActiveUser";
    stage_ = Stage::Verifying;
    phaseStart_ = clock_.elapsed();
    emit progress(QObject::tr("Waiting for Steam to sign in as %1").arg(label));
    timer_.start(std::max(50, options_.pollMs));
}

void SteamAccountSwitcher::pollVerify() {
    const QString label = options_.personaName.isEmpty() ? options_.accountName : options_.personaName;
    const qulonglong active = steamActiveUserId(registryKey());
    const QString activeId = active ? QString::number(SteamIdBase + active) : QString();
    if (active && activeId == options_.steamId64 && process_->steamRunning(nullptr)) {
        result_.signedInAs = label;
        finish(SteamSwitchResult::Outcome::SignedIn, true, QObject::tr("Signed in as %1.").arg(label));
        return;
    }
    if (clock_.elapsed() - phaseStart_ < options_.verifyTimeoutMs) return;

    QString which;
    const bool running = process_->steamRunning(&which);
    if (active) {
        QString other = activeId;
        for (const SteamAccount& a : scanSteamAccounts(options_.steamRoot))
            if (a.steamId64 == activeId) other = a.personaName.isEmpty() ? a.accountName : a.personaName;
        result_.signedInAs = other;
        finish(SteamSwitchResult::Outcome::OtherAccount, true,
               QObject::tr("Steam signed in as %1, not %2. The selection was written; sign in as %2 "
                           "inside Steam once, then try again.").arg(other, label));
    } else if (running) {
        finish(SteamSwitchResult::Outcome::LoginWindow, true,
               QObject::tr("Steam started but did not sign in as %1; it is probably showing its own sign-in "
                           "window (the saved sign-in may have expired, or Steam Guard needs a code). Sign in "
                           "as %1 there once. The XMB never handles passwords.").arg(label));
    } else {
        finish(SteamSwitchResult::Outcome::NotRunning, true,
               QObject::tr("Steam was started but is not running now, so it did not sign in as %1. Start "
                           "Steam yourself; the selection was written.").arg(label));
    }
}

} // namespace xmb
