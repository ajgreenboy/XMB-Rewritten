#pragma once
#include <QObject>
#include <QString>
#include <QVector>
#include <algorithm>

// The PlayStation Network column's pages, the older firmware look (audit/PSN-PAGES.md).
//
// This build has NO PlayStation Network connection and never will say otherwise. Everything here is presentation:
//   - the Sign In page (a form that collects nothing it can use and always ends in the truthful "not connected" page);
//   - the Account Management tree (rows that each explain the connection they would need).
// No account, balance, user or session is ever produced, so nothing here can look signed in.
//
// The pure logic is in this header so it is tested without a window (tests/psn_pages_tests.cpp). The drawing is in
// view.cpp (drawPsnSignInPage) and the shell side is in psn_shell.cpp.
namespace xmb::psn {

// The wording the Sign In row has always answered with (shell.cpp's unavailableReason("signin") now reads this).
inline QString notConnectedText() {
    return QObject::tr("This application does not connect to PlayStation Network, so there is no account to sign in to.");
}

// ---- The Sign In form ------------------------------------------------------------------------------------------
// Row order top to bottom, as the older firmware draws it (recording m2, 51 s).
enum class Field { Email, Password, AutoSignIn, SavePassword, SignIn, Forgot };
constexpr int kFieldCount=6;
// "Signing in... Please wait." stays up this long before the truthful failure replaces it. It is a pause the page
// needs to be read in, not a request: nothing is sent anywhere.
constexpr int kSigningMilliseconds=900;
// A typed value is capped the way the console's own field is.
constexpr int kMaxEmailLength=128,kMaxPasswordLength=32;

enum class Phase { Form, Signing };

struct SignInForm {
    // Held in this process's memory only while the page is open, never written, logged or sent. wipe() zeroes them.
    QString email,password;
    bool autoSignIn=false;
    bool savePasswordOwn=false;
    Field focus=Field::Email;
    Phase phase=Phase::Form;

    // Auto Sign-In implies a saved password, so the Save Password box shows ticked and dimmed while it is on
    // (recording m2: the box under a ticked Auto Sign-In is dim). INFERRED from that one frame.
    bool savePasswordChecked() const { return autoSignIn || savePasswordOwn; }
    bool fieldEnabled(Field field) const { return !(field==Field::SavePassword && autoSignIn); }
    // One bullet per typed character, never the characters.
    QString maskedPassword() const { return QString(password.size(),QLatin1Char('*')); }

    // Up/Down walk the rows without wrapping and skip a dimmed one.
    void move(int direction) {
        const int step=direction<0?-1:1;
        int next=int(focus)+step;
        while(next>=0 && next<kFieldCount && !fieldEnabled(Field(next)))next+=step;
        if(next>=0 && next<kFieldCount)focus=Field(next);
    }
    // Cross on a check box. Returns false when the row is not a check box or is dimmed.
    bool toggle(Field field) {
        if(!fieldEnabled(field))return false;
        if(field==Field::AutoSignIn){autoSignIn=!autoSignIn;return true;}
        if(field==Field::SavePassword){savePasswordOwn=!savePasswordOwn;return true;}
        return false;
    }
    void setEmail(const QString& value){email=value.left(kMaxEmailLength);}
    void setPassword(const QString& value){password.fill(QChar(0));password=value.left(kMaxPasswordLength);}
    // Cross on Sign In. There is no connection, so this only starts the pause before the failure page.
    bool submit() {
        if(phase==Phase::Signing)return false;
        phase=Phase::Signing;
        return true;
    }
    void cancelSigning(){phase=Phase::Form;}
    // If Auto Sign-In is on and focus sits on the dimmed row, step off it.
    void normaliseFocus() { if(!fieldEnabled(focus))move(1); }
    void wipe() {
        password.fill(QChar(0));password.clear();
        email.fill(QChar(0));email.clear();
        autoSignIn=savePasswordOwn=false;focus=Field::Email;phase=Phase::Form;
    }
};

// What the page ends with, whatever was typed. There is no success outcome in this build.
inline QString signInFailureTitle() { return QObject::tr("Sign In"); }
inline QString signInFailureText() { return notConnectedText(); }
inline QString forgotPasswordText() {
    return QObject::tr("Resetting a password needs PlayStation Network, which this application does not connect to, so there is nothing to reset.");
}

// ---- The Account Management tree -------------------------------------------------------------------------------
// The sub-list the older firmware pushes under Account Management, as the newer recording's tree lays it out (its
// layout is the only thing taken from that recording). Every node here is a row that needs a PSN connection.
struct Node {
    QString id,title,description,container,texture;
    bool hasChildren=false;
};

inline QVector<Node> accountRoot() {
    return {
        {QStringLiteral("account-info"),QObject::tr("Account Information"),{},QStringLiteral("explore_category_psn"),QStringLiteral("item_tex_account"),true},
        {QStringLiteral("account-redeem"),QObject::tr("Redeem Codes"),QObject::tr("Redeem a PSN Card number or promotion code."),QStringLiteral("newstore_plugin"),QStringLiteral("ButtonIconRedeem"),false},
        {QStringLiteral("account-transactions"),QObject::tr("Transaction Management"),{},QStringLiteral("newstore_plugin"),QStringLiteral("ButtonIconCart"),false},
        {QStringLiteral("account-activation"),QObject::tr("System Activation"),{},QStringLiteral("explore_plugin_full"),QStringLiteral("item_tex_ps3sd_folder"),false},
        {QStringLiteral("account-facebook"),QObject::tr("Facebook"),{},QStringLiteral("newstore_plugin"),QStringLiteral("FacebookLogo"),false},
        {QStringLiteral("account-privacy"),QObject::tr("Privacy Settings"),{},QStringLiteral("explore_plugin_full"),QStringLiteral("tex_lock_icon"),false},
    };
}
// Account Information's own list. The Avatar picker is not built: it would need a signed-in account's own pictures.
inline QVector<Node> accountInformation() {
    const auto leaf=[](const char* id,const QString& title){
        return Node{QStringLiteral("info-")+QLatin1String(id),title,{},QStringLiteral("explore_category_psn"),QStringLiteral("item_tex_account"),false};
    };
    return {
        leaf("billing",QObject::tr("Billing Information")),
        leaf("signin-id",QObject::tr("Sign-In ID (E-mail Address)")),
        leaf("password",QObject::tr("Password")),
        leaf("name",QObject::tr("Name")),
        leaf("address",QObject::tr("Address")),
        leaf("profile",QObject::tr("Profile")),
        leaf("notifications",QObject::tr("Notification Preferences")),
    };
}
// The page a leaf row raises. It names the row and says why there is nothing to show; it never shows a value.
inline QString leafText(const QString& title) {
    return QObject::tr("%1 needs a PlayStation Network connection, which this application does not make, so there is nothing to show or change here.").arg(title);
}
// How many nodes the whole tree holds (root rows plus Account Information's list), for the log line.
inline int treeSize() { return int(accountRoot().size()+accountInformation().size()); }

} // namespace xmb::psn
