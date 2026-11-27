#include "raf_render.h"
#include "edge_decode.h"
#include "gtf_decode.h"
#include <QDebug>
#include <QMatrix4x4>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QSurfaceFormat>
#include <QTime>
#include <QtMath>
#include <cstddef>

namespace xmb {
namespace {
QVector3D vec(const P3t::Raf::Vec3& v) { return {float(v.x),float(v.y),float(v.z)}; }
QVector4D vec4(const P3t::Raf::Vec3& v) { return {float(v.x),float(v.y),float(v.z),0.f}; }
QVector4D vec4(const P3t::Raf::Vec4& v) { return {float(v.x),float(v.y),float(v.z),float(v.w)}; }
QImage renderOffscreen(const QByteArray& raf,double seconds,bool runScript,int width,int height,QString* error,RafStillStats* stats,const QDateTime& when) {
    if(error)error->clear();
    if(stats)*stats={};
    const auto fail=[&](const QString& message)->QImage { if(error)*error=message;return {}; };
    if(width<=0 || height<=0)return fail(QStringLiteral("invalid image size"));
    QSurfaceFormat format;format.setVersion(3,3);format.setProfile(QSurfaceFormat::CoreProfile);format.setDepthBufferSize(24);
    QOpenGLContext context;context.setFormat(format);
    if(!context.create())return fail(QStringLiteral("cannot create OpenGL context"));
    QOffscreenSurface surface;surface.setFormat(context.format());surface.create();
    if(!surface.isValid() || !context.makeCurrent(&surface))return fail(QStringLiteral("cannot make offscreen context current"));
    QOpenGLFunctions_3_3_Core gl;
    if(!gl.initializeOpenGLFunctions())return fail(QStringLiteral("OpenGL 3.3 core unavailable"));
    QOpenGLFramebufferObjectFormat fboFormat;fboFormat.setAttachment(QOpenGLFramebufferObject::CombinedDepthStencil);
    QOpenGLFramebufferObject fbo(width,height,fboFormat);
    if(!fbo.isValid() || !fbo.bind())return fail(QStringLiteral("cannot create framebuffer"));
    RafScenePlayer player;
    QString why;
    if(!player.init(gl,raf,when,runScript,&why))return fail(why);
    player.render(gl,width,height,seconds,runScript);
    QImage result=fbo.toImage();
    if(stats)*stats=player.stats();
    player.release(gl);
    return result;
}
}

bool rafThemesEnabled() { return qEnvironmentVariable("XMB_RAF_THEMES")!=QStringLiteral("0"); }
bool rafAnimationEnabled() { return rafThemesEnabled() && qEnvironmentVariable("XMB_RAF_ANIMATE")!=QStringLiteral("0"); }

bool RafScenePlayer::init(QOpenGLFunctions_3_3_Core& gl,const QByteArray& raf,const QDateTime& when,bool runScript,QString* error) {
    release(gl);
    if(error)error->clear();
    const auto fail=[&](const QString& message)->bool { release(gl);if(error)*error=message;return false; };
    QString why;const P3t theme=P3t::read(raf,&why);
    if(theme.isNull() || !theme.hasScene())return fail(QStringLiteral("invalid RAF: ")+why);
    scene_=theme.scene();
    const char* vertex=R"(#version 330 core
layout(location=0) in vec3 position; layout(location=1) in vec3 normal; layout(location=2) in vec2 uv;
uniform mat4 mvp,model; uniform mat3 normalMatrix; uniform vec4 uvTransform;
out vec3 worldPos,worldNormal; out vec2 texCoord;
void main(){ vec4 p=model*vec4(position,1); worldPos=p.xyz; worldNormal=normalMatrix*normal;
 texCoord=uv*uvTransform.zw+uvTransform.xy; gl_Position=mvp*vec4(position,1); })";
    const char* fragment=R"(
in vec3 worldPos,worldNormal; in vec2 texCoord; out vec4 outColor;
uniform sampler2D diffuseMap; uniform vec4 actorColor; uniform vec3 lightPos,lightColor,attenuation,ambient,eyePos;
void main(){ vec4 tex=texture(diffuseMap,texCoord);
#ifdef UNLIT
 outColor=tex*actorColor;
#else
 vec3 delta=lightPos-worldPos; float d=length(delta);
 vec3 n=normalize(worldNormal), l=normalize(delta), v=normalize(eyePos-worldPos);
 float lambert=max(dot(n,l),0.0);
 float falloff=1.0/max(dot(attenuation,vec3(1,d,d*d)),0.001);
 vec3 lighting=ambient+lightColor*lambert*falloff;
 float specular=lambert>0.0 ? pow(max(dot(n,normalize(l+v)),0.0),32.0)*0.2 : 0.0;
 vec3 color=tex.rgb*actorColor.rgb*lighting+lightColor*specular*falloff;
#ifdef EDGE_LIT
 color+=tex.rgb*actorColor.rgb*pow(1.0-max(dot(n,v),0.0),3.0)*0.35;
#endif
 outColor=vec4(color,tex.a*actorColor.a);
#endif
 })";
    const auto compile=[&](QOpenGLShaderProgram& program,const QByteArray& define) {
        return program.addShaderFromSourceCode(QOpenGLShader::Vertex,vertex) &&
               program.addShaderFromSourceCode(QOpenGLShader::Fragment,QByteArray("#version 330 core\n")+define+fragment) &&
               program.link();
    };
    if(!compile(litProgram_,{}) || !compile(textureProgram_,"#define UNLIT\n") || !compile(edgeProgram_,"#define EDGE_LIT\n"))
        return fail(QStringLiteral("shader: ")+litProgram_.log()+textureProgram_.log()+edgeProgram_.log());
    int triangles=0;
    for(const auto& model:scene_.models) {
        QVector<EdgeVertex> vertices;QVector<quint32> indices;
        if(!decodeEdge(model.geometryData,vertices,indices,&why))return fail(QStringLiteral("model %1: %2").arg(model.id,why));
        Mesh m;m.count=int(indices.size());triangles+=m.count/3;
        gl.glGenVertexArrays(1,&m.vao);gl.glGenBuffers(1,&m.vbo);gl.glGenBuffers(1,&m.ibo);gl.glBindVertexArray(m.vao);
        gl.glBindBuffer(GL_ARRAY_BUFFER,m.vbo);gl.glBufferData(GL_ARRAY_BUFFER,vertices.size()*sizeof(EdgeVertex),vertices.constData(),GL_STATIC_DRAW);
        gl.glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,m.ibo);gl.glBufferData(GL_ELEMENT_ARRAY_BUFFER,indices.size()*sizeof(quint32),indices.constData(),GL_STATIC_DRAW);
        gl.glEnableVertexAttribArray(0);gl.glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,sizeof(EdgeVertex),reinterpret_cast<void*>(offsetof(EdgeVertex,position)));
        gl.glEnableVertexAttribArray(1);gl.glVertexAttribPointer(1,3,GL_FLOAT,GL_FALSE,sizeof(EdgeVertex),reinterpret_cast<void*>(offsetof(EdgeVertex,normal)));
        gl.glEnableVertexAttribArray(2);gl.glVertexAttribPointer(2,2,GL_FLOAT,GL_FALSE,sizeof(EdgeVertex),reinterpret_cast<void*>(offsetof(EdgeVertex,uv)));
        meshes_.insert(model.id,m);
    }
    gl.glBindVertexArray(0);
    for(const auto& texture:scene_.textures) {
        QImage image=decodeGtf(texture.data,&why).convertToFormat(QImage::Format_RGBA8888);
        if(image.isNull())return fail(QStringLiteral("texture %1: %2").arg(texture.id,why));
        GLuint id=0;gl.glGenTextures(1,&id);gl.glBindTexture(GL_TEXTURE_2D,id);
        gl.glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,image.width(),image.height(),0,GL_RGBA,GL_UNSIGNED_BYTE,image.constBits());
        gl.glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);gl.glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
        gl.glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_REPEAT);gl.glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_REPEAT);
        textures_.insert(texture.id,id);
    }
    gl.glBindTexture(GL_TEXTURE_2D,0);
    for(const auto& material:scene_.materials)materials_.insert(material.id,material);
    for(const auto& actor:scene_.actors) {
        if(!meshes_.contains(actor.model) || !materials_.contains(actor.material) || !textures_.contains(materials_.value(actor.material).colorMap))
            return fail(QStringLiteral("actor %1 has unresolved model or diffuse texture").arg(actor.id));
    }
    const auto& c=scene_.camera;
    const QVector3D eye=vec(c.position),direction=vec(c.direction),up=vec(c.up);
    const bool cameraFallback=!c.present || c.yfov<=0 || c.yfov>=3.14159 || c.znear<=0 || c.zfar<=c.znear ||
        direction.lengthSquared()<1e-8f || up.lengthSquared()<1e-8f || QVector3D::crossProduct(direction,up).lengthSquared()<1e-8f;
    bool lightFallback=true;
    for(const auto& light:scene_.lights)if(light.type==0 && vec(light.color).lengthSquared()>0){lightFallback=false;break;}
    stats_.actors=int(scene_.actors.size());stats_.models=int(meshes_.size());stats_.textures=int(textures_.size());
    stats_.triangles=triangles;stats_.cameraFallback=cameraFallback;stats_.lightFallback=lightFallback;
    if(runScript && scene_.script.present && scene_.script.vsmx) {
        VsmxEvalOptions opts;
        const QDateTime clock=when.isValid()?when:QDateTime::currentDateTime();
        const QTime t=clock.time();
        opts.hour=t.hour();opts.minute=t.minute();opts.second=t.second();opts.millisecond=t.msec();
        for(const auto& actor:scene_.actors) {
            auto& pose=opts.initialPose[actor.id];
            pose[VsmxKey::Position]=vec4(actor.position);
            pose[VsmxKey::Rotation]=vec4(actor.rotation);
            pose[VsmxKey::Scale]=vec4(actor.scale);
            pose[VsmxKey::Colour]=vec4(actor.color);
            pose[VsmxKey::Alpha]=QVector4D(0,0,0,float(actor.color.w));
            pose[VsmxKey::UvOffset]=vec4(actor.uvOffset);
            pose[VsmxKey::UvScale]=vec4(actor.uvScale);
        }
        for(const auto& light:scene_.lights) {
            auto& pose=opts.initialPose[light.id];
            pose[VsmxKey::Position]=vec4(light.position);
            pose[VsmxKey::Colour]=QVector4D(float(light.color.x),float(light.color.y),float(light.color.z),1.f);
            pose[VsmxKey::Attenuation]=vec4(light.attenuation);
        }
        if(c.present && !c.id.isEmpty())opts.initialPose[c.id][VsmxKey::Position]=vec4(c.position);
        QString scriptErr;
        if(evalVsmx(scene_.script.data,&timeline_,&scriptErr,opts))scriptEvaluated_=true;
        else qInfo().noquote()<<"raf: VSMX script evaluation failed, keeping initial pose:"<<scriptErr;
    }
    ready_=true;
    return true;
}

void RafScenePlayer::render(QOpenGLFunctions_3_3_Core& gl,int width,int height,double seconds,bool sampleScript) {
    if(!ready_ || width<=0 || height<=0)return;
    const bool useScript=sampleScript && scriptEvaluated_;
    const auto& c=scene_.camera;
    QVector3D eye=vec(c.position),direction=vec(c.direction),up=vec(c.up);
    if(useScript && c.present && !c.id.isEmpty() && hasKeyKind(timeline_,c.id,VsmxKey::Position))
        eye=sample(timeline_,c.id,VsmxKey::Position,seconds,vec4(c.position)).toVector3D();
    const bool cameraFallback=stats_.cameraFallback;
    if(cameraFallback){eye={0,0,4};direction={0,0,-1};up={0,1,0};}
    QMatrix4x4 projection,view;
    projection.perspective(cameraFallback?45.f:qRadiansToDegrees(float(c.yfov)),float(width)/height,
                           cameraFallback?.01f:float(c.znear),cameraFallback?1000.f:float(c.zfar));
    view.lookAt(eye,eye+direction,up);
    QVector3D lightPos(0,2,4),lightColor(1,1,1),attenuation(1,0,0),ambient(.2f,.2f,.2f);
    for(const auto& light:scene_.lights) {
        if(light.type==0 && vec(light.color).lengthSquared()>0) {
            lightPos=vec(light.position);lightColor=vec(light.color);attenuation=vec(light.attenuation);
            if(useScript && !light.id.isEmpty()) {
                if(hasKeyKind(timeline_,light.id,VsmxKey::Position))
                    lightPos=sample(timeline_,light.id,VsmxKey::Position,seconds,vec4(light.position)).toVector3D();
                if(hasKeyKind(timeline_,light.id,VsmxKey::Colour))
                    lightColor=sample(timeline_,light.id,VsmxKey::Colour,seconds,QVector4D(lightColor,1.f)).toVector3D();
                if(hasKeyKind(timeline_,light.id,VsmxKey::Attenuation))
                    attenuation=sample(timeline_,light.id,VsmxKey::Attenuation,seconds,vec4(light.attenuation)).toVector3D();
            }
            if(attenuation.lengthSquared()<1e-8f)attenuation={1,0,0};
            break;
        }
    }
    gl.glViewport(0,0,width,height);gl.glClearColor(0,0,0,1);gl.glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
    gl.glDisable(GL_CULL_FACE);gl.glDisable(GL_DEPTH_TEST);gl.glEnable(GL_BLEND);gl.glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
    for(const auto& actor:scene_.actors) {
        const auto material=materials_.value(actor.material);
        const RafEffectSpec fx=resolveRafEffect(
            material.effect,
            !material.normalMap.isEmpty() && material.tangentNormalMap.isEmpty(),
            !material.tangentNormalMap.isEmpty(),
            !material.reflectionMap.isEmpty());
        QOpenGLShaderProgram* selected=&litProgram_;
        if(fx.unlit)selected=&textureProgram_;
        else if(fx.edgeLit)selected=&edgeProgram_;
        if(!fx.known && !unknownEffects_.contains(material.effect.toLower())){
            qInfo()<<"Unknown RAF effect, using basic lighting:"<<material.effect;
            unknownEffects_.insert(material.effect.toLower());
        }
        QVector3D pos=vec(actor.position),rot=vec(actor.rotation),scl=vec(actor.scale),uvOff=vec(actor.uvOffset),uvScl=vec(actor.uvScale);
        QVector4D col=vec4(actor.color);
        if(useScript) {
            if(hasKeyKind(timeline_,actor.id,VsmxKey::Position))pos=sample(timeline_,actor.id,VsmxKey::Position,seconds,vec4(actor.position)).toVector3D();
            if(hasKeyKind(timeline_,actor.id,VsmxKey::Rotation))rot=sample(timeline_,actor.id,VsmxKey::Rotation,seconds,vec4(actor.rotation)).toVector3D();
            if(hasKeyKind(timeline_,actor.id,VsmxKey::Scale))scl=sample(timeline_,actor.id,VsmxKey::Scale,seconds,vec4(actor.scale)).toVector3D();
            if(hasKeyKind(timeline_,actor.id,VsmxKey::Colour))col=sample(timeline_,actor.id,VsmxKey::Colour,seconds,col);
            if(hasKeyKind(timeline_,actor.id,VsmxKey::Alpha))col.setW(sample(timeline_,actor.id,VsmxKey::Alpha,seconds,QVector4D(0,0,0,col.w())).w());
            if(hasKeyKind(timeline_,actor.id,VsmxKey::UvOffset))uvOff=sample(timeline_,actor.id,VsmxKey::UvOffset,seconds,vec4(actor.uvOffset)).toVector3D();
            if(hasKeyKind(timeline_,actor.id,VsmxKey::UvScale))uvScl=sample(timeline_,actor.id,VsmxKey::UvScale,seconds,vec4(actor.uvScale)).toVector3D();
        }
        if(col.w()<=0.0001f || (std::abs(scl.x())<1e-6f && std::abs(scl.y())<1e-6f && std::abs(scl.z())<1e-6f))continue;
        auto& program=*selected;program.bind();
        program.setUniformValue("diffuseMap",0);program.setUniformValue("lightPos",lightPos);program.setUniformValue("lightColor",lightColor);
        program.setUniformValue("attenuation",attenuation);program.setUniformValue("ambient",ambient);program.setUniformValue("eyePos",eye);
        QMatrix4x4 model;model.translate(pos);
        model.rotate(qRadiansToDegrees(rot.z()),0,0,1);model.rotate(qRadiansToDegrees(rot.y()),0,1,0);model.rotate(qRadiansToDegrees(rot.x()),1,0,0);
        model.scale(scl);
        program.setUniformValue("model",model);program.setUniformValue("mvp",projection*view*model);program.setUniformValue("normalMatrix",model.normalMatrix());
        program.setUniformValue("uvTransform",QVector4D(uvOff.x(),uvOff.y(),uvScl.x(),uvScl.y()));
        program.setUniformValue("actorColor",col);
        GLboolean prevDepthMask=GL_TRUE;
        gl.glGetBooleanv(GL_DEPTH_WRITEMASK,&prevDepthMask);
        if(fx.blendEnable){
            gl.glEnable(GL_BLEND);
            gl.glBlendFunc(GLenum(fx.blendSrc),GLenum(fx.blendDst));
        } else if(fx.hasExplicitDepthToken) {
            // Keep historical blend behaviour for pure_texture_alpha_0_depth_1 while disabling depth writes.
            gl.glEnable(GL_BLEND);
            gl.glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
        } else {
            gl.glDisable(GL_BLEND);
        }
        if(!fx.depthMask || (!fx.blendEnable && fx.hasExplicitDepthToken))gl.glDepthMask(GL_FALSE);
        gl.glActiveTexture(GL_TEXTURE0);gl.glBindTexture(GL_TEXTURE_2D,textures_.value(material.colorMap));
        const Mesh m=meshes_.value(actor.model);gl.glBindVertexArray(m.vao);gl.glDrawElements(GL_TRIANGLES,m.count,GL_UNSIGNED_INT,nullptr);
        gl.glDepthMask(prevDepthMask);
        gl.glEnable(GL_BLEND);gl.glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
        program.release();
    }
    gl.glBindVertexArray(0);gl.glBindTexture(GL_TEXTURE_2D,0);gl.glDisable(GL_BLEND);
}

void RafScenePlayer::release(QOpenGLFunctions_3_3_Core& gl) {
    for(const auto& m:meshes_){
        if(m.vbo)gl.glDeleteBuffers(1,&m.vbo);
        if(m.ibo)gl.glDeleteBuffers(1,&m.ibo);
        if(m.vao)gl.glDeleteVertexArrays(1,&m.vao);
    }
    for(GLuint id:textures_)if(id)gl.glDeleteTextures(1,&id);
    meshes_.clear();textures_.clear();materials_.clear();unknownEffects_.clear();
    litProgram_.removeAllShaders();textureProgram_.removeAllShaders();edgeProgram_.removeAllShaders();
    scene_={};timeline_={};stats_={};ready_=false;scriptEvaluated_=false;
}

QImage renderRafStill(const QByteArray& raf,const QString& devFlash,int width,int height,QString* error,RafStillStats* stats) {
    Q_UNUSED(devFlash); // The generic shader does not depend on firmware shader archives.
    return renderOffscreen(raf,0.,false,width,height,error,stats,{});
}

QImage renderRafFrame(const QByteArray& raf,double seconds,int width,int height,QString* error,RafStillStats* stats,const QDateTime& when) {
    return renderOffscreen(raf,seconds,true,width,height,error,stats,when);
}
}
