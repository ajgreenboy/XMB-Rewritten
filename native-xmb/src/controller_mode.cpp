#include "controller_mode.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QtGlobal>
#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#include <softpub.h>
#include <wincrypt.h>
#include <wintrust.h>
#include <shellapi.h>
#endif
#ifdef XMB_HAVE_VIGEM
// The vendored header is Latin-1 in its author comments (C4828 under /utf-8); it is upstream's file and stays untouched.
#pragma warning(push)
#pragma warning(disable:4828)
#include <ViGEm/Client.h>
#pragma warning(pop)
#endif

namespace xmb::controller {
namespace {
QString envText(const char* name){return qEnvironmentVariable(name).trimmed();}

// The backend a build without the ViGEm client has: truthful, never pretends to create a pad.
class UnavailablePad final : public VirtualPad {
public:
    QString backendName() const override {return QStringLiteral("unavailable");}
    bool create(QString* why) override {if(why)*why=QStringLiteral("This build has no virtual gamepad backend (ViGEm client not built).");return false;}
    bool update(const Xbox360Report&,QString* why) override {if(why)*why=QStringLiteral("no virtual pad");return false;}
    void destroy() override {}
    bool exists() const override {return false;}
};

#ifdef XMB_HAVE_VIGEM
// The real adapter: ViGEmClient (third_party/ViGEmClient). Every call is checked; nothing throws.
class ViGEmPad final : public VirtualPad {
public:
    ~ViGEmPad() override {destroy();}
    QString backendName() const override {return QStringLiteral("vigem");}
    bool create(QString* why) override {
        if(target_)return true;
        client_=vigem_alloc();
        if(!client_){if(why)*why=QStringLiteral("Out of memory allocating the ViGEm client.");return false;}
        const VIGEM_ERROR connected=vigem_connect(client_);
        if(!VIGEM_SUCCESS(connected)){const QString reason=describe(connected);release();if(why)*why=reason;return false;}
        target_=vigem_target_x360_alloc();
        if(!target_){release();if(why)*why=QStringLiteral("Out of memory allocating the virtual pad.");return false;}
        const VIGEM_ERROR added=vigem_target_add(client_,target_);
        if(!VIGEM_SUCCESS(added)){const QString reason=describe(added);vigem_target_free(target_);target_=nullptr;release();if(why)*why=reason;return false;}
        added_=true;
        // Rumble (phase 2) would register vigem_target_x360_register_notification here; phase 1 registers nothing.
        return true;
    }
    bool update(const Xbox360Report& r,QString* why) override {
        if(!target_||!added_){if(why)*why=QStringLiteral("no virtual pad");return false;}
        XUSB_REPORT report;
        report.wButtons=r.buttons;report.bLeftTrigger=r.leftTrigger;report.bRightTrigger=r.rightTrigger;
        report.sThumbLX=r.thumbLX;report.sThumbLY=r.thumbLY;report.sThumbRX=r.thumbRX;report.sThumbRY=r.thumbRY;
        const VIGEM_ERROR result=vigem_target_x360_update(client_,target_,report);
        if(!VIGEM_SUCCESS(result)){if(why)*why=describe(result);return false;}
        return true;
    }
    void destroy() override {
        if(target_){
            if(added_&&client_)vigem_target_remove(client_,target_);
            vigem_target_free(target_);target_=nullptr;added_=false;
        }
        release();
    }
    bool exists() const override {return target_&&added_;}
private:
    void release(){if(client_){vigem_disconnect(client_);vigem_free(client_);client_=nullptr;}}
    static QString describe(VIGEM_ERROR e){
        switch(e){
        case VIGEM_ERROR_BUS_NOT_FOUND:return QStringLiteral("The ViGEmBus driver is not installed or not running.");
        case VIGEM_ERROR_NO_FREE_SLOT:return QStringLiteral("No free virtual pad slot.");
        case VIGEM_ERROR_BUS_ACCESS_FAILED:return QStringLiteral("Access to the ViGEmBus driver was denied.");
        case VIGEM_ERROR_BUS_VERSION_MISMATCH:return QStringLiteral("The ViGEmBus driver version does not match the client.");
        case VIGEM_ERROR_TARGET_UNINITIALIZED:
        case VIGEM_ERROR_TARGET_NOT_PLUGGED_IN:return QStringLiteral("The virtual pad is not plugged in.");
        case VIGEM_ERROR_ALREADY_CONNECTED:return QStringLiteral("Already connected to the ViGEmBus driver.");
        case VIGEM_ERROR_INVALID_TARGET:return QStringLiteral("The virtual pad target is invalid.");
        case VIGEM_ERROR_REMOVAL_FAILED:return QStringLiteral("The virtual pad could not be removed.");
        default:return QStringLiteral("ViGEm error 0x%1.").arg(quint32(e),0,16);
        }
    }
    PVIGEM_CLIENT client_=nullptr;
    PVIGEM_TARGET target_=nullptr;
    bool added_=false;
};
#endif
}

bool killSwitchEngaged(){return envText("XMB_CONTROLLER_MODE")==QLatin1String("0");}

std::unique_ptr<VirtualPad> makeBackend(const QString& which) {
    const QString name=(which.isEmpty()?envText("XMB_CONTROLLER_BACKEND"):which).toLower();
    if(name==QLatin1String("fake")) {
        auto fake=std::make_unique<FakePad>(envText("XMB_CONTROLLER_FAKE_LOG"));
        fake->createFailure=envText("XMB_CONTROLLER_FAKE_FAIL");   // scripted runs: make create() fail with this reason
        return fake;
    }
#ifdef XMB_HAVE_VIGEM
    return std::make_unique<ViGEmPad>();
#else
    return std::make_unique<UnavailablePad>();
#endif
}

DriverStatus queryDriver() {
    DriverStatus out;
    const QString forced=envText("XMB_CONTROLLER_DRIVER").toLower();   // test seam: present | missing
    if(forced==QLatin1String("present")){out.state=DriverState::Running;out.detail=QStringLiteral("forced by XMB_CONTROLLER_DRIVER");return out;}
    if(forced==QLatin1String("missing")){out.state=DriverState::Missing;out.detail=QStringLiteral("forced by XMB_CONTROLLER_DRIVER");return out;}
#ifdef Q_OS_WIN
    // Read-only service query: no client library, no elevation.
    SC_HANDLE manager=OpenSCManagerW(nullptr,nullptr,SC_MANAGER_CONNECT);
    if(!manager){out.state=DriverState::Unknown;out.detail=QStringLiteral("The service manager could not be read.");return out;}
    SC_HANDLE service=OpenServiceW(manager,L"ViGEmBus",SERVICE_QUERY_STATUS);
    if(!service){
        const DWORD error=GetLastError();
        out.state=error==ERROR_SERVICE_DOES_NOT_EXIST?DriverState::Missing:DriverState::Unknown;
        out.detail=error==ERROR_SERVICE_DOES_NOT_EXIST?QStringLiteral("The ViGEmBus driver is not installed."):QStringLiteral("The ViGEmBus service could not be read (error %1).").arg(error);
    } else {
        SERVICE_STATUS status{};
        if(QueryServiceStatus(service,&status)){
            out.state=status.dwCurrentState==SERVICE_RUNNING?DriverState::Running:DriverState::Stopped;
            out.detail=out.state==DriverState::Running?QStringLiteral("ViGEmBus is running."):QStringLiteral("The ViGEmBus driver is installed but not running.");
        } else {out.state=DriverState::Unknown;out.detail=QStringLiteral("The ViGEmBus service status could not be read.");}
        CloseServiceHandle(service);
    }
    CloseServiceHandle(manager);
#else
    out.state=DriverState::Missing;out.detail=QStringLiteral("Controller mode is Windows only.");
#endif
    return out;
}

SteamInputStatus steamInputStatus(const QString&,const QString&) {
    // GAP, with evidence (audit/CONTROLLER-MODE.md): "Steam Input for PlayStation controllers" is the client setting
    // controller_ps_support (an int32 in the Steam client's settings table, field 14003 in steamui) and the per-title
    // mode is a protobuf app-config field. Neither key appears in config\config.vdf, userdata\<id>\config\localconfig.vdf
    // or cloudstorage\*.json on a machine that has a DualSense, so there is no file key to read. Never guessed.
    SteamInputStatus s;
    s.state=SteamInput::Unknown;
    s.detail=QStringLiteral("Steam Input status unknown");
    return s;
}

QString rowDetail(const DriverStatus& driver,const SteamInputStatus& steamInput) {
    QString driverText;
    switch(driver.state){
    case DriverState::Running:driverText=QStringLiteral("Driver: ready");break;
    case DriverState::Stopped:driverText=QStringLiteral("Driver: installed but not running");break;
    case DriverState::Missing:driverText=QStringLiteral("Driver: not installed");break;
    case DriverState::Unknown:driverText=QStringLiteral("Driver: unknown");break;
    }
    QString text=driverText+QStringLiteral(". ")+steamInput.detail+QStringLiteral(".");
    if(steamInput.state==SteamInput::On)text+=QStringLiteral(" Steam Input is on for this game; turn it off to avoid double input.");
    return text;
}

const DriverInstaller& pinnedInstaller() {
    static const DriverInstaller pin{QStringLiteral("ViGEmBus_1.22.0_x64_x86_arm64.exe"),QStringLiteral("1.22.0"),
        QStringLiteral("89220a7865076b342892f98865f3499fb7c4cfd673159e89d352c360fd014c6a"),
        QStringLiteral("Nefarius Software Solutions e.U."),6278576};
    return pin;
}
QString bundledInstallerPath(const QString& appDirectory) {
    const QString path=QDir(appDirectory).filePath(QStringLiteral("drivers/")+pinnedInstaller().fileName);
    return QFileInfo(path).isFile()?QDir::toNativeSeparators(path):QString();
}

#ifdef Q_OS_WIN
namespace {
bool signedBy(const QString& path,const QString& signer,QString* why) {
    const std::wstring wide=QDir::toNativeSeparators(path).toStdWString();
    WINTRUST_FILE_INFO file{};file.cbStruct=sizeof(file);file.pcwszFilePath=wide.c_str();
    WINTRUST_DATA data{};data.cbStruct=sizeof(data);data.dwUIChoice=WTD_UI_NONE;data.fdwRevocationChecks=WTD_REVOKE_NONE;
    data.dwUnionChoice=WTD_CHOICE_FILE;data.pFile=&file;data.dwStateAction=WTD_STATEACTION_VERIFY;data.dwProvFlags=WTD_REVOCATION_CHECK_NONE;
    GUID action=WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const LONG verdict=WinVerifyTrust(nullptr,&action,&data);
    const bool ok=verdict==ERROR_SUCCESS;
    QString name;
    if(ok){
        CRYPT_PROVIDER_DATA* provider=WTHelperProvDataFromStateData(data.hWVTStateData);
        CRYPT_PROVIDER_SGNR* signerInfo=provider?WTHelperGetProvSignerFromChain(provider,0,FALSE,0):nullptr;
        CRYPT_PROVIDER_CERT* cert=signerInfo?WTHelperGetProvCertFromChain(signerInfo,0):nullptr;
        if(cert&&cert->pCert){
            wchar_t buffer[256]{};
            CertGetNameStringW(cert->pCert,CERT_NAME_SIMPLE_DISPLAY_TYPE,0,nullptr,buffer,255);
            name=QString::fromWCharArray(buffer);
        }
    }
    data.dwStateAction=WTD_STATEACTION_CLOSE;WinVerifyTrust(nullptr,&action,&data);
    if(!ok){if(why)*why=QStringLiteral("The installer's Authenticode signature is not valid (0x%1).").arg(quint32(verdict),0,16);return false;}
    if(name!=signer){if(why)*why=QStringLiteral("The installer is signed by \"%1\", not \"%2\".").arg(name,signer);return false;}
    return true;
}
}
#endif

bool verifyInstaller(const QString& path,QString* why) {
    const DriverInstaller& pin=pinnedInstaller();
    QFile file(path);
    if(!file.open(QIODevice::ReadOnly)){if(why)*why=QStringLiteral("The installer could not be read.");return false;}
    if(file.size()!=pin.size){if(why)*why=QStringLiteral("The installer's size does not match the pinned size.");return false;}
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if(!hash.addData(&file)){if(why)*why=QStringLiteral("The installer could not be read.");return false;}
    if(QString::fromLatin1(hash.result().toHex()).compare(pin.sha256,Qt::CaseInsensitive)!=0){
        if(why)*why=QStringLiteral("The installer's SHA-256 does not match the pinned value.");return false;
    }
    file.close();
#ifdef Q_OS_WIN
    return signedBy(path,pin.signer,why);
#else
    if(why)*why=QStringLiteral("Controller mode is Windows only.");
    return false;
#endif
}

bool installDriver(const QString& appDirectory,QString* why) {
    const QString path=bundledInstallerPath(appDirectory);
    if(path.isEmpty()){if(why)*why=QStringLiteral("This build has no bundled controller driver installer (drivers\\%1 is missing).").arg(pinnedInstaller().fileName);return false;}
    if(!verifyInstaller(path,why))return false;
#ifdef Q_OS_WIN
    // Interactive: the vendor documents no silent switch (audit/CONTROLLER-MODE.md), so the installer's own window shows.
    // Only the installer is elevated; this process stays as it is.
    const std::wstring file=path.toStdWString();
    SHELLEXECUTEINFOW info{};info.cbSize=sizeof(info);info.fMask=SEE_MASK_NOASYNC;info.lpVerb=L"runas";info.lpFile=file.c_str();info.nShow=SW_SHOWNORMAL;
    if(!ShellExecuteExW(&info)){
        const DWORD error=GetLastError();
        if(why)*why=error==ERROR_CANCELLED?QStringLiteral("The administrator prompt was declined."):QStringLiteral("The installer could not be started (error %1).").arg(error);
        return false;
    }
    return true;
#else
    if(why)*why=QStringLiteral("Controller mode is Windows only.");
    return false;
#endif
}
}
