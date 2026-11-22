#pragma once
// A persistent Windows.Graphics.Capture session on the title's window (audit/INGAME-BG-CAPTURE.md).
// Standalone like game_capture.h: Qt Core/Gui plus Win32/WinRT, no dependency on shell or view. Nothing is injected into,
// hooked in, or suspended in the target process; it is the same DWM capture captureGameWindow uses, kept open.
//
// While running it keeps the newest GOOD frame resident on the GPU. Each arriving frame costs two GPU copies and one
// mip generation and no CPU readback: the frame is copied into a ring slot, a ~60x33 box-filtered probe of it is read
// back a frame later (ingame_backdrop.h decides blank vs good from it), and a good slot is promoted to "last good" by a
// pointer swap. The only CPU copy of a full frame is grabLastGood(), once, when the in-game XMB opens.
//
// GPU memory (B8G8R8A8, 4 bytes/px): a 3-slot ring plus the last-good texture, each with a mip chain down to the probe
// level (x1.33), plus the 2-buffer WGC frame pool: 7.3 frames of w*h*4 bytes. 1080p: 8.3 MB/frame, ~61 MB. 4K: 33.2 MB/frame,
// ~243 MB. grabLastGood() adds one transient w*h*4 staging texture and the returned QImage (8.3 / 33.2 MB).
#include <QImage>
#include <QString>
#include <QtGlobal>
#include <memory>

namespace xmb {

struct GameTapStats {
    bool running = false;
    quint64 frames = 0;        // frames delivered by the capture session
    quint64 goodFrames = 0;    // classified, not blank
    quint64 blankFrames = 0;   // classified blank (a game that blanks or cloaks on focus loss)
    qint64 lastGoodAgeMs = -1; // since the newest good frame ARRIVED; -1 = never
    qint64 lastFrameAgeMs = -1;
    bool lastClassifiedBlank = false;
    int width = 0, height = 0; // of the ring (the window's content size)
    double avgCallbackUs = 0;  // CPU time per delivered frame on the capture thread
    double maxCallbackUs = 0;
    quint64 gpuBytes = 0;      // resident GPU memory the tap holds
};

class GameFrameTap {
public:
    GameFrameTap();
    ~GameFrameTap();
    GameFrameTap(const GameFrameTap&) = delete;
    GameFrameTap& operator=(const GameFrameTap&) = delete;

    // Starts capturing `hwnd`. Any previous session is stopped first. False with *error set when WGC is unsupported, the
    // window cannot be captured (excluded, elevated, gone) or the device cannot be created.
    bool start(quintptr hwnd, QString* error);
    void stop();
    bool running() const;
    quintptr hwnd() const;

    // Counters and ages, after resolving any probe still in flight.
    GameTapStats stats();
    // The newest good frame as an opaque RGB32 image (the one CPU copy). False when no good frame was ever seen.
    // *detail describes the frame ("1920x1080 age 41 ms").
    bool grabLastGood(QImage* image, QString* detail);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace xmb
