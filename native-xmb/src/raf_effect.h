#pragma once
#include <QString>
#include <initializer_list>

namespace xmb {

// Traced from raf.prx FUN_00031e68 (0x031ea0..0x032c80), FUN_0003f5e0 (0x03f5e0..0x03f874),
// 0x0404a4..0x0405f8 and FUN_00040fb0 (0x040fb0..0x041260): maps a material's effect string and
// bound texture slots to the firmware's raf.qrc fragment program name and GL blend/depth/alpha state.
struct RafEffectSpec {
    QString baseEffect=QStringLiteral("pure_texture"); // "pure_texture", "basic_lighting", or "sce01"
    QString firmwareProgram=QStringLiteral("lib/raf/_gen_ubber1_1000.fpo");
    int shaderIndex=1;        // 0..15 = _gen_ubber1_XXXX.fpo (bit 0..3 -> digit 0..3), 16 = sce01.fpo
    bool unlit=true;          // true for pure_texture (_gen_ubber1_1000.fpo)
    bool edgeLit=false;       // "edge_lit" token (+0x1c)
    bool depthTest=true;      // "depth_0" -> false, "depth_1" / default -> true (+0x20)
    bool depthMask=true;      // "depth_0" -> false, "depth_1" / default -> true (+0x28)
    quint32 depthFunc=0x0203; // GL_LEQUAL (0x0203), or GL_ALWAYS (0x0207) for "depth_0" (+0x24)
    bool alphaTest=true;      // "alpha_0" -> false, "alpha_1" / "alpha_add" / default -> true (+0x2c)
    quint32 alphaFunc=0x0204; // GL_GREATER (0x0204) (+0x30)
    float alphaRef=0.f;       // (+0x34)
    bool blendEnable=true;    // "alpha_0" -> false, "alpha_1" / "alpha_add" / default -> true (+0x38)
    quint32 blendSrc=0x0302;  // GL_SRC_ALPHA (0x0302), or GL_ONE (1) for "alpha_add" (+0x3c)
    quint32 blendDst=0x0303;  // GL_ONE_MINUS_SRC_ALPHA (0x0303), or GL_ONE (1) for "alpha_add" (+0x40)
    bool hasExplicitDepthToken=false;
    bool known=true;
};

inline RafEffectSpec resolveRafEffect(const QString& rawEffect,bool hasObjectNormalMap=false,bool hasTangentNormalMap=false,bool hasReflectionMap=false) {
    RafEffectSpec spec;
    const QString e=rawEffect.trimmed().toLower();
    // FUN_00031e68 (0x031fb0..0x032400): substring scan in exact firmware order:
    //   pure_texture, basic_lighting, sce01, edge_lit, alpha_0, alpha_1, alpha_add, depth_0, depth_1.
    bool matchedBase=false;
    if(e.contains(QStringLiteral("pure_texture"))){spec.baseEffect=QStringLiteral("pure_texture");matchedBase=true;}
    if(e.contains(QStringLiteral("basic_lighting"))){spec.baseEffect=QStringLiteral("basic_lighting");matchedBase=true;}
    if(e.contains(QStringLiteral("sce01"))){spec.baseEffect=QStringLiteral("sce01");matchedBase=true;}
    if(e.contains(QStringLiteral("edge_lit")))spec.edgeLit=true;
    if(e.contains(QStringLiteral("alpha_0"))){spec.blendEnable=false;spec.alphaTest=false;}
    if(e.contains(QStringLiteral("alpha_1"))){
        spec.alphaTest=true;spec.alphaFunc=0x0204;spec.alphaRef=0.f;
        spec.blendEnable=true;spec.blendSrc=0x0302;spec.blendDst=0x0303;
    }
    if(e.contains(QStringLiteral("alpha_add"))){
        spec.alphaTest=true;spec.alphaFunc=0x0204;spec.alphaRef=0.f;
        spec.blendEnable=true;spec.blendSrc=1;spec.blendDst=1;
    }
    if(e.contains(QStringLiteral("depth_0"))){
        spec.depthTest=false;spec.depthFunc=0x0207;spec.depthMask=false;spec.hasExplicitDepthToken=true;
    }
    if(e.contains(QStringLiteral("depth_1"))){
        spec.depthTest=true;spec.depthFunc=0x0203;spec.depthMask=true;spec.hasExplicitDepthToken=true;
    }
    QString rem=e;
    for(const QString& tok:{QStringLiteral("pure_texture"),QStringLiteral("basic_lighting"),QStringLiteral("sce01"),
                            QStringLiteral("edge_lit"),QStringLiteral("alpha_0"),QStringLiteral("alpha_1"),
                            QStringLiteral("alpha_add"),QStringLiteral("depth_0"),QStringLiteral("depth_1")})
        rem.replace(tok,QStringLiteral("_"));
    rem.replace(QLatin1Char('_'),QString());
    spec.known=matchedBase && rem.isEmpty();

    // FUN_0003f5e0 (0x03f740..0x03f874) & 0x0404a4..0x0405f8 & FUN_00040fb0 (0x040fb0..0x041260):
    if(spec.baseEffect==QStringLiteral("sce01")) {
        spec.shaderIndex=16;
        spec.unlit=false;
        spec.firmwareProgram=QStringLiteral("lib/raf/sce01.fpo");
    } else if(spec.baseEffect==QStringLiteral("pure_texture") && spec.known) {
        spec.shaderIndex=1;
        spec.unlit=true;
        spec.firmwareProgram=QStringLiteral("lib/raf/_gen_ubber1_1000.fpo");
    } else {
        int flags=0;
        if(hasTangentNormalMap)flags=3;
        else if(hasObjectNormalMap)flags=1;
        if(hasReflectionMap)flags|=4;
        flags|=8; // +0x180 lighting enable bit
        spec.shaderIndex=flags;
        spec.unlit=false;
        QString digits;
        for(int bit=0;bit<4;++bit)digits+=QLatin1Char(((flags>>bit)&1)?'1':'0');
        spec.firmwareProgram=QStringLiteral("lib/raf/_gen_ubber1_%1.fpo").arg(digits);
    }
    return spec;
}

} // namespace xmb
