#pragma once
#include "p3t.h"
#include "vsmx_eval.h"
#include <QByteArray>
#include <QDateTime>
#include <QHash>
#include <QImage>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLShaderProgram>
#include <QSet>
#include <QString>

namespace xmb {
struct RafStillStats {
    int actors=0,models=0,textures=0,triangles=0;
    bool cameraFallback=false,lightFallback=false;
};
// Live GL 3.3 scene player for a dynamic theme's embedded RAF scene.
// Uploads Edge meshes and GTF level-0 textures once on an active OpenGL 3.3 Core context, evaluates the VSMX
// script into a keyframe timeline (when present), and draws the scene per frame at `seconds`.
class RafScenePlayer {
public:
    struct Mesh { GLuint vao=0,vbo=0,ibo=0; int count=0; };
    bool init(QOpenGLFunctions_3_3_Core& gl,const QByteArray& raf,const QDateTime& when={},bool runScript=true,QString* error=nullptr);
    void render(QOpenGLFunctions_3_3_Core& gl,int width,int height,double seconds,bool sampleScript=true);
    void release(QOpenGLFunctions_3_3_Core& gl);
    bool ready() const { return ready_; }
    bool hasScript() const { return scriptEvaluated_; }
    bool animated() const { return scriptEvaluated_ && timeline_.hasMotion(); }
    const RafStillStats& stats() const { return stats_; }
    const VsmxTimeline& timeline() const { return timeline_; }
private:
    P3t::Raf scene_;
    VsmxTimeline timeline_;
    bool ready_=false,scriptEvaluated_=false;
    RafStillStats stats_;
    QHash<QString,Mesh> meshes_;
    QHash<QString,GLuint> textures_;
    QHash<QString,P3t::Raf::Material> materials_;
    QOpenGLShaderProgram litProgram_,textureProgram_,edgeProgram_;
    QSet<QString> unknownEffects_;
};
// A dynamic theme's scene drawn as ONE STILL FRAME: the actors at the initial pose the RAF tree declares, the VSMX
// script not run (deterministic for --raf-render and the raf-static-scene gate), approximate GLSL lighting in place of
// the firmware's raf.qrc effect programs. Null with an error on any failure; callers then keep the theme's still bgimage.
QImage renderRafStill(const QByteArray& raf,const QString& devFlash,int width,int height,QString* error=nullptr,RafStillStats* stats=nullptr);
// Offscreen frame at `seconds` with VSMX script evaluation enabled (for --raf-render file.raf@seconds@out.png and diagnostics).
QImage renderRafFrame(const QByteArray& raf,double seconds,int width,int height,QString* error=nullptr,RafStillStats* stats=nullptr,const QDateTime& when={});
// Kill switches: XMB_RAF_THEMES=0 skips the scene altogether (still bgimage); XMB_RAF_ANIMATE=0 keeps the t=0 still frame.
bool rafThemesEnabled();
bool rafAnimationEnabled();
}
