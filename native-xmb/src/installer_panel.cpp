#include "installer_panel.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QRegularExpression>
#include <algorithm>

namespace xmb::installerpanel {

// ---- Log ---------------------------------------------------------------------------------------------------------
void InnoLogTracker::reset() {
    state_ = {};
    carry_.clear();
    first_ = true;
    expectDest_ = false;
}

void InnoLogTracker::feed(const QByteArray& chunk) {
    carry_ += chunk;
    if (first_ && !carry_.isEmpty()) {
        if (carry_.startsWith("\xEF\xBB\xBF")) carry_.remove(0, 3);
        first_ = false;
    }
    // A single line can never legitimately be this long; drop it rather than grow without bound.
    if (carry_.size() > 1 << 20 && !carry_.contains('\n')) carry_.clear();
    qsizetype start = 0;
    for (;;) {
        const qsizetype end = carry_.indexOf('\n', start);
        if (end < 0) break;
        QByteArray line = carry_.mid(start, end - start);
        if (line.endsWith('\r')) line.chop(1);
        parseLine(QString::fromUtf8(line));
        start = end + 1;
    }
    carry_.remove(0, start);
}

void InnoLogTracker::parseLine(const QString& raw) {
    static const QRegularExpression stamp(QStringLiteral(R"(^\s*\d{4}-\d\d-\d\d \d\d:\d\d:\d\d(?:\.\d+)?\s+)"));
    static const QRegularExpression percent(QStringLiteral(R"((?<![\d.])(\d{1,3})\s?%)"));
    static const QRegularExpression percentContext(QStringLiteral("extract|unpack|decompress|progress|install|copy"),
                                                   QRegularExpression::CaseInsensitiveOption);
    QString line = raw;
    line.remove(stamp);
    line = line.trimmed();
    if (line.isEmpty()) return;
    state_.seen = true;

    const auto startsWith = [&](const char* text) { return line.startsWith(QLatin1String(text)); };
    if (startsWith("Starting the installation process")) {
        state_.installing = true;
        state_.step = QStringLiteral("Starting the installation");
    } else if (startsWith("Creating directory:")) {
        state_.step = QStringLiteral("Creating folders");
    } else if (startsWith("-- File entry --")) {
        ++state_.fileEntries;
        state_.step = QStringLiteral("Copying files");
        expectDest_ = true;
    } else if (startsWith("Dest filename:")) {
        const QString name = QFileInfo(line.mid(14).trimmed()).fileName();
        if (!name.isEmpty()) state_.item = name;
        if (expectDest_) expectDest_ = false;
        state_.step = QStringLiteral("Copying files");
    } else if (startsWith("-- Icon entry --")) {
        state_.step = QStringLiteral("Creating shortcuts");
    } else if (startsWith("-- INI entry --") || startsWith("-- Registry entry --")) {
        state_.step = QStringLiteral("Writing settings");
    } else if (startsWith("-- Run entry --")) {
        state_.step = QStringLiteral("Running post-install steps");
    } else if (startsWith("-- Language entry --") || startsWith("-- Message entry --")) {
        // Setup bookkeeping, not an install step.
    } else if (startsWith("Installation process succeeded")) {
        state_.succeeded = true;
        state_.step = QStringLiteral("Installation finished");
    } else if (startsWith("Installation process failed")) {
        state_.failed = true;
        state_.step = QStringLiteral("Installation failed");
    } else if (startsWith("Rolling back changes")) {
        state_.failed = true;
        state_.rollingBack = true;
        state_.step = QStringLiteral("Rolling back changes");
    } else if (startsWith("Exception message:")) {
        state_.failed = true;
        state_.error = line.mid(18).trimmed().left(200);
    } else if (startsWith("Log closed")) {
        state_.closed = true;
    }

    // A percentage is taken only from a line that talks about extraction/progress and carries one; the last such line
    // wins. Inno itself does not log one, so this serves repack front-ends that do (unverified against real repacks).
    if (percentContext.match(line).hasMatch()) {
        const auto m = percent.match(line);
        if (m.hasMatch()) {
            const int value = m.captured(1).toInt();
            if (value >= 0 && value <= 100) state_.percent = value;
        }
    }
}

// ---- Status ------------------------------------------------------------------------------------------------------
QString formatElapsed(qint64 ms) {
    if (ms < 0) ms = 0;
    const qint64 total = ms / 1000;
    const qint64 h = total / 3600, m = (total / 60) % 60, s = total % 60;
    if (h > 0) return QStringLiteral("%1:%2:%3").arg(h).arg(m, 2, 10, QLatin1Char('0')).arg(s, 2, 10, QLatin1Char('0'));
    return QStringLiteral("%1:%2").arg(m).arg(s, 2, 10, QLatin1Char('0'));
}

QString formatBytes(qint64 bytes) {
    if (bytes < 0) return {};
    if (bytes < 1024) return QStringLiteral("%1 B").arg(bytes);
    static const char* const units[] = {"KB", "MB", "GB", "TB"};
    double value = double(bytes);
    int unit = -1;
    while (value >= 1024.0 && unit < 3) { value /= 1024.0; ++unit; }
    return QStringLiteral("%1 %2").arg(value, 0, 'f', value < 10 ? 2 : 1).arg(QLatin1String(units[unit]));
}

StatusView deriveStatus(const StatusInputs& in) {
    StatusView view;
    if (in.terminating) view.headline = QStringLiteral("Force closing the installer");
    else if (in.awaitingForceConfirm) view.headline = QStringLiteral("The installer did not close; waiting for your answer");
    else if (in.running && in.closeRequested) view.headline = QStringLiteral("Close requested; waiting for the installer to exit");
    else if (!in.running) view.headline = QStringLiteral("The installer has exited");
    else if (in.logAvailable && in.log.rollingBack) view.headline = QStringLiteral("The installer reports it is rolling back");
    else if (in.logAvailable && in.log.failed && !in.log.error.isEmpty()) view.headline = QStringLiteral("The installer reports a problem");
    else if (in.logAvailable && in.log.succeeded) view.headline = QStringLiteral("The installer reports it finished; still running");
    else if (in.logAvailable && !in.log.step.isEmpty()) view.headline = in.log.step;
    else view.headline = QStringLiteral("The installer is running");

    view.lines.push_back({QStringLiteral("Elapsed"), formatElapsed(in.elapsedMs)});
    if (in.logAvailable) {
        if (!in.log.step.isEmpty() && in.log.step != view.headline) view.lines.push_back({QStringLiteral("Step"), in.log.step});
        if (!in.log.item.isEmpty()) view.lines.push_back({QStringLiteral("Current file"), in.log.item});
        if (in.log.percent >= 0) view.lines.push_back({QStringLiteral("Progress"), QStringLiteral("%1% (from the installer's log)").arg(in.log.percent)});
        if (in.log.fileEntries > 0) view.lines.push_back({QStringLiteral("Files in log"), QString::number(in.log.fileEntries)});
        if (!in.log.error.isEmpty()) view.lines.push_back({QStringLiteral("Installer message"), in.log.error});
    }
    if (in.bytesWritten >= 0)
        view.lines.push_back({QStringLiteral("Written to destination"),
                              (in.bytesLowerBound ? QStringLiteral("at least ") : QString()) + formatBytes(in.bytesWritten)});
    return view;
}

// ---- Exit --------------------------------------------------------------------------------------------------------
Outcome classifyExit(const ExitInputs& in) {
    if (in.forced) return Outcome::Cancelled;
    if (in.crashed) return in.closeRequested ? Outcome::Cancelled : Outcome::Crashed;
    const bool logFailed = in.logAvailable && in.log.failed;
    if (in.exitCode == 0) return (in.destinationHasFiles && !logFailed) ? Outcome::Succeeded : Outcome::ExitedUnverified;
    if (in.closeRequested && !(in.logAvailable && in.log.succeeded)) return Outcome::Cancelled;
    return Outcome::Failed;
}

QString outcomeText(Outcome outcome, const ExitInputs& in, const QString& destination, const QString& logPath, qint64 bytesWritten) {
    const QString written = bytesWritten >= 0 ? QStringLiteral(" (%1)").arg(formatBytes(bytesWritten)) : QString();
    const QString log = logPath.isEmpty() ? QString() : QStringLiteral(" Log: %1").arg(logPath);
    switch (outcome) {
    case Outcome::Succeeded:
        return QStringLiteral("The installer exited with code 0 and the destination now holds files%1. Nothing else was checked: "
                              "Register Installed Game looks for a game executable in %2.").arg(written, destination);
    case Outcome::ExitedUnverified:
        if (in.logAvailable && in.log.failed)
            return QStringLiteral("The installer exited with code 0 but its log reports a problem%1. This is not treated as installed.%2")
                .arg(in.log.error.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(in.log.error), log);
        return QStringLiteral("The installer exited with code 0 but the destination holds no files. This is not treated as installed.%1").arg(log);
    case Outcome::Failed:
        return QStringLiteral("The installer failed: exit code %1.%2%3").arg(in.exitCode)
            .arg(in.logAvailable && !in.log.error.isEmpty() ? QStringLiteral(" It said: %1.").arg(in.log.error) : QString(), log);
    case Outcome::Crashed:
        return QStringLiteral("The installer crashed (exit code %1).%2").arg(in.exitCode).arg(log);
    case Outcome::Cancelled:
        return QStringLiteral("The installer was closed before it finished%1. Files it already wrote to %2 were left in place and may be a "
                              "half-finished install; nothing was deleted.")
            .arg(in.forced ? QStringLiteral(" (it had to be force closed)") : QString(), destination);
    }
    return {};
}

// ---- Close escalation --------------------------------------------------------------------------------------------
CloseEscalation::Action CloseEscalation::requestClose(qint64 nowMs) {
    switch (stage_) {
    case Stage::Idle:
    case Stage::TerminateFailed:
        // A new request after a declined or failed attempt starts over with the polite stage.
        stage_ = Stage::PoliteSent;
        stamp_ = nowMs;
        requested_ = true;
        return Action::SendClose;
    case Stage::AwaitingConfirm:
        return Action::AskForce;     // pressed again while the question was pending: ask again
    case Stage::PoliteSent:          // already asked politely; the grace period is still running
    case Stage::Terminating:
    case Stage::Done:
        return Action::None;
    }
    return Action::None;
}

CloseEscalation::Action CloseEscalation::tick(qint64 nowMs, bool alive) {
    switch (stage_) {
    case Stage::PoliteSent:
        if (!alive) { stage_ = Stage::Done; return Action::None; }
        if (nowMs - stamp_ >= grace_) { stage_ = Stage::AwaitingConfirm; return Action::AskForce; }
        return Action::None;
    case Stage::Terminating:
        if (!alive) { stage_ = Stage::Done; return Action::None; }
        if (nowMs - stamp_ >= terminateWait_) stage_ = Stage::TerminateFailed;
        return Action::None;
    case Stage::AwaitingConfirm:
        if (!alive) stage_ = Stage::Done;
        return Action::None;
    default:
        return Action::None;
    }
}

CloseEscalation::Action CloseEscalation::confirmForce(qint64 nowMs) {
    if (stage_ != Stage::AwaitingConfirm) return Action::None;   // never terminate without the question having been asked
    stage_ = Stage::Terminating;
    stamp_ = nowMs;
    forced_ = true;
    return Action::Terminate;
}

void CloseEscalation::declineForce() {
    if (stage_ != Stage::AwaitingConfirm) return;
    stage_ = Stage::Idle;
    requested_ = false;
}

void CloseEscalation::processExited() { stage_ = Stage::Done; }

// ---- Tree / folder -----------------------------------------------------------------------------------------------
QList<steamquit::ProcessInfo> installerTree(const QList<steamquit::ProcessInfo>& snapshot, quint32 rootPid, quint64 rootStart) {
    QList<steamquit::ProcessInfo> set;
    for (const auto& p : snapshot)
        if (p.pid == rootPid && (rootStart == 0 || p.startTime == 0 || p.startTime == rootStart)) { set.push_back(p); break; }
    if (set.isEmpty()) return set;
    for (bool grew = true; grew;) {
        grew = false;
        for (const auto& p : snapshot) {
            const bool known = std::any_of(set.cbegin(), set.cend(), [&](const auto& q) { return q.pid == p.pid; });
            if (known || !p.pid) continue;
            const auto parent = std::find_if(set.cbegin(), set.cend(), [&](const auto& q) { return q.pid == p.parentPid; });
            if (parent == set.cend()) continue;
            if (parent->startTime && p.startTime && p.startTime < parent->startTime) continue;   // recycled parent id
            set.push_back(p);
            grew = true;
        }
    }
    return set;
}

qint64 directoryBytes(const QString& path, int maxEntries, bool* lowerBound) {
    if (lowerBound) *lowerBound = false;
    if (path.isEmpty() || !QFileInfo(path).isDir()) return -1;
    qint64 total = 0;
    int seen = 0;
    QDirIterator it(path, QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks | QDir::Hidden | QDir::System,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        if (++seen > maxEntries) { if (lowerBound) *lowerBound = true; break; }
        const QFileInfo info = it.fileInfo();
        if (info.isFile()) total += info.size();
    }
    return total;
}

bool directoryHasFiles(const QString& path) {
    if (path.isEmpty() || !QFileInfo(path).isDir()) return false;
    QDirIterator it(path, QDir::Files | QDir::NoDotAndDotDot | QDir::NoSymLinks | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
    return it.hasNext();
}

}
