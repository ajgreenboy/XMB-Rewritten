// Jellyfin setup inside the XMB (audit/JELLYFIN-SETUP.md; the pattern is Home Assistant's row in home_shell.cpp).
//
// TV/Video Services lists a "Jellyfin" row while no Jellyfin server is registered (shell.cpp refreshContent).
// Cross opens this page, an ordinary page of rows like the sign-in page:
//
//   Server Address        the XMB keyboard; the address is checked as soon as it is accepted
//   (result)              the server's name and version, or one plain sentence on why it cannot be used
//   Add Jellyfin          writes the registration into <data>/apps.json (the --import-apps writer) and refreshes
//                         the column in place, so the normal Jellyfin row (library, sign-in) appears
//   Check Server          asks the address again
//   Look for Servers      Jellyfin's UDP auto-discovery on this network; the servers that answer become rows,
//                         and choosing one fills the address
//
// The same page is "Change Server Address" on a registered Jellyfin row's item menu. Only the server's public
// information is requested; no credential is typed, sent or logged here (sign-in is the separate page).
#include "shell.h"
#include "diagnostics.h"
#include "glyphs.h"
#include "service_icons.h"
#include <QDir>
#include <QFile>
#include <algorithm>

namespace xmb {
namespace {
Entry setupRow(QString id,QString title,Action action,QString detail,QString value={}) {
    Entry e;e.id=std::move(id);e.title=std::move(title);e.action=action;e.detail=std::move(detail);e.value=std::move(value);
    e.container=QStringLiteral("explore_plugin_full");e.texture=QStringLiteral("item_tex_plain_folder");e.source=QStringLiteral("native");
    return e;
}
Entry withIcon(Entry e,const QImage& icon) {if(!icon.isNull()){e.artwork=icon;e.iconSized=true;}return e;}
}

bool Shell::jellyfinSetupOpen() const {
    for(const auto& page:pages_)if(page.jfSetup)return true;
    return false;
}

QVector<Entry> Shell::jellyfinSetupRows() const {
    using jellyfinSetup::SetupState;
    const auto& st=jfSetup_;
    QVector<Entry> rows;
    const auto parsed=jellyfinSetup::parseAddress(st.address);
    {
        // A field is drawn like a settings row: the round bullet, the label, the value on the right.
        Entry e=setupRow(QStringLiteral("jfsetup:address"),tr("Server Address"),Action::JellyfinSetupField,
            tr("Cross to type the address of your Jellyfin server, for example http://192.168.1.20:8096"));
        e.container=QStringLiteral("qgl/icontex");e.texture=QStringLiteral("23n");
        e.fieldValue=st.address.isEmpty()?tr("Not set"):st.address;
        rows.push_back(std::move(e));
    }
    switch(st.check) {
    case SetupState::Check::Idle:break;
    case SetupState::Check::Checking:
        rows.push_back(withIcon(setupRow(QStringLiteral("jfsetup:result"),tr("Checking..."),Action::Information,
            tr("Asking the server who it is"),tr("Waiting for the server to answer.")),glyphs::search()));
        break;
    case SetupState::Check::Ok:
        rows.push_back(withIcon(setupRow(QStringLiteral("jfsetup:result"),st.serverName,Action::Information,
            tr("Jellyfin %1 - found").arg(st.serverVersion),
            tr("%1 is a Jellyfin server, version %2, at %3.").arg(st.serverName,st.serverVersion,st.checkedAddress)),glyphs::check()));
        break;
    case SetupState::Check::Failed:
        rows.push_back(withIcon(setupRow(QStringLiteral("jfsetup:result"),tr("Could Not Use This Address"),Action::Information,
            st.error,st.error),glyphs::warning()));
        break;
    }
    {
        const bool current=parsed.ok && st.checkedAddress==parsed.text;
        const bool confirmed=current && st.check==SetupState::Check::Ok;
        const bool anyway=current && st.check==SetupState::Check::Failed && st.unreachable;
        const bool editing=!st.editId.isEmpty();
        QString title=anyway?tr("Save Anyway"):editing?tr("Save Address"):tr("Add Jellyfin");
        QString detail;
        if(confirmed)detail=editing?tr("Points this Jellyfin at the new address."):tr("Adds Jellyfin to TV/Video Services, where you sign in.");
        else if(anyway)detail=tr("The server did not answer. Saving keeps the address so you can sign in once it is on.");
        else if(st.check==SetupState::Check::Checking)detail=tr("Waiting for the server to answer.");
        else detail=tr("Check the server first. It is saved once it answers as Jellyfin.");
        auto save=withIcon(setupRow(QStringLiteral("jfsetup:save"),title,Action::JellyfinSetupSave,detail),serviceIcons::jellyfin());
        save.disabled=!(confirmed || anyway);
        rows.push_back(std::move(save));
    }
    rows.push_back(withIcon(setupRow(QStringLiteral("jfsetup:check"),tr("Check Server"),Action::JellyfinSetupCheck,
        tr("Asks the address for its Jellyfin name and version.")),glyphs::search()));
    {
        QString detail=tr("Asks this network which Jellyfin servers are on it.");
        if(st.searching)detail=tr("Looking... Cross stops.");
        else if(st.searched)detail=st.found.isEmpty()?tr("No server answered. Type its address instead."):
            (st.found.size()==1?tr("1 server answered."):tr("%1 servers answered.").arg(int(st.found.size())));
        rows.push_back(withIcon(setupRow(QStringLiteral("jfsetup:find"),st.searching?tr("Looking for Servers"):tr("Look for Servers on This Network"),
            Action::JellyfinSetupFind,detail),glyphs::search()));
    }
    for(int i=0;i<st.found.size();++i) {
        const auto& s=st.found[i];
        rows.push_back(withIcon(setupRow(QStringLiteral("jfsetup:found:")+QString::number(i),s.name,Action::JellyfinSetupPick,
            s.url.toString(QUrl::FullyEncoded),QString::number(i)),serviceIcons::jellyfin()));
    }
    return rows;
}

void Shell::refreshJellyfinSetupPage() {
    for(auto& page:pages_) {
        if(!page.jfSetup)continue;
        const QString focused=page.entries.value(page.selection).id;
        page.entries=jellyfinSetupRows();
        page.selection=std::clamp(page.selection,0,std::max(0,int(page.entries.size())-1));
        for(int i=0;i<page.entries.size();++i)if(page.entries[i].id==focused){page.selection=i;break;}
    }
}

void Shell::focusJellyfinSetupRow(const QString& id) {
    for(auto& page:pages_) {
        if(!page.jfSetup)continue;
        for(int i=0;i<page.entries.size();++i)if(page.entries[i].id==id){page.selection=i;break;}
    }
}

void Shell::openJellyfinSetupPage(const QString& editId) {
    if(jellyfinSetupOpen() || pages_.size()>=32)return;
    jfSetup_={};
    jfSetup_.editId=editId;
    if(!editId.isEmpty()) {
        const auto app=std::find_if(contentApps_.cbegin(),contentApps_.cend(),[&](const ContentApp& a){return a.id==editId && a.kind==QLatin1String("jellyfin");});
        if(app==contentApps_.cend()){showMessage(tr("Jellyfin"),tr("This Jellyfin registration could not be found."),MessageKind::Page);return;}
        jfSetup_.address=app->url;
    }
    if(!jfProbe_) {
        jfProbe_=new jellyfinSetup::ServerProbe(dlnaNetwork_,this);
        connect(jfProbe_,&jellyfinSetup::ServerProbe::finished,this,[this](quint64 request,bool ok,QString name,QString version,QString message,bool unreachable){
            if(!jellyfinSetupOpen() || request!=jfSetup_.request)return;
            using Check=jellyfinSetup::SetupState::Check;
            jfSetup_.check=ok?Check::Ok:Check::Failed;
            jfSetup_.serverName=name;jfSetup_.serverVersion=version;jfSetup_.error=message;jfSetup_.unreachable=unreachable;
            loading_=false;status_.clear();
            refreshJellyfinSetupPage();
            // A row shows its second line only while focused: a good answer leaves the focus on the Add row (the next
            // press saves it), a bad one on the result row, so its reason is on the screen.
            focusJellyfinSetupRow(ok?QStringLiteral("jfsetup:save"):QStringLiteral("jfsetup:result"));
            emit changed();
        });
    }
    if(!jfDiscovery_) {
        jfDiscovery_=new jellyfinSetup::Discovery(this);
        connect(jfDiscovery_,&jellyfinSetup::Discovery::found,this,[this](QString name,QString id,QUrl url){
            if(!jellyfinSetupOpen())return;
            jfSetup_.found.push_back({name,id,url});
            refreshJellyfinSetupPage();emit changed();
        });
        connect(jfDiscovery_,&jellyfinSetup::Discovery::finished,this,[this](int){
            if(!jellyfinSetupOpen())return;
            jfSetup_.searching=false;jfSetup_.searched=true;
            refreshJellyfinSetupPage();emit changed();
        });
    }
    Page page;
    page.title=editId.isEmpty()?tr("Set Up Jellyfin"):tr("Change Jellyfin Address");
    page.jfSetup=true;
    page.entries=jellyfinSetupRows();
    pages_.push_back(std::move(page));
    qInfo().noquote()<<"jellyfin setup: page opened for"<<(editId.isEmpty()?"adding a server":"changing the address");
    emit changed();
}

void Shell::closeJellyfinSetupPage() {
    if(jfProbe_)jfProbe_->cancel();
    if(jfDiscovery_)jfDiscovery_->cancel();
    for(int i=pages_.size()-1;i>=0;--i)if(pages_[i].jfSetup){pages_.removeAt(i);break;}
    jfSetup_={};
    loading_=false;status_.clear();
}

void Shell::setJellyfinSetupAddress(const QString& typed) {
    if(!jellyfinSetupOpen())return;
    jfSetup_.address=typed.trimmed();
    jfSetup_.check=jellyfinSetup::SetupState::Check::Idle;jfSetup_.checkedAddress.clear();
    if(jfProbe_)jfProbe_->cancel();
    if(jfSetup_.address.isEmpty()){refreshJellyfinSetupPage();emit changed();return;}
    checkJellyfinServer();
}

void Shell::checkJellyfinServer() {
    if(!jellyfinSetupOpen())return;
    using Check=jellyfinSetup::SetupState::Check;
    auto& st=jfSetup_;
    const auto parsed=jellyfinSetup::parseAddress(st.address);
    st.error.clear();st.unreachable=false;st.serverName.clear();st.serverVersion.clear();
    if(!parsed.ok) {
        // Nothing sensitive is logged: what was typed is not echoed, only that it was refused.
        qInfo().noquote()<<"jellyfin setup: address refused:"<<parsed.error;
        st.check=Check::Failed;st.error=parsed.error;st.checkedAddress.clear();
        st.address=jellyfinSetup::withoutCredentials(st.address);   // a secret typed by mistake is not shown back
        refreshJellyfinSetupPage();focusJellyfinSetupRow(QStringLiteral("jfsetup:result"));emit changed();return;
    }
    st.address=parsed.text;   // show exactly what would be saved
    st.checkedAddress=parsed.text;
    if(diagnostics::safeMode()) {
        st.check=Check::Failed;st.error=tr("Safe mode does not use the network.");
        refreshJellyfinSetupPage();focusJellyfinSetupRow(QStringLiteral("jfsetup:result"));emit changed();return;
    }
    st.check=Check::Checking;
    st.request=jfProbe_->check(parsed.url);
    refreshJellyfinSetupPage();emit changed();
}

void Shell::saveJellyfinSetup() {
    if(!jellyfinSetupOpen())return;
    using Check=jellyfinSetup::SetupState::Check;
    const auto& st=jfSetup_;
    const auto parsed=jellyfinSetup::parseAddress(st.address);
    const bool current=parsed.ok && st.checkedAddress==parsed.text;
    if(!(current && (st.check==Check::Ok || (st.check==Check::Failed && st.unreachable)))) {
        interactionAudio_.play(InteractionAudio::Cue::Error);
        showMessage(tr("Jellyfin"),tr("Check the server address first. It can be saved once it answers as Jellyfin."),MessageKind::Page);
        emit changed();return;
    }
    const QString editId=st.editId;
    QString previous;
    for(const auto& app:contentApps_)if(app.id==editId)previous=app.url;
    QString error,savedId;
    if(!jellyfinSetup::saveRegistration(appRegistryPath(),parsed.text,editId,error,&savedId)) {
        qWarning().noquote()<<"jellyfin setup: save failed:"<<error;
        showMessage(tr("Jellyfin"),error,MessageKind::Page);emit changed();return;
    }
    qInfo().noquote()<<"jellyfin setup: saved"<<savedId<<"to apps.json"<<(editId.isEmpty()?"(new)":"(address changed)");
    const QString name=st.serverName;
    if(!editId.isEmpty() && previous!=parsed.text) {
        // The old session belongs to the old server: drop it, and the cached icon that server served.
        jellyfinRequest_=0;jellyfin_.signOut();jellyfin_.forgetSession();
        QFile::remove(QDir(dataDirectory_).filePath(QStringLiteral("jellyfin/server-icon.png")));
    }
    closeJellyfinSetupPage();
    notify(tr("Jellyfin"),editId.isEmpty()?(name.isEmpty()?tr("Jellyfin added"):tr("%1 added").arg(name)):tr("Server address saved"));
    refreshContent();   // F5: the Jellyfin row replaces the setup row in place, no restart
    emit changed();
}

void Shell::searchJellyfinServers() {
    if(!jellyfinSetupOpen() || !jfDiscovery_)return;
    if(jfSetup_.searching) {   // Cross again stops
        jfDiscovery_->cancel();
        jfSetup_.searching=false;jfSetup_.searched=true;
        refreshJellyfinSetupPage();emit changed();return;
    }
    if(diagnostics::safeMode()) {
        showMessage(tr("Jellyfin"),tr("Safe mode does not use the network."),MessageKind::Page);emit changed();return;
    }
    jfSetup_.found.clear();jfSetup_.searched=false;jfSetup_.searching=true;
    jfDiscovery_->start();
    refreshJellyfinSetupPage();emit changed();
}

void Shell::activateJellyfinSetup(const Entry& entry) {
    switch(entry.action) {
    case Action::JellyfinSetup:openJellyfinSetupPage();break;
    case Action::JellyfinSetupField:
        if(jellyfinSetupOpen())emit requestJellyfinFieldEdit(QStringLiteral("jfsetup:address"),tr("Jellyfin Server Address"),jfSetup_.address,false);
        break;
    case Action::JellyfinSetupCheck:checkJellyfinServer();break;
    case Action::JellyfinSetupSave:saveJellyfinSetup();break;
    case Action::JellyfinSetupFind:searchJellyfinServers();break;
    case Action::JellyfinSetupPick: {
        if(!jellyfinSetupOpen())break;
        const int index=entry.value.toInt();
        if(index<0 || index>=jfSetup_.found.size())break;
        if(jfDiscovery_)jfDiscovery_->cancel();
        jfSetup_.searching=false;jfSetup_.searched=true;
        jfSetup_.address=jfSetup_.found[index].url.toString(QUrl::FullyEncoded);
        checkJellyfinServer();
        break;
    }
    default:break;
    }
}

}
