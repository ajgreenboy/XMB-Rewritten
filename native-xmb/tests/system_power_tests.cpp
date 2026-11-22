#include "system_power.h"
#include <QCoreApplication>
#include <QVector>
#include <cstdio>

// System-power tests. Build with -DXMB_SYSTEM_POWER_BUILD_TESTS=ON; exit code 0 = all pass.
// Nothing here ever suspends, shuts down or locks the host: the dry-run path short-circuits every
// request, and on Linux the logind/systemctl command mapping is asserted through the runner seam
// (an in-test recorder) rather than by spawning systemctl/loginctl.
using namespace xmb;

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    int failures = 0;
    const auto check = [&](bool ok, const char* what) {
        if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++failures; }
    };

    // ---- dry run: every request is accepted without touching the OS -------------------------------
    check(!powerDryRun(), "dry run off by default");
    setPowerDryRun(true);
    check(powerDryRun(), "setPowerDryRun(true) takes effect");
    {
        QString error;
        check(requestRestart(&error), "dry-run restart accepted");
        check(requestShutdown(&error), "dry-run shutdown accepted");
        check(lockWorkstation(&error), "dry-run lock accepted");
        check(turnDisplayOff(&error), "dry-run display off accepted");
    }
    setPowerDryRun(false);
    check(!powerDryRun(), "setPowerDryRun(false) takes effect");

#ifdef Q_OS_LINUX
    // ---- logind/systemctl command mapping (never executed) ---------------------------------------
    {
        const LinuxPowerCommand suspend = linuxPowerCommand(LinuxPowerAction::Sleep);
        check(suspend.valid && suspend.program == QLatin1String("systemctl") &&
              suspend.arguments == QStringList{QStringLiteral("suspend")}, "sleep maps to systemctl suspend");
        const LinuxPowerCommand hibernate = linuxPowerCommand(LinuxPowerAction::Hibernate);
        check(hibernate.program == QLatin1String("systemctl") &&
              hibernate.arguments == QStringList{QStringLiteral("hibernate")}, "hibernate maps to systemctl hibernate");
        const LinuxPowerCommand restart = linuxPowerCommand(LinuxPowerAction::Restart);
        check(restart.program == QLatin1String("systemctl") &&
              restart.arguments == QStringList{QStringLiteral("reboot")}, "restart maps to systemctl reboot");
        const LinuxPowerCommand shutdown = linuxPowerCommand(LinuxPowerAction::Shutdown);
        check(shutdown.program == QLatin1String("systemctl") &&
              shutdown.arguments == QStringList{QStringLiteral("poweroff")}, "shutdown maps to systemctl poweroff");
        const LinuxPowerCommand lock = linuxPowerCommand(LinuxPowerAction::Lock);
        check(lock.program == QLatin1String("loginctl") &&
              lock.arguments == QStringList{QStringLiteral("lock-session")}, "lock maps to loginctl lock-session");
    }

    // ---- request routing: requests hand the right action to the runner ---------------------------
    {
        QVector<LinuxPowerAction> seen;
        setLinuxPowerRunnerForTest([&](LinuxPowerAction action, QString*) { seen.append(action); return true; });
        QString error;
        check(requestShutdown(&error), "shutdown routed and accepted");
        check(requestRestart(&error), "restart routed and accepted");
        check(lockWorkstation(&error), "lock routed and accepted");
        check(seen == QVector<LinuxPowerAction>{LinuxPowerAction::Shutdown, LinuxPowerAction::Restart,
                                               LinuxPowerAction::Lock},
              "requests route Shutdown/Restart/Lock in order");
        setLinuxPowerRunnerForTest({});
    }

    // ---- sleep never reaches the host in a test ---------------------------------------------------
    {
        QString error;
        bool runnerCalled = false;
        setLinuxPowerRunnerForTest([&](LinuxPowerAction action, QString*) {
            runnerCalled = (action == LinuxPowerAction::Sleep);
            return true;
        });
        const bool ok = requestSleep(false, &error);
        if (canSleep()) {
            check(ok && runnerCalled, "sleep routed to the runner when /sys/power/state advertises mem");
        } else {
            check(!ok && !runnerCalled && !error.isEmpty(), "sleep refused up front when mem is not advertised");
        }
        setLinuxPowerRunnerForTest({});
    }
#endif

    std::fprintf(stdout, failures ? "system-power tests: %d FAILED\n" : "system-power tests: all passed\n", failures);
    return failures ? 1 : 0;
}
