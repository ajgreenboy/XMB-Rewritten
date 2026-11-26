#pragma once
#include <QByteArray>
#include <QMap>
#include <QStringList>
#include <QVector4D>

namespace xmb {
struct VsmxKey {
    enum Kind { Alpha, Position, Scale, Colour, Rotation, UvOffset, UvScale, Attenuation } kind=Alpha;
    enum Curve { Linear, Smooth } curve=Linear;
    double time=0;
    QVector4D value;
};
struct VsmxTimeline {
    QMap<QString,double> constants;
    QMap<QString,QString> strings;
    QMap<QString,QVector<VsmxKey>> actors;
    QStringList notEvaluated;
    double loopPeriod=0; // > 0 when an IntervalTimer makes the timeline periodic
    bool hasMotion() const;
};
struct VsmxEvalOptions {
    int resolution=1080;
    double aspect=16.0/9.0;
    int hour=12,minute=0,second=0,millisecond=0;
    int maxIntervalTicks=8;
    // Initial scene pose per actor/light/camera so timed moves start from the RAF tree's authored values
    // rather than zero when the script issues a set* call before writing the property directly.
    QMap<QString,QMap<VsmxKey::Kind,QVector4D>> initialPose;
};
bool evalVsmx(const QByteArray& data,VsmxTimeline* out,QString* error=nullptr,const VsmxEvalOptions& options={});
bool hasKeyKind(const VsmxTimeline& timeline,const QString& actor,VsmxKey::Kind kind);
QVector4D sample(const VsmxTimeline& timeline,const QString& actor,VsmxKey::Kind kind,double t,const QVector4D& fallback={});

// VSMX disassembly (read-only, for --raf-report). Header words are little-endian: code region at 8/12, text (UTF-16)
// at 0x10, property names (UTF-16) at 0x1c, variable names (Latin-1) at 0x28; every instruction is 8 bytes (u32 id
// whose low byte is the opcode, u32 operand). Tables are indexed strings, not byte offsets.
struct VsmxInstruction { quint32 index=0,id=0,value=0; int op=0; QString mnemonic,operands; };
struct VsmxProgram { QStringList text,properties,names; QVector<VsmxInstruction> code; };
bool disassembleVsmx(const QByteArray& data,VsmxProgram* out,QString* error=nullptr);
// What a script uses, from a linear sweep with a symbolic operand stack (reset at each END_STATEMENT; jumps are not
// followed except a function literal's skip-over jump, so a callee whose expression straddles a branch reads as "?").
// The globals are the names raf.prx registers for scripts (Actor, Camera, Light, IntervalTimer, OneShotTimer, Date,
// Math, System).
struct VsmxUsage {
    QMap<QString,int> opcodes;                  // "0x2e VARIABLE" -> count
    QMap<QString,int> constructors,methods,calls; // NEW callee, METHOD_CALL method name, CALL callee
    QStringList globals,propertiesRead,propertiesWritten,unknownOpcodes;
    int functions=0;
};
VsmxUsage vsmxUsage(const VsmxProgram& program);
const QStringList& vsmxRuntimeGlobals();
}
