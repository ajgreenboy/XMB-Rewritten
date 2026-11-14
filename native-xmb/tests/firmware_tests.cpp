#include "firmware_extract.h"
#include "firmware_install.h"
#include "firmware_crypto.h"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QTemporaryDir>
#include <QtEndian>
#include <cstdio>

namespace {
void put32(QByteArray& b,int at,quint32 v){qToBigEndian(v,b.data()+at);}
void put64(QByteArray& b,int at,quint64 v){qToBigEndian(v,b.data()+at);}
QByteArray sequence(int n,int start=0){QByteArray b;for(int i=0;i<n;++i)b+=char(start+i);return b;}
bool write(const QString& path,const QByteArray& bytes){QFile f(path);return f.open(QIODevice::WriteOnly)&&f.write(bytes)==bytes.size();}
QByteArray archive(const QByteArray& name,const QByteArray& contents) {
    QByteArray h(512,0);h.replace(0,name.size(),name);h.replace(100,8,QByteArray("0000644\0",8));
    h.replace(124,12,QByteArray::number(contents.size(),8).rightJustified(11,'0')+'\0');
    h.replace(148,8,QByteArray(8,' '));h[156]='0';h.replace(257,8,QByteArray("ustar\00000",8));
    unsigned sum=0;for(char c:h)sum+=static_cast<unsigned char>(c);
    h.replace(148,8,QByteArray::number(sum,8).rightJustified(6,'0')+QByteArray("\0 ",2));
    return h+contents+QByteArray((512-contents.size()%512)%512,0)+QByteArray(1024,0);
}
QByteArray package(const QByteArray& tar) {
    const QByteArray compressed=qCompress(tar).mid(4);
    QByteArray info(64,0);info.replace(0,16,sequence(16,32));info.replace(32,16,sequence(16,48));
    QByteArray metadata(208,0);put32(metadata,12,3);put32(metadata,16,2);
    for(int i=0;i<3;++i){const int h=32+i*48;put64(metadata,h,304);put32(metadata,h+32,1);put32(metadata,h+44,1);}
    put64(metadata,128,304);put64(metadata,136,quint64(compressed.size()));
    put32(metadata,160,3);put32(metadata,164,0);put32(metadata,168,1);put32(metadata,172,2);
    metadata.replace(176,16,sequence(16,64));metadata.replace(192,16,sequence(16,80));
    xmb::firmware::aesCtr(metadata,info.left(16),info.mid(32,16));
    xmb::firmware::aesCbc(info,sequence(32),sequence(16,16),true);
    QByteArray encrypted=compressed;xmb::firmware::aesCtr(encrypted,sequence(16,64),sequence(16,80));
    QByteArray sce(32,0);sce.replace(0,4,QByteArray("SCE\0",4));put32(sce,4,2);put64(sce,16,304);put64(sce,24,quint64(encrypted.size()));
    return sce+info+metadata+encrypted;
}
QByteArray pup(const QByteArray& update) {
    const QByteArray version("4.93\0",5);QByteArray b(176,0);b.replace(0,8,QByteArray("SCEUF\0\0\0",8));
    put64(b,8,1);put64(b,16,1);put64(b,24,2);put64(b,32,176);put64(b,40,quint64(version.size()+update.size()));
    put64(b,48,0x100);put64(b,56,176);put64(b,64,quint64(version.size()));
    put64(b,80,0x300);put64(b,88,quint64(176+version.size()));put64(b,96,quint64(update.size()));
    put64(b,112,0x100);b.replace(120,20,xmb::firmware::hmacSha1(sequence(64),version));
    put64(b,144,0x300);b.replace(152,20,xmb::firmware::hmacSha1(sequence(64),update));return b+version+update;
}
}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);int failures=0;
    const auto check=[&](bool ok,const char* name){std::printf("%s: %s\n",ok?"PASS":"FAIL",name);if(!ok)++failures;};
    qunsetenv("XMB_FIRMWARE_KEYS");QString missingError;
    check(!xmb::extractPup(QStringLiteral("missing.pup"),QStringLiteral("unused"),&missingError)&&missingError==QStringLiteral("decryption keys not provided (create native-xmb/src/firmware_keys.inc)"),"missing-key fallback");
    QByteArray data=QByteArray::fromHex("6bc1bee22e409f96e93d7e117393172a"),plain=data;
    xmb::firmware::aesCtr(data,QByteArray::fromHex("2b7e151628aed2a6abf7158809cf4f3c"),QByteArray::fromHex("f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff"));
    check(data.toHex()=="874d6191b620e3261bef6864990db6ce","NIST SP800-38A AES-128-CTR");
    QByteArray multi=QByteArray::fromHex("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e5130c81c46a35ce411e5fbc1191a0a52eff69f2445df4f9b17ad2b417be66c3710");
    xmb::firmware::aesCtr(multi,QByteArray::fromHex("2b7e151628aed2a6abf7158809cf4f3c"),QByteArray::fromHex("f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff"));
    check(multi.toHex()=="874d6191b620e3261bef6864990db6ce9806f66b7970fdff8617187bb9fffdff5ae4df3edbd5d35e5b4f09020db03eab1e031dda2fbe03d1792170a0f3009cee","NIST four-block AES-CTR counter progression");
    QByteArray cbc=plain;const QByteArray key=QByteArray::fromHex("603deb1015ca71be2b73aef0857d77811f352c073b6108d72d9810a30914dff4");
    const QByteArray iv=QByteArray::fromHex("000102030405060708090a0b0c0d0e0f");
    xmb::firmware::aesCbc(cbc,key,iv,true);check(cbc.toHex()=="f58c4c04d6e5f1ba779eabfb5f7bfbd6","NIST AES-256-CBC encrypt");
    xmb::firmware::aesCbc(cbc,key,iv);check(cbc==plain,"NIST AES-256-CBC decrypt");
    check(xmb::firmware::hmacSha1(QByteArray(20,0x0b),"Hi There").toHex()=="b617318655057264e28bc0b6fb378c8ef146be00","RFC2202 HMAC-SHA1");
    check(xmb::firmware::hmacSha1(QByteArray(80,char(0xaa)),"Test Using Larger Than Block-Size Key - Hash Key First").toHex()=="aa4ae5e15272d00e95705637ce8a3b55ed402112","RFC2202 long-key HMAC-SHA1");
    QTemporaryDir temp;check(temp.isValid(),"temporary directory");
    QByteArray declarations;
    for(const auto& pair:{qMakePair(QByteArray("PUP_KEY"),sequence(64)),qMakePair(QByteArray("SCEPKG_ERK"),sequence(32)),qMakePair(QByteArray("SCEPKG_RIV"),sequence(16,16))}) {
        declarations+="static const unsigned char "+pair.first+'['+QByteArray::number(pair.second.size())+"] = {";
        for(char c:pair.second)declarations+=QByteArray::number(static_cast<unsigned char>(c))+',';
        declarations+="};\n";
    }
    const QString keyPath=temp.path()+"/test.inc";check(write(keyPath,declarations),"synthetic keys");qputenv("XMB_FIRMWARE_KEYS",keyPath.toUtf8());
    QString error;const QString source=temp.path()+"/PS3UPDAT.PUP",dest=temp.path()+"/flash";
    QByteArray fixture=pup(archive("dev_flash_synthetic",package(archive("dev_flash/vsh/resource/explore_plugin_full.rco","fixture"))));
    check(write(source,fixture)&&xmb::extractPup(source,dest,&error),"PUP HMAC + encrypted/compressed SCE + TAR end-to-end");
    if(!error.isEmpty())std::printf("detail: %s\n",error.toUtf8().constData());
    QFile extracted(dest+"/vsh/resource/explore_plugin_full.rco");check(extracted.open(QIODevice::ReadOnly)&&extracted.readAll()=="fixture","dev_flash prefix stripped and bytes correct");
    fixture[120]=char(fixture[120]^1);write(source,fixture);
    check(!xmb::extractPup(source,temp.path()+"/bad",&error)&&error.contains("HMAC"),"wrong PUP hash rejected");
    const QByteArray normal=archive("dev_flash/vsh/normal","ok");
    fixture=pup(archive("dev_flash_synthetic",package(normal.left(normal.size()-1024)+archive("../evil","escape"))));write(source,fixture);
    check(!xmb::extractPup(source,temp.path()+"/unsafe",&error)&&error.contains("Unsafe TAR path"),"TAR traversal rejected");
    check(!QFile::exists(temp.path()+"/evil")&&!QFile::exists(temp.path()+"/unsafe/vsh/normal"),"TAR rejection happens before any writes");
    for(const QByteArray& bad:{QByteArray("/absolute"),QByteArray("C:/escape"),QByteArray("dir\\..\\evil"),QByteArray("dir/file:stream"),QByteArray("CON")}) {
        write(source,pup(archive("dev_flash_synthetic",package(archive(bad,"escape")))));
        check(!xmb::extractPup(source,temp.path()+"/unsafe",&error)&&error.contains("Unsafe TAR path"),bad.constData());
    }
    QByteArray linked=archive("dev_flash/vsh/link","target");linked[156]='2';linked.replace(148,8,QByteArray(8,' '));
    unsigned checksum=0;for(int i=0;i<512;++i)checksum+=static_cast<unsigned char>(linked[i]);
    linked.replace(148,8,QByteArray::number(checksum,8).rightJustified(6,'0')+QByteArray("\0 ",2));
    write(source,pup(archive("dev_flash_synthetic",package(linked))));
    check(!xmb::extractPup(source,temp.path()+"/unsafe",&error)&&error.contains("links/devices forbidden"),"TAR symlink rejected");
    QByteArray malformed=pup(archive("ordinary","hello"));put64(malformed,88,~quint64(0));write(source,malformed);
    check(!xmb::extractPup(source,temp.path()+"/unsafe",&error)&&error.contains("bounds"),"overflowing PUP offset rejected");
    write(source,pup(archive("ordinary","hello")));
    check(!xmb::extractPup(source,temp.path()+"/empty",&error)&&error.contains("2 entries"),"no-package PUP parses/verifies but cannot claim extraction success");
    // ---- firmware_install: policy around the network/extract steps, all offline with fake hooks ----
    {
        const auto goodPup=pup(archive("dev_flash_synthetic",package(archive("dev_flash/vsh/resource/explore_plugin_full.rco","fixture"))));
        const auto withVersion=[&](QByteArray bytes,const QByteArray& version) { // same length so offsets stay valid
            bytes.replace(176,version.size(),version);return bytes;
        };
        QString versionText,versionError;
        write(source,goodPup);
        check(xmb::pupVersion(source,&versionText,&versionError)&&versionText==QStringLiteral("4.93"),"pupVersion reads entry 0x100");
        write(source,withVersion(goodPup,"4.92"));
        check(xmb::pupVersion(source,&versionText,&versionError)&&versionText==QStringLiteral("4.92"),"pupVersion reads another version");
        write(source,QByteArray("not a pup, but long enough to have a header....................."));
        check(!xmb::pupVersion(source,&versionText,&versionError)&&versionText.isEmpty(),"pupVersion rejects a non-PUP");
        const QString installRoot=temp.path()+"/install";
        int latestCalls=0,downloadCalls=0;QString offered=QStringLiteral("4.93");QByteArray served=goodPup;bool extractReal=true;
        xmb::FirmwareInstallHooks hooks;
        hooks.latest=[&](QString*){++latestCalls;return xmb::FirmwareRelease{offered,QStringLiteral("http://example.invalid/PS3UPDAT.PUP")};};
        hooks.download=[&](const QString&,QString* path,QString*,const std::function<void(qint64,qint64)>& progress) {
            ++downloadCalls;const QString file=installRoot+QStringLiteral("-download-%1.PUP").arg(downloadCalls);
            if(!write(file,served))return false;
            progress(0,100);progress(50,100);progress(100,100);*path=file;return true;
        };
        hooks.extract=[&](const QString& pupPath,const QString& dst,QString* err) {
            if(!extractReal){if(err)*err=QStringLiteral("boom");QDir().mkpath(dst+"/half");return false;}
            return xmb::extractPup(pupPath,dst,err);
        };
        xmb::FirmwareInstallOptions options;options.destDir=installRoot+"/dev_flash";
        QList<int> percents;
        const auto record=[&](const QString&,int percent){if(percent>=0)percents<<percent;};
        auto result=xmb::installFirmware(options,hooks,record);
        check(result.outcome==xmb::FirmwareInstallOutcome::Installed&&xmb::firmwareDevFlashPresent(options.destDir),"install: fresh dev_flash is created");
        check(percents.contains(50)&&percents.last()==100,"install: download percent is reported up to 100");
        check(!QFile::exists(options.destDir+".partial")&&!QFile::exists(installRoot+"-download-1.PUP"),"install: staging folder and downloaded PUP are cleaned up");
        result=xmb::installFirmware(options,hooks);
        check(result.outcome==xmb::FirmwareInstallOutcome::AlreadyInstalled&&latestCalls==1&&downloadCalls==1,"install: idempotent, a complete dev_flash is not downloaded again");
        QFile marker(options.destDir+"/old-marker");if(marker.open(QIODevice::WriteOnly)){marker.write("old");marker.close();}
        options.force=true;result=xmb::installFirmware(options,hooks);
        check(result.outcome==xmb::FirmwareInstallOutcome::Installed&&downloadCalls==2&&!QFile::exists(options.destDir+"/old-marker")&&!QFile::exists(options.destDir+".old"),"install: --force replaces the whole tree and drops the .old copy");
        // A failed run must leave the existing tree exactly as it was and no half-written folder behind.
        if(marker.open(QIODevice::WriteOnly)){marker.write("old");marker.close();}
        extractReal=false;result=xmb::installFirmware(options,hooks);
        check(result.outcome==xmb::FirmwareInstallOutcome::Failed&&result.message.contains("boom")&&QFile::exists(options.destDir+"/old-marker")&&xmb::firmwareDevFlashPresent(options.destDir)
              &&!QFile::exists(options.destDir+".partial")&&!QFile::exists(installRoot+"-download-3.PUP"),"install: failed unpack is transactional (old dev_flash intact, no .partial)");
        extractReal=true;
        const QString fresh=installRoot+"/other/dev_flash";options.destDir=fresh;options.force=false;extractReal=false;
        result=xmb::installFirmware(options,hooks);
        check(result.outcome==xmb::FirmwareInstallOutcome::Failed&&!QFileInfo::exists(fresh)&&!QFileInfo::exists(fresh+".partial"),"install: failed first run leaves no half dev_flash");
        extractReal=true;
        const int downloadsBefore=downloadCalls;offered=QStringLiteral("4.94");
        result=xmb::installFirmware(options,hooks);
        check(result.outcome==xmb::FirmwareInstallOutcome::Failed&&result.message.contains("4.94")&&downloadCalls==downloadsBefore&&!QFileInfo::exists(fresh),"install: a list that offers another version is rejected before downloading");
        offered=QStringLiteral("4.93");served=withVersion(goodPup,"4.92");
        result=xmb::installFirmware(options,hooks);
        check(result.outcome==xmb::FirmwareInstallOutcome::Failed&&result.message.contains("4.92")&&!QFileInfo::exists(fresh)&&!QFile::exists(installRoot+QStringLiteral("-download-%1.PUP").arg(downloadCalls)),"install: a downloaded PUP of another version is rejected and removed");
        served=goodPup;served[120]=char(served[120]^1);
        result=xmb::installFirmware(options,hooks);
        check(result.outcome==xmb::FirmwareInstallOutcome::Failed&&result.message.contains("HMAC")&&!QFileInfo::exists(fresh)&&!QFileInfo::exists(fresh+".partial"),"install: a tampered PUP fails verification and writes nothing");
        options.destDir=QString();
        check(xmb::installFirmware(options,hooks).outcome==xmb::FirmwareInstallOutcome::Failed,"install: empty destination is rejected");
        // The swap itself: staging replaces dest, and a missing staging folder changes nothing.
        QDir().mkpath(temp.path()+"/swap/dest");QDir().mkpath(temp.path()+"/swap/stage");
        write(temp.path()+"/swap/dest/a",QByteArray("old"));write(temp.path()+"/swap/stage/b",QByteArray("new"));
        check(xmb::commitFirmwareDirectory(temp.path()+"/swap/stage",temp.path()+"/swap/dest",&error)&&QFile::exists(temp.path()+"/swap/dest/b")&&!QFile::exists(temp.path()+"/swap/dest/a")
              &&!QFile::exists(temp.path()+"/swap/stage")&&!QFile::exists(temp.path()+"/swap/dest.old"),"commit: staging replaces the old tree");
        check(!xmb::commitFirmwareDirectory(temp.path()+"/swap/missing",temp.path()+"/swap/dest",&error)&&QFile::exists(temp.path()+"/swap/dest/b"),"commit: missing staging leaves dest alone");
    }
    // Optional persistent synthetic fixture for exercising the real CLI, no firmware/real keys.
    if(argc==2){QDir().mkpath(QString::fromLocal8Bit(argv[1]));write(QString::fromLocal8Bit(argv[1])+"/test.inc",declarations);
        write(QString::fromLocal8Bit(argv[1])+"/synthetic.pup",pup(archive("dev_flash_synthetic",package(archive("dev_flash/vsh/resource/explore_plugin_full.rco","fixture")))));}
    return failures?1:0;
}
