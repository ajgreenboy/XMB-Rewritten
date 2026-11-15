// Jellyfin setup: address rules, the public-information answer, discovery replies and the apps.json save
// (audit/JELLYFIN-SETUP.md). Pure logic only: no window, no network, no real server.
//   xmb-jellyfin-setup-tests <scratch dir ending in jellyfin-setup>
#include "jellyfin_setup.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <cstdio>

namespace {
int checks=0,failures=0;
void check(bool condition,const char* label) {
    ++checks;if(!condition){++failures;std::fprintf(stderr,"FAIL: %s\n",label);}
}
QByteArray readAll(const QString& path){QFile f(path);return f.open(QIODevice::ReadOnly)?f.readAll():QByteArray();}
bool writeAll(const QString& path,const QByteArray& bytes){QFile f(path);return f.open(QIODevice::WriteOnly|QIODevice::Truncate) && f.write(bytes)==bytes.size();}
}

int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    using namespace xmb;
    using namespace xmb::jellyfinSetup;
    if(argc<2){std::fprintf(stderr,"usage: xmb-jellyfin-setup-tests <scratch dir>\n");return 2;}
    const QString scratch=QDir(QString::fromLocal8Bit(argv[1])).absolutePath();
    if(!scratch.endsWith(QLatin1String("jellyfin-setup"))){std::fprintf(stderr,"scratch dir name must end in jellyfin-setup\n");return 2;}
    QDir(scratch).removeRecursively();QDir().mkpath(scratch);

    // ---- address rules (the registry's: http(s), a host, no credentials, query or fragment) -------------------
    check(parseAddress("http://192.168.1.20:8096").ok,"address: plain http with a port");
    check(parseAddress("https://jf.example.com").ok,"address: https with a name");
    check(parseAddress("  http://192.168.1.20:8096  ").ok,"address: surrounding blanks are ignored");
    check(parseAddress("192.168.1.20:8096").text==QLatin1String("http://192.168.1.20:8096"),"address: no scheme means http");
    check(parseAddress("jellyfin.lan").text==QLatin1String("http://jellyfin.lan"),"address: a bare name means http");
    check(parseAddress("HTTP://Server.LAN:8096/").text==QLatin1String("http://server.lan:8096"),"address: scheme and host lower-cased, trailing slash removed");
    check(parseAddress("http://host/jellyfin///").text==QLatin1String("http://host/jellyfin"),"address: a subpath is kept, trailing slashes removed");
    check(!parseAddress("").ok && !parseAddress("   ").ok,"address: empty is refused");
    check(!parseAddress("ftp://host").ok,"address: ftp is refused");
    check(!parseAddress("file:///C:/x").ok,"address: file is refused");
    check(!parseAddress("javascript://x").ok,"address: javascript is refused");
    check(!parseAddress("http://user:pass@host").ok,"address: credentials are refused");
    check(!parseAddress("http://user@host").ok,"address: a user name alone is refused");
    check(parseAddress("http://user:pass@host").error.contains("password",Qt::CaseInsensitive),"address: credentials message says why");
    check(withoutCredentials("http://user:secret@host:8096")=="http://host:8096" && withoutCredentials("user:pw@host")=="host"
          && withoutCredentials("http://host/a@b")=="http://host/a@b" && withoutCredentials("http://host")=="http://host"
          && withoutCredentials("http://")=="http://" && withoutCredentials("")==QString(),"address: credentials are stripped for display only");
    check(!parseAddress("http://host/?api_key=abc").ok,"address: a query is refused");
    check(!parseAddress("http://host/#x").ok,"address: a fragment is refused");
    check(!parseAddress("http://").ok,"address: no host is refused");
    check(!parseAddress("http://my server").ok,"address: a space is refused");
    check(!parseAddress(QString("http://")+QString(3000,'a')).ok,"address: too long is refused");
    check(!parseAddress("http://host:99999").ok,"address: a bad port is refused");
    // Whatever parseAddress accepts, the registry accepts too, so a save cannot be refused afterwards.
    for(const char* sample:{"192.168.1.20:8096","https://a.b/c","http://[fe80::1]:8096","HTTP://X.Y"}) {
        const auto parsed=parseAddress(QString::fromLatin1(sample));
        check(!parsed.ok || ContentRegistry::isValidServerUrl(parsed.text,true),"address: an accepted address passes the registry rule");
    }
    check(ContentRegistry::isValidServerUrl("http://h:8096",true) && !ContentRegistry::isValidServerUrl("http://h/?x=1",true)
          && ContentRegistry::isValidServerUrl("http://h/?x=1",false),"registry rule: query refused for jellyfin only");
    check(publicInfoUrl(QUrl("http://h:8096")).toString()==QLatin1String("http://h:8096/System/Info/Public"),"public info url");
    check(publicInfoUrl(QUrl("http://h/jf/")).toString()==QLatin1String("http://h/jf/System/Info/Public"),"public info url keeps a subpath");

    // ---- the public-information answer --------------------------------------------------------------------------
    ServerInfo info;QString why;
    check(parsePublicInfo(R"({"LocalAddress":"http://10.0.0.2:8096","ServerName":"Servatron","Version":"10.10.7","ProductName":"Jellyfin Server","OperatingSystem":"","Id":"abc123","StartupWizardCompleted":true})",info,why)
          && info.name=="Servatron" && info.version=="10.10.7" && info.id=="abc123","info: a real answer");
    check(parsePublicInfo(R"({"ServerName":"Old","Version":"10.6.4","Id":"x"})",info,why),"info: an older server without ProductName");
    check(parsePublicInfo(R"({"Version":"10.9.0","Id":"x"})",info,why) && info.name=="Jellyfin","info: no name falls back to Jellyfin");
    check(!parsePublicInfo(R"({"ServerName":"E","Version":"4.8","ProductName":"Emby Server","Id":"x"})",info,why) && why.contains("Emby"),"info: Emby is named and refused");
    check(!parsePublicInfo("<html>nginx</html>",info,why) && !why.isEmpty(),"info: html is refused");
    check(!parsePublicInfo("",info,why),"info: empty is refused");
    check(!parsePublicInfo("[1,2]",info,why),"info: an array is refused");
    check(!parsePublicInfo(R"({"hello":"world"})",info,why),"info: some other service's json is refused");
    parsePublicInfo(QByteArray(R"({"ServerName":"Bad\u0007\nName","Version":"1","Id":"x"})"),info,why);
    check(!info.name.contains(QChar(7)) && !info.name.contains(QChar('\n')),"info: control characters are removed from the name");
    parsePublicInfo(QByteArray(R"({"ServerName":")")+QByteArray(500,'n')+QByteArray(R"(","Version":"1","Id":"x"})"),info,why);
    check(info.name.size()<=64,"info: the name is bounded");

    // ---- failure sentences --------------------------------------------------------------------------------------
    bool unreachable=false;
    check(describeFailure(QNetworkReply::ConnectionRefusedError,0,"h:1",&unreachable).contains("refused") && unreachable,"failure: refused is unreachable");
    check(describeFailure(QNetworkReply::HostNotFoundError,0,"h",&unreachable).contains("not found") && unreachable,"failure: unknown host");
    check(describeFailure(QNetworkReply::TimeoutError,0,"h",&unreachable).contains("did not answer") && unreachable,"failure: timeout");
    check(describeFailure(QNetworkReply::ContentNotFoundError,404,"h",&unreachable).contains("not Jellyfin") && !unreachable,"failure: 404 answered but is not Jellyfin");
    check(describeFailure(QNetworkReply::InternalServerError,500,"h",&unreachable).contains("500") && !unreachable,"failure: an HTTP error is an answer");
    check(describeFailure(QNetworkReply::NoError,301,"h",&unreachable).contains("redirects") && !unreachable,"failure: a redirect is explained");

    // ---- discovery ----------------------------------------------------------------------------------------------
    check(discoveryRequest()==QByteArray("who is JellyfinServer?"),"discovery: the request is the fixed string");
    check(DiscoveryPort==7359,"discovery: port 7359");
    check(isLanAddress(QHostAddress("192.168.1.5")) && isLanAddress(QHostAddress("10.1.2.3")) && isLanAddress(QHostAddress("172.16.0.1"))
          && isLanAddress(QHostAddress("172.31.255.255")) && isLanAddress(QHostAddress("169.254.1.1")) && isLanAddress(QHostAddress("127.0.0.1")),"lan: private IPv4 ranges");
    check(!isLanAddress(QHostAddress("8.8.8.8")) && !isLanAddress(QHostAddress("172.32.0.1")) && !isLanAddress(QHostAddress("11.0.0.1")) && !isLanAddress(QHostAddress()),"lan: public addresses are not LAN");
    check(isLanAddress(QHostAddress("fe80::1")) && isLanAddress(QHostAddress("fd00::1")) && !isLanAddress(QHostAddress("2001:db8::1")),"lan: IPv6 link-local and ULA only");
    check(isLanAddress(QHostAddress("::ffff:192.168.1.9")) && !isLanAddress(QHostAddress("::ffff:8.8.8.8")),"lan: IPv4-mapped addresses are judged as IPv4");
    FoundServer found;
    const QByteArray reply=R"({"Address":"http://172.17.0.2:8096","Id":"deadbeef","Name":"Servatron","EndpointAddress":null})";
    check(parseDiscoveryReply(reply,QHostAddress("192.168.1.122"),found) && found.name=="Servatron" && found.id=="deadbeef","reply: a real answer");
    check(found.url.toString()=="http://192.168.1.122:8096","reply: the offered address is the sender's, not the host the reply names");
    check(parseDiscoveryReply(R"({"Address":"https://evil.example.com:8920/jf","Id":"i","Name":"N"})",QHostAddress("10.0.0.7"),found)
          && found.url.toString()=="https://10.0.0.7:8920/jf","reply: scheme, port and subpath are kept, the host is the sender's");
    check(!parseDiscoveryReply(reply,QHostAddress("8.8.8.8"),found),"reply: a non-LAN sender is not believed");
    check(!parseDiscoveryReply(reply,QHostAddress("fe80::1"),found),"reply: an IPv6 sender is not one of ours");
    check(parseDiscoveryReply(reply,QHostAddress("::ffff:192.168.1.50"),found) && found.url.host()=="192.168.1.50","reply: an IPv4-mapped LAN sender works");
    check(!parseDiscoveryReply("who is JellyfinServer?",QHostAddress("192.168.1.2"),found),"reply: our own request echoed back is ignored");
    check(!parseDiscoveryReply("not json",QHostAddress("192.168.1.2"),found),"reply: garbage is ignored");
    check(!parseDiscoveryReply(R"({"Address":"http://a:1"})",QHostAddress("192.168.1.2"),found),"reply: no id is ignored");
    check(!parseDiscoveryReply(R"({"Id":"x"})",QHostAddress("192.168.1.2"),found),"reply: no address is ignored");
    check(!parseDiscoveryReply(R"({"Address":"ftp://a:1","Id":"x"})",QHostAddress("192.168.1.2"),found),"reply: a non-http address is ignored");
    check(!parseDiscoveryReply(R"({"Address":"http://u:p@a:1","Id":"x"})",QHostAddress("192.168.1.2"),found),"reply: an address with credentials is ignored");
    check(!parseDiscoveryReply(QByteArray(5000,' ')+reply,QHostAddress("192.168.1.2"),found),"reply: an oversized datagram is ignored");
    check(parseDiscoveryReply(R"({"Address":"http://a:1","Id":"x"})",QHostAddress("192.168.1.2"),found) && found.name=="Jellyfin server","reply: a missing name is filled in");

    // ---- apps.json ----------------------------------------------------------------------------------------------
    const QString fresh=QDir(scratch).filePath("fresh-apps.json");
    QString error,id;
    check(saveRegistration(fresh,"192.168.1.122:8096","",error,&id) && id=="jellyfin","save: creates apps.json on a fresh install");
    QVector<ContentApp> apps;
    check(ContentRegistry::load(fresh,apps,error) && apps.size()==1,"save: the file loads back");
    check(apps.value(0).id=="jellyfin" && apps.value(0).title=="Jellyfin" && apps.value(0).kind=="jellyfin"
          && apps.value(0).category=="tv" && apps.value(0).url=="http://192.168.1.122:8096","save: id, title, kind, category and url as import-apps would");
    check(hasJellyfinApp(apps),"save: hasJellyfinApp sees it");
    // The same registration through --import-apps' writer gives a byte-identical file.
    const QString manifest=QDir(scratch).filePath("manifest.json");
    writeAll(manifest,R"({"version":1,"apps":[{"id":"jellyfin","title":"Jellyfin","kind":"jellyfin","category":"tv","url":"http://192.168.1.122:8096"}]})");
    const QString imported=QDir(scratch).filePath("imported-apps.json");
    check(ContentRegistry::importFile(manifest,imported,error),"save: import-apps accepts the manifest");
    check(readAll(fresh)==readAll(imported),"save: byte-identical to what --import-apps writes");
    check(!saveRegistration(fresh,"192.168.1.5:8096","",error) && error.contains("already"),"save: a second add is refused");
    check(saveRegistration(fresh,"https://jf.example.com/jf/","jellyfin",error,&id) && id=="jellyfin","save: changing the address");
    ContentRegistry::load(fresh,apps,error);
    check(apps.size()==1 && apps[0].url=="https://jf.example.com/jf" && apps[0].title=="Jellyfin" && apps[0].category=="tv","save: only the url changed");
    check(!saveRegistration(fresh,"http://u:p@h","jellyfin",error) && !error.isEmpty(),"save: credentials are refused");
    ContentRegistry::load(fresh,apps,error);
    check(apps[0].url=="https://jf.example.com/jf","save: a refused save leaves the file alone");
    check(!saveRegistration(fresh,"http://h","nope",error),"save: an unknown id to change is refused");

    // Other registrations survive, and keep their fields.
    const QString mixed=QDir(scratch).filePath("mixed-apps.json");
    writeAll(mixed,R"({"version":1,"apps":[{"id":"jellyfin","title":"Old Web","kind":"web","category":"tv","url":"https://old.example.com"},{"id":"spot","title":"Spotify","kind":"web","category":"music","url":"https://open.spotify.com","window":"kiosk"}]})");
    check(saveRegistration(mixed,"http://10.0.0.9:8096","",error,&id) && id=="jellyfin-server","save: an id owned by a web app is not overwritten");
    ContentRegistry::load(mixed,apps,error);
    check(apps.size()==3 && apps[0].title=="Old Web" && apps[1].window=="kiosk" && apps[2].id=="jellyfin-server" && apps[2].kind=="jellyfin","save: other apps untouched, new one appended");
    check(registrationId(apps)=="jellyfin-server-2","registrationId: next free id");

    // An unreadable apps.json is reported and left exactly as it was.
    const QString broken=QDir(scratch).filePath("broken-apps.json");
    writeAll(broken,"{ not json");
    check(!saveRegistration(broken,"http://h:8096","",error) && !error.isEmpty() && readAll(broken)=="{ not json","save: a broken apps.json is reported, not replaced");
    // A data folder that does not exist yet is created.
    const QString nested=QDir(scratch).filePath("a/b/apps.json");
    check(saveRegistration(nested,"http://h:8096","",error) && QFile::exists(nested),"save: creates the data folder when it is missing");

    std::printf("jellyfin-setup tests: %d checks, %d failures\n",checks,failures);
    return failures?1:0;
}
