// PlayStation Network column pages: the pure logic only (sign-in form, Account Management tree, wording).
//   xmb-psn-pages-tests
#include "psn_pages.h"

#include <QCoreApplication>
#include <QSet>
#include <cstdio>

using namespace xmb::psn;

namespace {
int checks = 0, failures = 0;
void check(bool condition, const char* label) {
    ++checks;
    if (!condition) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", label);
    }
}
bool saysNotConnected(const QString& text) {
    return text.contains(QStringLiteral("PlayStation Network")) && text.contains(QStringLiteral("not")) &&
           (text.contains(QStringLiteral("does not")) || text.contains(QStringLiteral("not connect")));
}
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);

    // ---- Sign-in form: focus walk --------------------------------------------------------------------------
    SignInForm form;
    check(form.focus == Field::Email && form.phase == Phase::Form, "starts on the e-mail field, in the form");
    check(!form.autoSignIn && !form.savePasswordChecked(), "nothing is pre-ticked: nothing was ever saved");
    form.move(-1);
    check(form.focus == Field::Email, "Up on the first row stays");
    for (int i = 0; i < 10; ++i) form.move(1);
    check(form.focus == Field::Forgot, "Down stops on the last row, no wrap");
    form.move(-1);
    check(form.focus == Field::SignIn, "Up from Forgot reaches Sign In");
    form.move(-1);
    check(form.focus == Field::SavePassword, "Up from Sign In reaches Save Password");

    // ---- Check boxes ---------------------------------------------------------------------------------------
    check(form.toggle(Field::SavePassword) && form.savePasswordChecked(), "Save Password ticks");
    check(form.toggle(Field::SavePassword) && !form.savePasswordChecked(), "Save Password unticks");
    check(!form.toggle(Field::Email) && !form.toggle(Field::SignIn), "only the two boxes toggle");
    check(form.toggle(Field::AutoSignIn) && form.autoSignIn, "Auto Sign-In ticks");
    check(form.savePasswordChecked() && !form.fieldEnabled(Field::SavePassword), "Auto Sign-In shows Save Password ticked and dimmed");
    check(!form.toggle(Field::SavePassword) && form.savePasswordChecked(), "the dimmed box cannot be toggled");
    form.focus = Field::AutoSignIn;
    form.move(1);
    check(form.focus == Field::SignIn, "Down from Auto Sign-In skips the dimmed box");
    form.move(-1);
    check(form.focus == Field::AutoSignIn, "Up skips the dimmed box too");
    form.focus = Field::SavePassword;
    form.normaliseFocus();
    check(form.focus == Field::SignIn, "focus on the dimmed box is moved off it");
    check(form.toggle(Field::AutoSignIn) && !form.savePasswordChecked(), "Auto Sign-In off returns Save Password to its own value");

    // ---- Typed values never show --------------------------------------------------------------------------
    form.setEmail(QStringLiteral("someone@example.invalid"));
    form.setPassword(QStringLiteral("hunter2xyz"));
    check(form.maskedPassword() == QStringLiteral("**********"), "one asterisk per character");
    check(!form.maskedPassword().contains(QLatin1Char('h')) && !form.maskedPassword().contains(QLatin1Char('x')), "the mask never holds a typed character");
    form.setPassword(QString(100, QLatin1Char('a')));
    check(form.password.size() == kMaxPasswordLength, "the password is capped");
    form.setEmail(QString(500, QLatin1Char('b')));
    check(form.email.size() == kMaxEmailLength, "the e-mail is capped");

    // ---- Submit and the only ending -----------------------------------------------------------------------
    check(form.phase == Phase::Form && form.submit() && form.phase == Phase::Signing, "Sign In starts the pause");
    check(!form.submit(), "a second Sign In while signing is ignored");
    form.cancelSigning();
    check(form.phase == Phase::Form, "Circle while signing returns to the form");
    check(kSigningMilliseconds > 0 && kSigningMilliseconds < 3000, "the pause is short and finite");
    check(saysNotConnected(signInFailureText()), "the sign-in ending says it is not connected");
    check(signInFailureText() == notConnectedText(), "the ending is the column's existing wording");
    check(signInFailureText().contains(QStringLiteral("no account to sign in to")), "the wording is the long-standing sentence");
    check(!signInFailureText().contains(QStringLiteral("success"), Qt::CaseInsensitive) &&
              !signInFailureText().contains(QStringLiteral("signed in as"), Qt::CaseInsensitive),
          "no success wording anywhere");
    check(saysNotConnected(forgotPasswordText()), "Forgot your password says it is not connected");

    // ---- Wipe ---------------------------------------------------------------------------------------------
    form.setEmail(QStringLiteral("a@b.invalid"));
    form.setPassword(QStringLiteral("pw"));
    form.autoSignIn = true;
    form.wipe();
    check(form.email.isEmpty() && form.password.isEmpty() && !form.autoSignIn && form.focus == Field::Email, "wipe clears everything typed");

    // ---- Account Management tree --------------------------------------------------------------------------
    const auto root = accountRoot();
    const QStringList expectedRoot = {QStringLiteral("Account Information"), QStringLiteral("Redeem Codes"),
                                      QStringLiteral("Transaction Management"), QStringLiteral("System Activation"),
                                      QStringLiteral("Facebook"), QStringLiteral("Privacy Settings")};
    check(root.size() == expectedRoot.size(), "six entries under Account Management");
    bool order = root.size() == expectedRoot.size();
    for (int i = 0; order && i < root.size(); ++i) order = root[i].title == expectedRoot[i];
    check(order, "the entries come in the recorded order");
    check(root[0].hasChildren && !root[1].hasChildren && !root[5].hasChildren, "only Account Information has a list under it");
    check(root[1].description == QStringLiteral("Redeem a PSN Card number or promotion code."), "Redeem Codes carries its description line");
    const auto info = accountInformation();
    check(info.size() == 7, "Account Information lists seven rows");
    bool noAvatar = true;
    for (const auto& n : info) noAvatar = noAvatar && !n.title.contains(QStringLiteral("Avatar"));
    check(noAvatar, "no Avatar picker: it would need an account's own pictures");

    QSet<QString> ids;
    bool allLeavesTruthful = true, allHaveArt = true, noFakeData = true;
    auto scan = [&](const QVector<Node>& nodes) {
        for (const auto& node : nodes) {
            ids.insert(node.id);
            allHaveArt = allHaveArt && !node.container.isEmpty() && !node.texture.isEmpty();
            if (!node.hasChildren) allLeavesTruthful = allLeavesTruthful && saysNotConnected(leafText(node.title)) && leafText(node.title).contains(node.title);
            const QString all = node.title + node.description + leafText(node.title);
            noFakeData = noFakeData && !all.contains(QLatin1Char('@')) && !all.contains(QStringLiteral("$")) && !all.contains(QStringLiteral("balance"), Qt::CaseInsensitive);
        }
    };
    scan(root);
    scan(info);
    check(ids.size() == treeSize(), "every node id is unique");
    check(allLeavesTruthful, "every leaf names itself and says there is no PlayStation Network connection");
    check(allHaveArt, "every row names a firmware texture");
    check(noFakeData, "no e-mail address, price or balance appears anywhere in the tree");

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
