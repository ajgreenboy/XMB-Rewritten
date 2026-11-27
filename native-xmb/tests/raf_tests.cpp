#include "edge_decode.h"
#include "gtf_decode.h"
#include "preferences.h"
#include "raf_effect.h"
#include "vsmx_eval.h"
#include <QCoreApplication>
#include <QImage>
#include <QTemporaryDir>
#include <QtEndian>
#include <bit>
#include <cmath>
#include <cstdio>
#include <numbers>

using namespace xmb;

// The pure decoders behind dynamic-theme (RAF) scenes (audit/RAF-THEMES.md): GTF textures, Edge geometry and the VSMX
// script disassembler/evaluator, plus the appearance/tvColumn preference contract. Every fixture is SYNTHETIC, built
// byte by byte below from the layouts the decoders document; no firmware or theme bytes. Build with
// -DXMB_RAF_BUILD_TESTS=ON; exit code 0 = all pass.
static int failures=0;
static void check(bool ok,const char* what) {
    std::printf("%s  %s\n",ok?"PASS":"FAIL",what);
    if(!ok)++failures;
}
static void be16(QByteArray& b,int at,quint16 v){qToBigEndian(v,b.data()+at);}
static void be32(QByteArray& b,int at,quint32 v){qToBigEndian(v,b.data()+at);}
static void le32(QByteArray& b,int at,quint32 v){qToLittleEndian(v,b.data()+at);}

// GTF: header with one texture; CellGcmTexture format at 0x18, width/height at 0x20/0x22, pitch at 0x28, pixels at 0x80.
static QByteArray gtf(int format,int width,int height,const QByteArray& pixels,quint32 pitch=0) {
    QByteArray b(0x80,'\0');
    be32(b,0,0x01020200);be32(b,8,1);be32(b,0x10,0x80);be32(b,0x14,quint32(pixels.size()));
    b[0x18]=char(format);b[0x19]=1;b[0x1a]=2;be16(b,0x20,quint16(width));be16(b,0x22,quint16(height));be32(b,0x28,pitch);
    return b+pixels;
}
static void gtfTests() {
    QString error;
    // B8 linear (0xA1): one byte is grey and alpha.
    QImage image=decodeGtf(gtf(0xa1,2,2,QByteArray::fromHex("00408000")),&error);
    check(!image.isNull() && image.size()==QSize(2,2),"B8 linear decodes at its size");
    check(image.pixel(1,0)==qRgba(0x40,0x40,0x40,0x40) && image.pixel(0,1)==qRgba(0x80,0x80,0x80,0x80),"B8 replicates the byte into grey and alpha");
    // B8 linear with a row pitch wider than the row.
    image=decodeGtf(gtf(0xa1,2,2,QByteArray::fromHex("1020ffff3040ffff"),4),&error);
    check(qRed(image.pixel(0,1))==0x30 && qRed(image.pixel(1,1))==0x40,"a linear pitch skips the row padding");
    // A8R8G8B8 swizzled (0x85): Morton order, x bit first. Pixel n carries red=n.
    QByteArray argb;for(int n=0;n<16;++n)argb+=QByteArray::fromHex("ff")+char(n)+char(0)+char(0);
    image=decodeGtf(gtf(0x85,4,4,argb),&error);
    check(!image.isNull() && qRed(image.pixel(1,0))==1 && qRed(image.pixel(0,1))==2 && qRed(image.pixel(1,1))==3 && qRed(image.pixel(2,0))==4 && qRed(image.pixel(3,3))==15,
          "A8R8G8B8 swizzled follows Morton order");
    check(qAlpha(image.pixel(0,0))==255,"A8R8G8B8 takes alpha from the first byte");
    // A8R8G8B8 linear (0xA5): plain rows.
    image=decodeGtf(gtf(0xa5,4,4,argb),&error);
    check(qRed(image.pixel(0,1))==4 && qRed(image.pixel(1,0))==1,"A8R8G8B8 linear is row-major");
    // DXT1 (0x86): c0 red, c1 blue (c0>c1: four colours), indices 0 then 1 per row.
    QByteArray dxt1=QByteArray::fromHex("00f81f00")+QByteArray::fromHex("00550000");
    image=decodeGtf(gtf(0x86,4,4,dxt1),&error);
    check(!image.isNull() && image.pixel(0,0)==qRgba(255,0,0,255),"DXT1 index 0 is colour 0");
    check(image.pixel(0,1)==qRgba(0,0,255,255),"DXT1 index 1 is colour 1");
    // DXT1 with c0<=c1: index 3 is transparent black.
    image=decodeGtf(gtf(0x86,4,4,QByteArray::fromHex("1f0000f8ffffffff")),&error);
    check(image.pixel(2,2)==qRgba(0,0,0,0),"DXT1 three-colour mode makes index 3 transparent");
    // DXT5 (0x88): alpha endpoints 255/0, alpha index 1 (=0) everywhere; colour block all white.
    const QByteArray dxt5=QByteArray::fromHex("ff00")+QByteArray::fromHex("499224499224")+QByteArray::fromHex("ffffffff00000000");
    image=decodeGtf(gtf(0x88,4,4,dxt5),&error);
    check(!image.isNull() && qAlpha(image.pixel(3,3))==0 && qRed(image.pixel(3,3))==255,"DXT5 interpolated alpha index 1 is the second endpoint");
    // DXT3 (0x87): explicit 4-bit alpha, nibble 0x8 -> 136.
    const QByteArray dxt3=QByteArray::fromHex("8888888888888888")+QByteArray::fromHex("ffffffff00000000");
    image=decodeGtf(gtf(0x87,4,4,dxt3),&error);
    check(qAlpha(image.pixel(1,2))==136,"DXT3 explicit alpha nibbles scale by 17");
    // Refusals.
    check(decodeGtf(QByteArray(0x10,'\0'),&error).isNull() && !error.isEmpty(),"a truncated header is refused with an error");
    check(decodeGtf(gtf(0x83,2,2,QByteArray(16,'\0')),&error).isNull() && error.contains(QStringLiteral("unsupported")),"an unknown format is refused");
    check(decodeGtf(gtf(0x85,3,4,QByteArray(48,'\0')),&error).isNull(),"swizzled non-power-of-two dimensions are refused");
    check(decodeGtf(gtf(0x85,4,4,QByteArray(10,'\0')),&error).isNull() && error.contains(QStringLiteral("truncated")),"short pixel data is refused");
    QByteArray lying=gtf(0xa1,2,2,QByteArray(4,'\0'));be32(lying,0x14,0x7fffffff);
    check(decodeGtf(lying,&error).isNull(),"a declared size past the end of the file is refused");
}

// Edge: one segment; the vertex descriptors are the one combination edge_decode.cpp accepts (float3 position,
// X11Y11Z10 normal, half2 uv). Quad = triangles (0,1,2) and (0,2,3); vertex 3 comes from an explicit delta.
static QByteArray edge(quint32 segments=1,bool wrongDescriptor=false) {
    const int seg=0x10,pos=0x100,normal=0x140,uv=0x160,index=0x180,desc=0x1a0,uvDesc=0x1c0;
    QByteArray b(0x1e0,'\0');
    be32(b,0,segments);be32(b,4,seg);
    be16(b,seg+8,4);be16(b,seg+10,6);be32(b,seg+0x10,index);be16(b,seg+0x14,12);
    be32(b,seg+0x18,pos);be32(b,seg+0x1c,normal);be32(b,seg+0x2c,uv);be32(b,seg+0x70,desc);be32(b,seg+0x74,uvDesc);
    const float p[4][3]={{1,2,3},{-1,0,0},{0,1,0},{0.5f,-0.5f,2}};
    for(int v=0;v<4;++v)for(int c=0;c<3;++c)be32(b,pos+v*12+c*4,std::bit_cast<quint32>(p[v][c]));
    for(int v=0;v<4;++v)be32(b,normal+v*4,1023u);          // +X: 11-bit snorm 1023 -> 1.0
    for(int v=0;v<4;++v){be16(b,uv+v*4,0x3c00);be16(b,uv+v*4+2,0x3800);} // (1.0, 0.5)
    // Index stream: 1 delta, bias 0, 1 selector byte, 4-bit deltas; selectors 0,0,0,1; codes 3,0; delta 3.
    be16(b,index,1);be16(b,index+2,0);be16(b,index+4,1);b[index+6]=4;b[index+7]=0;
    b[index+8]=char(0x10);b[index+9]=QByteArray::fromHex("c0").at(0);b[index+10]=char(0x30);
    QByteArray d1=QByteArray::fromHex("0210020000000000000203010c0000000c06010204020000");
    const QByteArray d2=QByteArray::fromHex("01040100000000000003020504080000");
    if(wrongDescriptor)d1[2]=char(0x7f);
    b.replace(desc,d1.size(),d1);b.replace(uvDesc,d2.size(),d2);
    return b;
}
static void edgeTests() {
    QVector<EdgeVertex> vertices;QVector<quint32> indices;QString error;
    const bool ok=decodeEdge(edge(),vertices,indices,&error);
    check(ok && vertices.size()==4 && indices.size()==6,"a one-segment Edge quad decodes to 4 vertices and 2 triangles");
    check(indices==QVector<quint32>({0,1,2,0,2,3}),"sequential selectors, an edge-reuse code and an explicit delta rebuild the indices");
    check(ok && vertices[0].position==QVector3D(1,2,3) && vertices[3].position==QVector3D(0.5f,-0.5f,2),"big-endian float positions");
    check(ok && std::abs(vertices[1].normal.x()-1.f)<1e-6f && vertices[1].normal.y()==0.f,"X11Y11Z10 normals are signed-normalised");
    check(ok && vertices[2].uv==QVector2D(1.f,.5f),"half-float texture coordinates");
    check(!decodeEdge(edge(2),vertices,indices,&error) && error.contains(QStringLiteral("segment")),"more than one segment is refused, not misread");
    {
        QByteArray twoSeg=edge(2);
        const int seg0=0x10,seg1=seg0+0x78;
        twoSeg.replace(seg1,0x78,twoSeg.mid(seg0,0x78));
        check(decodeEdge(twoSeg,vertices,indices,&error) && vertices.size()==8 && indices==QVector<quint32>({0,1,2,0,2,3,4,5,6,4,6,7}),
              "a populated two-segment Edge mesh decodes all 8 vertices and offsets segment 1 indices by +4");
    }
    check(!decodeEdge(edge(1,true),vertices,indices,&error) && error.contains(QStringLiteral("vertex stream")),"an unknown vertex format is refused");
    check(!decodeEdge(edge().left(0x40),vertices,indices,&error) && vertices.isEmpty() && indices.isEmpty(),"a truncated file is refused and leaves no output");
}

// VSMX: little-endian header, 8-byte instructions (opcode in the low byte of the first word), three string tables.
struct Op { quint32 id,value; };
static QByteArray utf16z(const QStringList& strings){QByteArray b;for(const auto& s:strings){for(QChar c:s){b+=char(c.unicode()&0xff);b+=char(c.unicode()>>8);}b+=QByteArray(2,'\0');}return b;}
static QByteArray vsmx(const QVector<Op>& code,const QStringList& text,const QStringList& props,const QStringList& names) {
    QByteArray body;for(const auto& o:code){char w[8];qToLittleEndian(o.id,w);qToLittleEndian(o.value,w+4);body.append(w,8);}
    const QByteArray t=utf16z(text),p=utf16z(props);QByteArray n;for(const auto& s:names)n+=s.toLatin1()+'\0';
    QByteArray b(0x34,'\0');b.replace(0,4,"VSMX");le32(b,4,0x20000);
    const quint32 codeAt=0x34,textAt=codeAt+quint32(body.size()),propAt=textAt+quint32(t.size()),nameAt=propAt+quint32(p.size());
    le32(b,8,codeAt);le32(b,12,quint32(body.size()));le32(b,0x10,textAt);le32(b,0x14,quint32(t.size()));
    le32(b,0x1c,propAt);le32(b,0x20,quint32(p.size()));le32(b,0x28,nameAt);le32(b,0x2c,quint32(n.size()));
    return b+body+t+p+n;
}
static void vsmxTests() {
    // logo = new Actor("logo"); logo.setPosition([1,2,3],0.5); d = new Date(); logo.color=[1,1,1,0];
    // new OneShotTimer(1, function(){ logo.setScale([2,2,2],0); }); <0x4d>; end
    const QVector<Op> code{
        {0x2e,0},{0x2e,1},{0x28,0},{0x3e,1},{0x01,0},{0x22,0},
        {0x2e,0},{0x30,0},{0x26,1},{0x26,2},{0x26,3},{0x49,3},{0x27,std::bit_cast<quint32>(0.5f)},{0x3d,2},{0x22,0},
        {0x2e,2},{0x2e,3},{0x3e,0},{0x01,0},{0x22,0},
        {0x2e,0},{0x26,1},{0x26,1},{0x26,1},{0x26,0},{0x49,4},{0x31,1},{0x22,0},
        {0x2e,4},{0x26,1},{0x2a,32},{0x39,42},
        {0x2e,0},{0x30,2},{0x26,2},{0x26,2},{0x26,2},{0x49,3},{0x26,0},{0x3d,2},{0x22,0},{0x3f,0},
        {0x3e,2},{0x22,0},
        {0x4d,7},{0x45,0}};
    const QByteArray script=vsmx(code,{"logo"},{"setPosition","color","setScale"},{"logo","Actor","d","Date","OneShotTimer"});
    VsmxProgram program;QString error;
    check(disassembleVsmx(script,&program,&error) && program.code.size()==code.size(),"the disassembler lists every instruction");
    check(program.code[2].mnemonic==QStringLiteral("CONST_STRING") && program.code[2].operands==QStringLiteral("0: logo"),"string operands resolve through the UTF-16 text table");
    check(program.code[7].operands==QStringLiteral("0: setPosition"),"property operands resolve through the property table");
    check(program.code[12].operands==QStringLiteral("0.5"),"float operands are IEEE singles");
    check(program.code[44].mnemonic==QStringLiteral("UNKNOWN") && program.code[44].operands.startsWith(QStringLiteral("raw=")),"an unknown opcode is listed raw, not guessed");
    const VsmxUsage usage=vsmxUsage(program);
    check(usage.constructors.value(QStringLiteral("Actor"))==1 && usage.constructors.value(QStringLiteral("Date"))==1,"constructors are named from the symbolic stack");
    check(usage.methods.value(QStringLiteral("setPosition"))==1 && usage.methods.value(QStringLiteral("setScale"))==1 && usage.methods.size()==2,"method calls are named, inside function bodies too");
    check(usage.constructors.value(QStringLiteral("OneShotTimer"))==1 && usage.functions==1,"a constructor whose argument is a function literal still resolves");
    check(usage.globals==QStringList({"Actor","Date","OneShotTimer"}),"runtime globals are the raf.prx names the script touches");
    check(usage.propertiesWritten==QStringList({"color"}) && usage.unknownOpcodes==QStringList({"0x4d"}),"property writes and unknown opcodes are collected");
    VsmxTimeline timeline;
    check(evalVsmx(script,&timeline,&error),"the evaluator runs the synthetic script");
    const QVector4D mid=sample(timeline,QStringLiteral("logo"),VsmxKey::Position,0.25);
    check(std::abs(mid.x()-.5f)<1e-5f && std::abs(mid.z()-1.5f)<1e-5f,"setPosition over 0.5 s is interpolated (linear) at 0.25 s");
    check(sample(timeline,QStringLiteral("logo"),VsmxKey::Position,9).y()==2.f,"after the move the target holds");
    check(sample(timeline,QStringLiteral("logo"),VsmxKey::Scale,2).x()==2.f,"a OneShotTimer body runs at its delay");
    bool stopped=false;for(const auto& n:timeline.notEvaluated)if(n.contains(QStringLiteral("0x4d")))stopped=true;
    check(stopped,"an unsupported opcode is reported in notEvaluated");
    check(!disassembleVsmx(QByteArray("VSMX"),&program,&error) && !error.isEmpty(),"a truncated header is refused");
    QByteArray bad=script;le32(bad,12,0x7ffffff8);
    check(!disassembleVsmx(bad,&program,&error) && error.contains(QStringLiteral("code region")),"a code region past the end is refused");
}

static void vsmxAnimatedTests() {
    // Dynamic theme subset: Date.getHours(), Math.PI / Math.sin, Light.setAttenuation, Actor.setRotation with smooth curve,
    // initialPose preservation, and periodic IntervalTimer looping.
    //
    // ring = new Actor("ring"); sun = new Light("sun"); d = new Date();
    // h = d.getHours(); s = Math.sin(Math.PI / 2);
    // sun.setAttenuation([1, h, s], 1, 1);
    // new IntervalTimer(2, function(){ ring.setRotation([0, ring.rotation[1] + 1, 0], 2, 0); });
    const QVector<Op> code{
        /* 0  */ {0x2e,0},{0x2e,1},{0x28,0},{0x3e,1},{0x01,0},{0x22,0},               // ring = new Actor("ring");
        /* 6  */ {0x2e,2},{0x2e,3},{0x28,1},{0x3e,1},{0x01,0},{0x22,0},               // sun = new Light("sun");
        /* 12 */ {0x2e,4},{0x2e,5},{0x3e,0},{0x01,0},{0x22,0},                        // d = new Date();
        /* 17 */ {0x2e,6},{0x2e,4},{0x30,0},{0x3d,0},{0x01,0},{0x22,0},               // h = d.getHours();
        /* 23 */ {0x2e,7},{0x2e,8},{0x30,1},{0x2e,8},{0x2f,2},{0x26,2},{0x05,0},{0x3d,1},{0x01,0},{0x22,0}, // s = Math.sin(Math.PI / 2);
        /* 33 */ {0x2e,2},{0x30,3},{0x26,1},{0x2e,6},{0x2e,7},{0x49,3},{0x26,1},{0x26,1},{0x3d,3},{0x22,0}, // sun.setAttenuation([1, h, s], 1, 1);
        /* 43 */ {0x2e,9},{0x26,2},{0x2a,47},{0x39,63},                               // new IntervalTimer(2, function() {
        /* 47 */ {0x2e,0},{0x30,4},{0x26,0},{0x2e,0},{0x2f,5},{0x26,1},{0x34,0},{0x26,1},{0x02,0},{0x26,0},{0x49,3},{0x26,2},{0x26,0},{0x3d,3},{0x22,0},{0x3f,0},
        /* 63 */ {0x3e,2},{0x22,0},{0x45,0}};                                         // });
    const QByteArray script=vsmx(
        code,
        {"ring","sun"},
        {"getHours","sin","PI","setAttenuation","setRotation","rotation"},
        {"ring","Actor","sun","Light","d","Date","h","s","Math","IntervalTimer"});
    VsmxEvalOptions opts;
    opts.hour=18;opts.maxIntervalTicks=4;
    opts.initialPose[QStringLiteral("sun")][VsmxKey::Attenuation]=QVector4D(1.f,0.f,0.f,0.f);
    opts.initialPose[QStringLiteral("ring")][VsmxKey::Rotation]=QVector4D(0.f,0.5f,0.f,0.f);
    VsmxTimeline timeline;QString error;
    check(evalVsmx(script,&timeline,&error,opts),"the extended evaluator runs Date, Math, Light.setAttenuation and IntervalTimer");
    check(std::abs(timeline.constants.value(QStringLiteral("h"))-18.)<1e-6,"Date.getHours() reads the configured clock hour");
    check(std::abs(timeline.constants.value(QStringLiteral("s"))-1.)<1e-6,"Math.sin(Math.PI / 2) evaluates to 1.0");
    // Smooth curve (3rd arg = 1): Hermite smoothstep at u=0.25 is 0.25^2 * 2.5 = 0.15625 (linear would be 0.25).
    const QVector4D attQuarter=sample(timeline,QStringLiteral("sun"),VsmxKey::Attenuation,0.25);
    check(std::abs(attQuarter.x()-1.f)<1e-5f && std::abs(attQuarter.y()-float(18.*0.15625))<1e-4f && std::abs(attQuarter.z()-0.15625f)<1e-4f,
          "setAttenuation starts from initialPose and eases with Hermite smoothstep when curve != 0");
    check(timeline.hasMotion() && std::abs(timeline.loopPeriod-8.)<1e-6,"IntervalTimer(2) over 4 ticks records an 8 s periodic loop");
    // Tick 1 fires at t=2 and rotates y from initialPose 0.5 -> 1.5 over [2, 4]; tick 2 rotates 1.5 -> 2.5 over [4, 6]; wraps modulo 8 s.
    const QVector4D rotAt3=sample(timeline,QStringLiteral("ring"),VsmxKey::Rotation,3.0);
    const QVector4D rotWrapped=sample(timeline,QStringLiteral("ring"),VsmxKey::Rotation,11.0); // 11 % 8 == 3
    check(std::abs(rotAt3.y()-1.0f)<1e-4f,"IntervalTimer body reads actor.rotation[1] and advances setRotation");
    check(std::abs(rotWrapped.y()-rotAt3.y())<1e-4f,"periodic IntervalTimer tracks wrap cleanly past loopPeriod");
}

static void tvColumnPreferenceTests() {
    QTemporaryDir dir;
    check(dir.isValid(),"scratch directory for tvColumn preference checks");
    Preferences prefs(dir.path());
    check(!prefs.contains(QStringLiteral("appearance/tvColumn")) && prefs.value(QStringLiteral("appearance/tvColumn"))==QVariant(true),
          "appearance/tvColumn defaults to true (10 columns out of the box) before any write");
    check(prefs.setValue(QStringLiteral("appearance/tvColumn"),false) && prefs.value(QStringLiteral("appearance/tvColumn"))==QVariant(false),
          "appearance/tvColumn accepts false (console 9-column mode) and persists");
    Preferences reloaded(dir.path());
    check(reloaded.value(QStringLiteral("appearance/tvColumn"))==QVariant(false),
          "appearance/tvColumn round-trips across disk reload");
    check(!prefs.setValue(QStringLiteral("appearance/tvColumn"),1) && !prefs.setValue(QStringLiteral("appearance/tvColumn"),QStringLiteral("off")),
          "appearance/tvColumn rejects non-boolean values");
}

static void rafEffectMappingTests() {
    // Traced from raf.prx FUN_00031e68 (0x031ea0..0x032c80), FUN_0003f5e0 (0x03f5e0..0x03f874),
    // 0x0404a4..0x0405f8 and FUN_00040fb0 (0x040fb0..0x041260).
    const RafEffectSpec sce=resolveRafEffect(QStringLiteral("sce01"));
    check(sce.known && sce.shaderIndex==16 && !sce.unlit && sce.firmwareProgram==QStringLiteral("lib/raf/sce01.fpo"),
          "sce01 resolves to shader index 16 (lib/raf/sce01.fpo)");

    const RafEffectSpec pure=resolveRafEffect(QStringLiteral("pure_texture_alpha_0_depth_1"));
    check(pure.known && pure.shaderIndex==1 && pure.unlit && !pure.blendEnable && !pure.alphaTest &&
          pure.depthTest && pure.depthMask && pure.depthFunc==0x0203 &&
          pure.firmwareProgram==QStringLiteral("lib/raf/_gen_ubber1_1000.fpo"),
          "pure_texture_alpha_0_depth_1 maps to _gen_ubber1_1000.fpo with blend off and GL_LEQUAL depth");

    const RafEffectSpec lit0001=resolveRafEffect(QStringLiteral("basic_lighting_edge_lit"),false,false,false);
    check(lit0001.known && lit0001.shaderIndex==8 && !lit0001.unlit && lit0001.edgeLit &&
          lit0001.firmwareProgram==QStringLiteral("lib/raf/_gen_ubber1_0001.fpo"),
          "basic_lighting_edge_lit with no normal/reflection map maps to _gen_ubber1_0001.fpo and edgeLit=true");

    const RafEffectSpec lit1001=resolveRafEffect(QStringLiteral("basic_lighting"),true,false,false);
    check(lit1001.known && lit1001.shaderIndex==9 && lit1001.firmwareProgram==QStringLiteral("lib/raf/_gen_ubber1_1001.fpo"),
          "basic_lighting with object-space normal map (type 1) maps to _gen_ubber1_1001.fpo");

    const RafEffectSpec lit1101=resolveRafEffect(QStringLiteral("basic_lighting"),false,true,false);
    check(lit1101.known && lit1101.shaderIndex==11 && lit1101.firmwareProgram==QStringLiteral("lib/raf/_gen_ubber1_1101.fpo"),
          "basic_lighting with tangent-space normal map (type 3) maps to _gen_ubber1_1101.fpo");

    const RafEffectSpec lit1111=resolveRafEffect(QStringLiteral("basic_lighting_alpha_add_depth_0"),true,true,true);
    check(lit1111.known && lit1111.shaderIndex==15 && lit1111.firmwareProgram==QStringLiteral("lib/raf/_gen_ubber1_1111.fpo") &&
          lit1111.blendEnable && lit1111.blendSrc==1 && lit1111.blendDst==1 &&
          !lit1111.depthTest && !lit1111.depthMask && lit1111.depthFunc==0x0207,
          "alpha_add sets additive GL_ONE/GL_ONE blend, depth_0 sets GL_ALWAYS depth off, and reflection+tangent sets _gen_ubber1_1111.fpo");
}

int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    gtfTests();
    edgeTests();
    vsmxTests();
    vsmxAnimatedTests();
    tvColumnPreferenceTests();
    rafEffectMappingTests();
    std::printf("%s\n",failures?"FAILED":"all passed");
    return failures?1:0;
}
