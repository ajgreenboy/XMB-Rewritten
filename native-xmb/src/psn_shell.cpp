// PlayStation Network column pages (psn_pages.h). There is no PSN connection in this build, so none of this
// produces an account, a balance, a user or a session: Sign In always ends in the truthful "not connected"
// page, and every Account Management leaf says what connection it would need.
#include "shell.h"
#include "assets.h"
#include "psn_pages.h"
#include <QDebug>

namespace xmb {

// The rows of one of the Account Management lists. `which` is "root" (the six entries under Account Management)
// or "info" (Account Information's own list); a node with children pushes a page, any other raises its leaf page.
static QVector<Entry> psnRows(const QString& which) {
    QVector<Entry> rows;
    const auto nodes=which==QLatin1String("info")?psn::accountInformation():psn::accountRoot();
    for(const auto& node:nodes) {
        Entry row;
        row.id=QStringLiteral("psn:")+node.id;row.title=node.title;row.detail=node.description;
        row.container=node.container;row.texture=node.texture;
        if(node.hasChildren){row.action=Action::PsnPage;row.value=QStringLiteral("info");}
        else{row.action=Action::PsnLeaf;row.value=node.title;}
        rows.push_back(std::move(row));
    }
    return rows;
}

void Shell::openPsnPage(const QString& which) {
    if(pages_.size()>=32)return;
    Page page;
    page.title=which==QLatin1String("info")?QObject::tr("Account Information")
                                             :assets_.text("explore_category_psn","msg_account_manage","Account Management");
    page.entries=psnRows(which);
    pages_.push_back(std::move(page));
    qInfo().noquote()<<"psn: page"<<which<<"opened,"<<pages_.last().entries.size()<<"rows; no PlayStation Network connection, every leaf explains it";
}

bool Shell::activatePsn(const Entry& entry) {
    switch(entry.action) {
    case Action::PsnSignIn:
        qInfo().noquote()<<"psn: sign-in page requested (nothing is sent anywhere; the page always ends in the not-connected page)";
        emit requestPsnSignIn();
        return true;
    case Action::PsnPage:
        openPsnPage(entry.value);
        return true;
    case Action::PsnLeaf:
        qInfo().noquote()<<"psn: leaf"<<entry.id<<"explained as needing a PlayStation Network connection";
        showMessage(entry.title,psn::leafText(entry.title),MessageKind::Page);
        emit changed();
        return true;
    default:return false;
    }
}

void Shell::psnSignInFailed() {
    qInfo().noquote()<<"psn: sign-in ended: not connected (no request was made; typed values were not logged)";
    showMessage(psn::signInFailureTitle(),psn::signInFailureText(),MessageKind::Page);
    emit changed();
}

void Shell::psnForgotPassword() {
    showMessage(QObject::tr("Forgot your password?"),psn::forgotPasswordText(),MessageKind::Page);
    emit changed();
}

} // namespace xmb
