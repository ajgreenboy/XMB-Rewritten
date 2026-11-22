// See game_tap.h. WinRT headers must precede Qt's (macro hygiene).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <unknwn.h>
#include <dxgi.h>
#include <d3d11_4.h>
#include <inspectable.h>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include "game_tap.h"
#include "ingame_backdrop.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>

namespace xmb {
namespace {

namespace wgc = winrt::Windows::Graphics::Capture;
namespace wgd = winrt::Windows::Graphics::DirectX;
namespace wgd11 = winrt::Windows::Graphics::DirectX::Direct3D11;

struct __declspec(uuid("A9B3D012-3DF2-4EE3-B8D1-8695F457D3C1")) IDxgiAccessLocal : ::IUnknown {
    virtual HRESULT __stdcall GetInterface(REFIID id, void** object) = 0;
};

QString hrText(HRESULT hr) { return QStringLiteral("0x%1").arg(quint32(hr), 8, 16, QLatin1Char('0')); }

qint64 nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
double microsSince(const LARGE_INTEGER& t0, const LARGE_INTEGER& freq) {
    LARGE_INTEGER t1;
    QueryPerformanceCounter(&t1);
    return double(t1.QuadPart - t0.QuadPart) * 1e6 / double(freq.QuadPart);
}

// One full-size copy of a frame with the mips needed for the probe, and the tiny staging texture the probe is read from.
struct Slot {
    winrt::com_ptr<ID3D11Texture2D> tex;
    winrt::com_ptr<ID3D11ShaderResourceView> srv;
    winrt::com_ptr<ID3D11Texture2D> probe;   // staging, probe-level size
    bool pending = false;                    // a probe read-back is in flight
    quint64 seq = 0;
    qint64 arrivedMs = 0;
};

struct State {
    std::mutex m;
    bool closed = false;
    quintptr hwnd = 0;

    winrt::com_ptr<ID3D11Device> dev;
    winrt::com_ptr<ID3D11DeviceContext> ctx;
    wgd11::IDirect3DDevice winrtDevice{nullptr};
    wgc::GraphicsCaptureItem item{nullptr};
    wgc::Direct3D11CaptureFramePool pool{nullptr};
    wgc::GraphicsCaptureSession session{nullptr};
    winrt::event_token token{};
    winrt::Windows::Graphics::SizeInt32 poolSize{};

    // Ring and last-good; the ring's dimensions, the probe level and its size sit beside them.
    std::array<Slot, 3> ring;
    Slot good;
    int ringW = 0, ringH = 0, level = 0, probeW = 0, probeH = 0;
    quint64 seq = 0, goodSeq = 0, classifiedSeq = 0;
    qint64 goodArrivedMs = -1, lastFrameMs = -1;
    bool lastBlank = false;
    quint64 frames = 0, goodFrames = 0, blankFrames = 0;
    double cbSumUs = 0, cbMaxUs = 0;
    quint64 cbCount = 0;
    LARGE_INTEGER freq{};

    void releaseRing() {
        for (auto& s : ring) s = Slot{};
        good = Slot{};
        ringW = ringH = 0;
        goodSeq = classifiedSeq = 0;
        goodArrivedMs = -1;
    }

    bool makeSlot(Slot& s, bool withProbe) {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = UINT(ringW); d.Height = UINT(ringH); d.MipLevels = UINT(level + 1); d.ArraySize = 1;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM; d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET; d.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
        s = Slot{};
        if (FAILED(dev->CreateTexture2D(&d, nullptr, s.tex.put()))) return false;
        if (FAILED(dev->CreateShaderResourceView(s.tex.get(), nullptr, s.srv.put()))) return false;
        if (withProbe) {
            D3D11_TEXTURE2D_DESC p{};
            p.Width = UINT(probeW); p.Height = UINT(probeH); p.MipLevels = 1; p.ArraySize = 1;
            p.Format = DXGI_FORMAT_B8G8R8A8_UNORM; p.SampleDesc.Count = 1; p.Usage = D3D11_USAGE_STAGING; p.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            if (FAILED(dev->CreateTexture2D(&p, nullptr, s.probe.put()))) return false;
        }
        return true;
    }
    bool buildRing(int w, int h) {
        releaseRing();
        ringW = w; ringH = h;
        level = ingamebg::probeLevel(w, h);
        probeW = ingamebg::levelSize(w, level); probeH = ingamebg::levelSize(h, level);
        for (auto& s : ring) if (!makeSlot(s, true)) { releaseRing(); return false; }
        if (!makeSlot(good, false)) { releaseRing(); return false; }
        return true;
    }

    // Classify every probe whose GPU copy has finished, oldest first. Never waits.
    void resolve() {
        std::array<Slot*, 3> order{&ring[0], &ring[1], &ring[2]};
        std::sort(order.begin(), order.end(), [](const Slot* a, const Slot* b) { return a->seq < b->seq; });
        for (Slot* s : order) {
            if (!s->pending) continue;
            D3D11_MAPPED_SUBRESOURCE map{};
            const HRESULT hr = ctx->Map(s->probe.get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &map);
            if (hr == DXGI_ERROR_WAS_STILL_DRAWING) continue;
            s->pending = false;
            if (FAILED(hr)) continue;
            const bool blank = ingamebg::levelIsBlank(static_cast<const uint8_t*>(map.pData), probeW, probeH, int(map.RowPitch));
            ctx->Unmap(s->probe.get(), 0);
            if (s->seq > classifiedSeq) { classifiedSeq = s->seq; lastBlank = blank; }
            if (blank) { ++blankFrames; continue; }
            ++goodFrames;
            if (s->seq > goodSeq) {
                std::swap(s->tex, good.tex);
                std::swap(s->srv, good.srv);
                goodSeq = s->seq;
                goodArrivedMs = s->arrivedMs;
            }
        }
    }

    void onFrame() {
        LARGE_INTEGER t0;
        QueryPerformanceCounter(&t0);
        std::lock_guard<std::mutex> lock(m);
        if (closed) return;
        auto frame = pool.TryGetNextFrame();
        if (!frame) return;
        ++frames;
        lastFrameMs = nowMs();
        resolve();
        const auto content = frame.ContentSize();
        if (content.Width <= 0 || content.Height <= 0) { frame.Close(); return; }
        if (content.Width != poolSize.Width || content.Height != poolSize.Height) {
            // The window was resized: the pool follows it, this frame (the old size) is dropped.
            poolSize = content;
            try { pool.Recreate(winrtDevice, wgd::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, content); } catch (...) {}
            frame.Close();
            return;
        }
        winrt::com_ptr<ID3D11Texture2D> src;
        try {
            auto access = frame.Surface().as<IDxgiAccessLocal>();
            if (FAILED(access->GetInterface(__uuidof(ID3D11Texture2D), src.put_void()))) { frame.Close(); return; }
        } catch (...) { frame.Close(); return; }
        D3D11_TEXTURE2D_DESC sd{};
        src->GetDesc(&sd);
        if (int(sd.Width) != ringW || int(sd.Height) != ringH) {
            if (!buildRing(int(sd.Width), int(sd.Height))) { frame.Close(); return; }
        }
        Slot* slot = nullptr;
        for (auto& s : ring) if (!s.pending) { slot = &s; break; }
        if (slot) {
            ctx->CopySubresourceRegion(slot->tex.get(), 0, 0, 0, 0, src.get(), 0, nullptr);
            ctx->GenerateMips(slot->srv.get());
            ctx->CopySubresourceRegion(slot->probe.get(), 0, 0, 0, 0, slot->tex.get(), UINT(level), nullptr);
            ctx->Flush();
            slot->pending = true;
            slot->seq = ++seq;
            slot->arrivedMs = lastFrameMs;
        }
        frame.Close();
        const double us = microsSince(t0, freq);
        cbSumUs += us; ++cbCount;
        cbMaxUs = std::max(cbMaxUs, us);
    }
};

}  // namespace

struct GameFrameTap::Impl {
    std::shared_ptr<State> state;
};

GameFrameTap::GameFrameTap() : impl_(std::make_unique<Impl>()) {}
GameFrameTap::~GameFrameTap() { stop(); }

bool GameFrameTap::running() const { return impl_->state && !impl_->state->closed; }
quintptr GameFrameTap::hwnd() const { return impl_->state ? impl_->state->hwnd : 0; }

void GameFrameTap::stop() {
    auto st = std::move(impl_->state);
    impl_->state.reset();
    if (!st) return;
    wgc::Direct3D11CaptureFramePool pool{nullptr};
    wgc::GraphicsCaptureSession session{nullptr};
    winrt::event_token token{};
    {
        std::lock_guard<std::mutex> lock(st->m);
        st->closed = true;
        pool = st->pool; session = st->session; token = st->token;
    }
    // Outside the lock: a callback in flight needs it to finish, and Close() may wait for that callback.
    try { if (pool) pool.FrameArrived(token); } catch (...) {}
    try { if (session) session.Close(); } catch (...) {}
    try { if (pool) pool.Close(); } catch (...) {}
    std::lock_guard<std::mutex> lock(st->m);
    st->releaseRing();
    st->pool = nullptr; st->session = nullptr; st->item = nullptr; st->winrtDevice = nullptr;
    st->ctx = nullptr; st->dev = nullptr;
}

bool GameFrameTap::start(quintptr handle, QString* error) {
    stop();
    auto fail = [&](const QString& why) { if (error) *error = why; return false; };
    HWND hwnd = reinterpret_cast<HWND>(handle);
    if (!hwnd || !IsWindow(hwnd)) return fail(QStringLiteral("not a window"));
    auto st = std::make_shared<State>();
    st->hwnd = handle;
    QueryPerformanceFrequency(&st->freq);
    try {
        try { winrt::init_apartment(winrt::apartment_type::multi_threaded); } catch (...) {}   // the caller's apartment is already set
        if (!wgc::GraphicsCaptureSession::IsSupported()) return fail(QStringLiteral("WGC not supported"));
        auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        HRESULT hr = interop->CreateForWindow(hwnd, winrt::guid_of<wgc::GraphicsCaptureItem>(), winrt::put_abi(st->item));
        if (FAILED(hr) || !st->item) return fail(QStringLiteral("CreateForWindow ") + hrText(hr));
        const auto size = st->item.Size();
        if (size.Width <= 0 || size.Height <= 0) return fail(QStringLiteral("item size empty"));
        st->poolSize = size;

        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                               D3D11_SDK_VERSION, st->dev.put(), nullptr, st->ctx.put());
        if (FAILED(hr)) return fail(QStringLiteral("D3D11CreateDevice ") + hrText(hr));
        if (auto mt = st->ctx.try_as<ID3D11Multithread>()) mt->SetMultithreadProtected(TRUE);
        auto dxgi = st->dev.as<IDXGIDevice>();
        winrt::com_ptr<::IInspectable> insp;
        hr = CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), insp.put());
        if (FAILED(hr)) return fail(QStringLiteral("CreateDirect3D11DeviceFromDXGIDevice ") + hrText(hr));
        st->winrtDevice = insp.as<wgd11::IDirect3DDevice>();

        st->pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(
            st->winrtDevice, wgd::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);
        st->session = st->pool.CreateCaptureSession(st->item);
        using winrt::Windows::Foundation::Metadata::ApiInformation;
        if (ApiInformation::IsPropertyPresent(L"Windows.Graphics.Capture.GraphicsCaptureSession", L"IsCursorCaptureEnabled")) {
            try { st->session.IsCursorCaptureEnabled(false); } catch (...) {}
        }
        if (ApiInformation::IsPropertyPresent(L"Windows.Graphics.Capture.GraphicsCaptureSession", L"IsBorderRequired")) {
            try { st->session.IsBorderRequired(false); } catch (...) {}
        }
        // 20 frames a second is plenty for a backdrop and for "frames keep arriving"; it keeps the capture cheap.
        if (ApiInformation::IsPropertyPresent(L"Windows.Graphics.Capture.GraphicsCaptureSession", L"MinUpdateInterval")) {
            try { st->session.MinUpdateInterval(std::chrono::milliseconds(50)); } catch (...) {}
        }
        std::shared_ptr<State> keep = st;
        st->token = st->pool.FrameArrived([keep](auto&&, auto&&) { keep->onFrame(); });
        st->session.StartCapture();
    } catch (const winrt::hresult_error& e) {
        return fail(QStringLiteral("winrt error ") + hrText(e.code()) + QLatin1Char(' ') + QString::fromWCharArray(e.message().c_str()));
    } catch (...) {
        return fail(QStringLiteral("unknown exception"));
    }
    impl_->state = st;
    return true;
}

GameTapStats GameFrameTap::stats() {
    GameTapStats out;
    auto st = impl_->state;
    if (!st) return out;
    std::lock_guard<std::mutex> lock(st->m);
    if (st->closed) return out;
    st->resolve();
    const qint64 now = nowMs();
    out.running = true;
    out.frames = st->frames; out.goodFrames = st->goodFrames; out.blankFrames = st->blankFrames;
    out.lastGoodAgeMs = st->goodArrivedMs < 0 ? -1 : now - st->goodArrivedMs;
    out.lastFrameAgeMs = st->lastFrameMs < 0 ? -1 : now - st->lastFrameMs;
    out.lastClassifiedBlank = st->lastBlank;
    out.width = st->ringW; out.height = st->ringH;
    out.avgCallbackUs = st->cbCount ? st->cbSumUs / double(st->cbCount) : 0;
    out.maxCallbackUs = st->cbMaxUs;
    const quint64 frameBytes = quint64(st->ringW) * quint64(st->ringH) * 4;
    out.gpuBytes = st->ringW ? quint64(double(frameBytes) * (4 * 4.0 / 3.0 + 2)) : 0;
    return out;
}

bool GameFrameTap::grabLastGood(QImage* image, QString* detail) {
    auto st = impl_->state;
    if (!st || !image) return false;
    std::lock_guard<std::mutex> lock(st->m);
    if (st->closed) return false;
    st->resolve();
    if (!st->good.tex || st->goodSeq == 0) { if (detail) *detail = QStringLiteral("no good frame yet"); return false; }
    D3D11_TEXTURE2D_DESC d{};
    st->good.tex->GetDesc(&d);
    d.MipLevels = 1; d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.MiscFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    winrt::com_ptr<ID3D11Texture2D> staging;
    if (FAILED(st->dev->CreateTexture2D(&d, nullptr, staging.put()))) { if (detail) *detail = QStringLiteral("staging texture failed"); return false; }
    st->ctx->CopySubresourceRegion(staging.get(), 0, 0, 0, 0, st->good.tex.get(), 0, nullptr);
    D3D11_MAPPED_SUBRESOURCE map{};
    if (FAILED(st->ctx->Map(staging.get(), 0, D3D11_MAP_READ, 0, &map))) { if (detail) *detail = QStringLiteral("Map failed"); return false; }
    const int w = int(d.Width), h = int(d.Height);
    QImage img(w, h, QImage::Format_RGB32);
    for (int y = 0; y < h; ++y) {
        const uchar* src = static_cast<const uchar*>(map.pData) + size_t(y) * map.RowPitch;
        uchar* dst = img.scanLine(y);
        memcpy(dst, src, size_t(w) * 4);
        for (int x = 0; x < w; ++x) dst[x * 4 + 3] = 0xFF;
    }
    st->ctx->Unmap(staging.get(), 0);
    *image = img;
    if (detail) *detail = QStringLiteral("%1x%2 age %3 ms").arg(w).arg(h).arg(nowMs() - st->goodArrivedMs);
    return true;
}

}  // namespace xmb
