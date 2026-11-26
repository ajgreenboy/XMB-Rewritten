#include "vsmx_eval.h"
#include <QtEndian>
#include <algorithm>
#include <bit>
#include <cmath>
#include <functional>
#include <iterator>
#include <numbers>

namespace xmb {
bool VsmxTimeline::hasMotion() const {
    if(loopPeriod>0)return true;
    for(auto it=actors.cbegin();it!=actors.cend();++it)
        for(const auto& k:it.value())if(k.time>0)return true;
    return false;
}
bool hasKeyKind(const VsmxTimeline& timeline,const QString& actor,VsmxKey::Kind kind) {
    const auto keys=timeline.actors.value(actor);
    for(const auto& key:keys)if(key.kind==kind)return true;
    return false;
}
QVector4D sample(const VsmxTimeline& timeline,const QString& actor,VsmxKey::Kind kind,double t,const QVector4D& fallback) {
    if(timeline.loopPeriod>0. && t>timeline.loopPeriod)t=std::fmod(t,timeline.loopPeriod);
    const auto keys=timeline.actors.value(actor);
    const VsmxKey* previous=nullptr;
    for(const auto& key:keys)if(key.kind==kind) {
        if(key.time>t) {
            if(!previous)return key.value;
            const double span=key.time-previous->time;
            if(span<=0.)return key.value;
            double u=std::clamp((t-previous->time)/span,0.,1.);
            if(key.curve==VsmxKey::Smooth)u=u*u*(3.-2.*u);
            return previous->value+(key.value-previous->value)*float(u);
        }
        previous=&key;
    }
    return previous?previous->value:fallback;
}

bool evalVsmx(const QByteArray& data,VsmxTimeline* out,QString* error,const VsmxEvalOptions& options) {
    const auto fail=[&](const QString& message){if(error)*error=message;return false;};
    if(!out || data.size()<0x34 || !data.startsWith("VSMX"))return fail(QStringLiteral("Invalid VSMX header"));
    const auto le=[&](qsizetype p){return qFromLittleEndian<quint32>(data.constData()+p);};
    const auto valid=[&](quint32 p,quint32 n){return quint64(p)+n<=quint64(data.size());};
    const quint32 code=le(8),size=le(12);
    if(!valid(code,size) || size%8)return fail(QStringLiteral("Invalid VSMX code region"));
    const auto table=[&](int p,bool wide,QStringList& result) {
        const quint32 off=le(p),len=le(p+4);
        if(!valid(off,len) || (wide && len%2))return false;
        if(!wide){for(const auto& s:data.mid(off,len).split('\0'))result.append(QString::fromLatin1(s));}
        else {QString s;for(quint32 i=0;i<len;i+=2){const ushort c=qFromLittleEndian<quint16>(data.constData()+off+i);if(c)s.append(QChar(c));else {result.append(s);s.clear();}}if(!s.isEmpty())result.append(s);}
        return true;
    };
    QStringList names,props,text;
    if(!table(0x10,true,text) || !table(0x1c,true,props) || !table(0x28,false,names))return fail(QStringLiteral("Invalid VSMX string table"));
    *out={};
    struct Value {
        enum Type { Unknown, Number, String, Ref, Object, DateObj, Array, Function, Method } type=Unknown;
        double n=0;
        QString s,property;
        QVector<double> array;
        quint32 target=0;
        Value::Type receiverType=Unknown;
    };
    const auto number=[](double n){Value v;v.type=Value::Number;v.n=n;return v;};
    const auto vecValue=[](const QVector4D& v,int n){
        Value x;x.type=Value::Array;x.array.resize(n);
        for(int i=0;i<n;++i)x.array[i]=double(v[i]);
        return x;
    };
    QMap<QString,Value> vars;
    const auto note=[&](const QString& s){if(!out->notEvaluated.contains(s))out->notEvaluated.append(s);};
    note(QStringLiteral("System.resolution=%1; Bezier curves approximated with Hermite smoothstep").arg(options.resolution));
    const auto initialFor=[&](const QString& actor,VsmxKey::Kind kind)->QVector4D {
        const auto actorIt=options.initialPose.constFind(actor);
        if(actorIt!=options.initialPose.cend()) {
            const auto kindIt=actorIt->constFind(kind);
            if(kindIt!=actorIt->cend())return *kindIt;
        }
        return {};
    };
    const auto currentFor=[&](const QString& actor,VsmxKey::Kind kind,double now)->QVector4D {
        return sample(*out,actor,kind,now,initialFor(actor,kind));
    };
    const auto key=[&](const QString& actor,VsmxKey::Kind kind,double time,const QVector4D& value,VsmxKey::Curve curve=VsmxKey::Linear){
        out->actors[actor].append({kind,curve,time,value});
        std::stable_sort(out->actors[actor].begin(),out->actors[actor].end(),[](const VsmxKey& a,const VsmxKey& b){return a.time<b.time;});
    };
    int budget=150000;
    quint32 rngState=0x9e3779b9u;
    std::function<bool(quint32,double,int,const QVector<Value>&,Value*)> run;
    run=[&](quint32 pc,double now,int depth,const QVector<Value>& callArgs,Value* retOut)->bool {
        if(depth>32)return false;
        QVector<Value> stack;QMap<QString,Value> locals;
        for(int i=0;i<callArgs.size();++i)locals.insert(QStringLiteral("#arg%1").arg(i),callArgs[i]);
        const auto resolve=[&](Value v){
            if(v.type==Value::Ref) {
                if(locals.contains(v.s))return locals.value(v.s);
                if(vars.contains(v.s))return vars.value(v.s);
            }
            return v;
        };
        const auto assignRef=[&](const QString& name,const Value& b) {
            if(name.startsWith(QLatin1Char('#')))locals[name]=b;
            else {
                vars[name]=b;
                if(b.type==Value::Number)out->constants[name]=b.n;
                else if(b.type==Value::String)out->strings[name]=b.s;
            }
        };
        const auto pop=[&](){if(stack.isEmpty())return Value{};return stack.takeLast();};
        while(pc<size/8 && --budget>0) {
            const quint32 id=le(code+pc*8),v=le(code+pc*8+4),op=id&255; ++pc;
            if(op==0x22){stack.clear();continue;}
            if(op==0x45)return true;
            if(op==0x3f){if(retOut && !stack.isEmpty())*retOut=resolve(pop());return true;}
            if(op==0x46 || op==0x47 || op==0x4a) {if(op==0x4a)note(QStringLiteral("Opcode 0x4a treated as a no-op for the HD resolution test"));continue;}
            if(op==0x26 || op==0x25){stack.append(number(op==0x25?double(v!=0):double(qint32(v))));continue;}
            if(op==0x27){stack.append(number(std::bit_cast<float>(v)));continue;}
            if(op==0x23){stack.append(Value{});continue;}
            if(op==0x28 || op==0x2e || op==0x2d) {
                Value x;
                if(op==0x2d){x.type=Value::Ref;x.s=QStringLiteral("#arg%1").arg(v);}
                else {
                    const auto& tab=op==0x28?text:names;
                    if(v>=quint32(tab.size()))return false;
                    x.type=op==0x28?Value::String:Value::Ref;
                    x.s=tab[int(v)];
                }
                stack.append(x);continue;
            }
            if(op==0x2a){Value x;x.type=Value::Function;x.target=v;stack.append(x);continue;}
            if(op==0x20){if(stack.isEmpty())return false;stack.append(stack.last());continue;}
            if(op==0x49){
                if(v>quint32(stack.size()))return false;
                Value x;x.type=Value::Array;x.array.resize(int(v));
                for(int i=int(v)-1;i>=0;--i){const Value a=resolve(pop());if(a.type!=Value::Number)x.type=Value::Unknown;else x.array[i]=a.n;}
                stack.append(x);continue;
            }
            if(op==0x34) { // INDEX: array[index]
                if(stack.size()<2)return false;
                const Value idx=resolve(pop()),arr=resolve(pop());
                Value x;
                if(arr.type==Value::Array && idx.type==Value::Number) {
                    const int i=int(idx.n);
                    if(i>=0 && i<arr.array.size())x=number(arr.array[i]);
                }
                stack.append(x);continue;
            }
            if(op==0x2f || op==0x30) {
                if(v>=quint32(props.size()) || stack.isEmpty())return false;
                const Value raw=pop(),a=resolve(raw);
                const QString p=props[int(v)];
                Value x;
                if(op==0x2f && a.type==Value::Ref && a.s==QStringLiteral("System")) {
                    if(p==QStringLiteral("resolution"))x=number(options.resolution);
                    else if(p==QStringLiteral("aspect"))x=number(options.aspect);
                    else if(p==QStringLiteral("timer"))x=number(now);
                } else if(op==0x2f && a.type==Value::Ref && a.s==QStringLiteral("Math")) {
                    if(p==QStringLiteral("PI"))x=number(std::numbers::pi);
                    else if(p==QStringLiteral("E"))x=number(std::numbers::e);
                } else if(op==0x2f && a.type==Value::Object) {
                    if(p==QStringLiteral("position"))x=vecValue(currentFor(a.s,VsmxKey::Position,now),3);
                    else if(p==QStringLiteral("rotation"))x=vecValue(currentFor(a.s,VsmxKey::Rotation,now),3);
                    else if(p==QStringLiteral("scale"))x=vecValue(currentFor(a.s,VsmxKey::Scale,now),3);
                    else if(p==QStringLiteral("color"))x=vecValue(currentFor(a.s,VsmxKey::Colour,now),4);
                    else if(p==QStringLiteral("uv_offset") || p==QStringLiteral("uvOffset"))x=vecValue(currentFor(a.s,VsmxKey::UvOffset,now),2);
                    else if(p==QStringLiteral("uv_scale") || p==QStringLiteral("uvScale"))x=vecValue(currentFor(a.s,VsmxKey::UvScale,now),2);
                } else {
                    x.type=op==0x30?Value::Method:Value::Unknown;
                    x.s=a.s;x.property=p;x.receiverType=a.type;
                }
                stack.append(x);continue;
            }
            if(op==0x01){
                if(stack.size()<2)return false;
                Value b=resolve(pop()),a=pop();
                if(a.type==Value::Ref)assignRef(a.s,b);
                stack.append(b);continue;
            }
            if(op>=0x0a && op<=0x0d) { // PRE/POST INCREMENT/DECREMENT
                if(stack.isEmpty())return false;
                Value a=pop(),cur=resolve(a);
                if(a.type==Value::Ref && cur.type==Value::Number) {
                    const double delta=(op==0x0a || op==0x0c)?1.:-1.;
                    const Value updated=number(cur.n+delta);
                    assignRef(a.s,updated);
                    stack.append((op==0x0a || op==0x0b)?updated:cur);
                } else stack.append(Value{});
                continue;
            }
            if(op==0x31) {
                if(v>=quint32(props.size()) || stack.size()<2)return false;
                Value b=resolve(pop()),a=resolve(pop());
                const QString p=props[int(v)];
                if(a.type==Value::Object && b.type==Value::Array && !a.s.isEmpty() && b.array.size()<=4) {
                    QVector4D val;for(int i=0;i<b.array.size();++i)val[i]=float(b.array[i]);
                    if(p==QStringLiteral("color") && b.array.size()==4){key(a.s,VsmxKey::Colour,now,val);key(a.s,VsmxKey::Alpha,now,QVector4D(0,0,0,val.w()));}
                    else if(p==QStringLiteral("position") && b.array.size()>=3)key(a.s,VsmxKey::Position,now,val);
                    else if(p==QStringLiteral("rotation") && b.array.size()>=3)key(a.s,VsmxKey::Rotation,now,val);
                    else if(p==QStringLiteral("scale") && b.array.size()>=3)key(a.s,VsmxKey::Scale,now,val);
                    else if((p==QStringLiteral("uv_offset") || p==QStringLiteral("uvOffset")) && b.array.size()>=2)key(a.s,VsmxKey::UvOffset,now,val);
                    else if((p==QStringLiteral("uv_scale") || p==QStringLiteral("uvScale")) && b.array.size()>=2)key(a.s,VsmxKey::UvScale,now,val);
                    else if(p==QStringLiteral("attenuation") && b.array.size()>=3)key(a.s,VsmxKey::Attenuation,now,val);
                }
                stack.append(b);continue;
            }
            if(op==0x08 || op==0x07 || op==0x09 || op==0x1c){
                if(stack.isEmpty())return false;
                Value a=resolve(pop());
                if(a.type==Value::Number)a.n=op==0x08?-a.n:op==0x09?double(!(a.n!=0.)):op==0x1c?double(~qint32(a.n)):a.n;
                stack.append(a);continue;
            }
            if((op>=2 && op<=6) || (op>=0x0e && op<=0x15) || (op>=0x19 && op<=0x1b) || (op>=0x1d && op<=0x1f)){
                if(stack.size()<2)return false;
                const Value b=resolve(pop()),a=resolve(pop());
                Value x;
                if(a.type==Value::Number && b.type==Value::Number){
                    double n=0;
                    switch(op){
                    case 2:n=a.n+b.n;break;case 3:n=a.n-b.n;break;case 4:n=a.n*b.n;break;
                    case 5:if(b.n==0)break;n=a.n/b.n;break;case 6:if(b.n==0)break;n=std::fmod(a.n,b.n);break;
                    case 0x0e:case 0x10:n=a.n==b.n;break;case 0x0f:case 0x11:n=a.n!=b.n;break;
                    case 0x12:n=a.n<b.n;break;case 0x13:n=a.n<=b.n;break;case 0x14:n=a.n>=b.n;break;case 0x15:n=a.n>b.n;break;
                    case 0x19:n=double(qint32(a.n)&qint32(b.n));break;
                    case 0x1a:n=double(qint32(a.n)^qint32(b.n));break;
                    case 0x1b:n=double(qint32(a.n)|qint32(b.n));break;
                    case 0x1d:n=double(qint32(a.n)<<(qint32(b.n)&31));break;
                    case 0x1e:n=double(qint32(a.n)>>(qint32(b.n)&31));break;
                    case 0x1f:n=double(quint32(qint32(a.n))>>(qint32(b.n)&31));break;
                    }
                    x=number(n);
                }
                stack.append(x);continue;
            }
            if(op==0x39){if(v>=size/8)return false;pc=v;continue;}
            if(op==0x3a || op==0x3b){
                if(stack.isEmpty())return false;
                const Value a=resolve(pop());
                if(a.type!=Value::Number){note(QStringLiteral("Unresolved conditional at instruction %1; execution stopped").arg(pc-1));return true;}
                if((a.n!=0)==(op==0x3a)){if(v>=size/8)return false;pc=v;}
                continue;
            }
            if(op==0x3c || op==0x3d || op==0x3e){
                if(v>quint32(stack.size()) || stack.size()<qsizetype(v)+1)return false;
                QVector<Value> args(int(v),Value{});
                for(int i=int(v)-1;i>=0;--i)args[i]=resolve(pop());
                const Value fn=resolve(pop());
                Value result;
                if(op==0x3e && fn.type==Value::Ref && (fn.s==QStringLiteral("Actor") || fn.s==QStringLiteral("Light") || fn.s==QStringLiteral("Camera")) && !args.isEmpty() && args[0].type==Value::String){
                    result.type=Value::Object;result.s=args[0].s;
                } else if(op==0x3e && fn.type==Value::Ref && fn.s==QStringLiteral("Date")) {
                    result.type=Value::DateObj;
                } else if(op==0x3e && fn.type==Value::Ref && fn.s==QStringLiteral("OneShotTimer") && args.size()==2 && args[0].type==Value::Number && args[1].type==Value::Function) {
                    if(!run(args[1].target,now+std::max(0.,args[0].n),depth+1,{},nullptr))return false;
                } else if(op==0x3e && fn.type==Value::Ref && fn.s==QStringLiteral("IntervalTimer") && args.size()==2 && args[0].type==Value::Number && args[1].type==Value::Function) {
                    const double period=args[0].n;
                    if(period>0. && options.maxIntervalTicks>0) {
                        for(int tick=1;tick<=options.maxIntervalTicks;++tick)
                            if(!run(args[1].target,now+period*tick,depth+1,{},nullptr))return false;
                        out->loopPeriod=std::max(out->loopPeriod,period*options.maxIntervalTicks);
                    }
                } else if(op==0x3c && fn.type==Value::Function) {
                    if(!run(fn.target,now,depth+1,args,&result))return false;
                } else if(op==0x3d && fn.type==Value::Method && fn.receiverType==Value::DateObj) {
                    if(fn.property==QStringLiteral("getHours"))result=number(options.hour);
                    else if(fn.property==QStringLiteral("getMinutes"))result=number(options.minute);
                    else if(fn.property==QStringLiteral("getSeconds"))result=number(options.second);
                    else if(fn.property==QStringLiteral("getMilliseconds"))result=number(options.millisecond);
                    else if(fn.property==QStringLiteral("getTime"))result=number(((options.hour*60.+options.minute)*60.+options.second)*1000.+options.millisecond);
                    else note(QStringLiteral("Unsupported Date.%1 at instruction %2").arg(fn.property).arg(pc-1));
                } else if(op==0x3d && fn.type==Value::Method && fn.s==QStringLiteral("Math")) {
                    const auto a0=args.size()>=1 && args[0].type==Value::Number?args[0].n:0.;
                    const auto a1=args.size()>=2 && args[1].type==Value::Number?args[1].n:0.;
                    const bool ok1=args.size()>=1 && args[0].type==Value::Number;
                    const bool ok2=ok1 && args.size()>=2 && args[1].type==Value::Number;
                    if(fn.property==QStringLiteral("sin") && ok1)result=number(std::sin(a0));
                    else if(fn.property==QStringLiteral("cos") && ok1)result=number(std::cos(a0));
                    else if(fn.property==QStringLiteral("tan") && ok1)result=number(std::tan(a0));
                    else if(fn.property==QStringLiteral("asin") && ok1)result=number(std::asin(std::clamp(a0,-1.,1.)));
                    else if(fn.property==QStringLiteral("acos") && ok1)result=number(std::acos(std::clamp(a0,-1.,1.)));
                    else if(fn.property==QStringLiteral("atan") && ok1)result=number(std::atan(a0));
                    else if(fn.property==QStringLiteral("atan2") && ok2)result=number(std::atan2(a0,a1));
                    else if(fn.property==QStringLiteral("abs") && ok1)result=number(std::abs(a0));
                    else if(fn.property==QStringLiteral("sqrt") && ok1)result=number(a0>=0.?std::sqrt(a0):0.);
                    else if(fn.property==QStringLiteral("floor") && ok1)result=number(std::floor(a0));
                    else if(fn.property==QStringLiteral("ceil") && ok1)result=number(std::ceil(a0));
                    else if(fn.property==QStringLiteral("round") && ok1)result=number(std::round(a0));
                    else if(fn.property==QStringLiteral("exp") && ok1)result=number(std::exp(a0));
                    else if(fn.property==QStringLiteral("log") && ok1)result=number(a0>0.?std::log(a0):0.);
                    else if(fn.property==QStringLiteral("pow") && ok2)result=number(std::pow(a0,a1));
                    else if(fn.property==QStringLiteral("min") && ok2)result=number(std::min(a0,a1));
                    else if(fn.property==QStringLiteral("max") && ok2)result=number(std::max(a0,a1));
                    else if(fn.property==QStringLiteral("random")) {
                        rngState=rngState*1664525u+1013904223u;
                        result=number(double(rngState>>8)/double(1u<<24));
                    } else note(QStringLiteral("Unsupported Math.%1 at instruction %2").arg(fn.property).arg(pc-1));
                } else if(op==0x3d && fn.type==Value::Method && args.size()>=2 && args[0].type==Value::Array && args[1].type==Value::Number && !fn.s.isEmpty()){
                    VsmxKey::Kind kind=VsmxKey::Colour;bool recognized=true;
                    if(fn.property==QStringLiteral("setColor"))kind=VsmxKey::Colour;
                    else if(fn.property==QStringLiteral("setPosition"))kind=VsmxKey::Position;
                    else if(fn.property==QStringLiteral("setScale"))kind=VsmxKey::Scale;
                    else if(fn.property==QStringLiteral("setRotation"))kind=VsmxKey::Rotation;
                    else if(fn.property==QStringLiteral("setUvOffset"))kind=VsmxKey::UvOffset;
                    else if(fn.property==QStringLiteral("setUvScale"))kind=VsmxKey::UvScale;
                    else if(fn.property==QStringLiteral("setAttenuation"))kind=VsmxKey::Attenuation;
                    else recognized=false;
                    if(recognized && args[1].n>=0 && args[0].array.size()<=4){
                        QVector4D val;for(int i=0;i<args[0].array.size();++i)val[i]=float(args[0].array[i]);
                        const VsmxKey::Curve curve=(args.size()>=3 && args[2].type==Value::Number && args[2].n!=0.)
                            ?VsmxKey::Smooth:VsmxKey::Linear;
                        const auto add=[&](VsmxKey::Kind k,const QVector4D& target){
                            const QVector4D initial=currentFor(fn.s,k,now);
                            key(fn.s,k,now,initial,VsmxKey::Linear);
                            key(fn.s,k,now+args[1].n,target,curve);
                        };
                        add(kind,val);
                        if(kind==VsmxKey::Colour && args[0].array.size()==4)add(VsmxKey::Alpha,QVector4D(0,0,0,val.w()));
                    } else note(QStringLiteral("Unsupported method %1 at instruction %2").arg(fn.property).arg(pc-1));
                } else note(QStringLiteral("Unsupported call at instruction %1").arg(pc-1));
                stack.append(result);continue;
            }
            if(op==0x36){if(stack.size()<3)return false;pop();pop();pop();stack.append(Value{});continue;}
            note(QStringLiteral("Unsupported opcode 0x%1 at instruction %2; execution stopped").arg(op,2,16,QChar('0')).arg(pc-1));return true;
        }
        return budget>0;
    };
    if(!run(0,0,0,{},nullptr))return fail(QStringLiteral("Invalid VSMX execution or instruction limit exceeded"));
    return true;
}
}

namespace xmb {
namespace {
// Opcode names as the community VSMX decompilers use them; gaps are opcodes never seen in a firmware script.
const char* const kMnemonic[]={
    nullptr,"ASSIGN","ADD","SUBTRACT","MULTIPLY","DIVIDE","MODULO","POSITIVE","NEGATE","NOT",
    "PRE_INCREMENT","PRE_DECREMENT","POST_INCREMENT","POST_DECREMENT","EQUAL","NOT_EQUAL","STRICT_EQUAL","STRICT_NOT_EQUAL",
    "LESS","LESS_EQUAL","GREATER_EQUAL","GREATER",nullptr,nullptr,"TYPEOF","BIT_AND","BIT_XOR","BIT_OR","BIT_NOT",
    "SHIFT_LEFT","SHIFT_RIGHT","SHIFT_UNSIGNED","DUPLICATE",nullptr,"END_STATEMENT","CONST_NULL","CONST_ARRAY",
    "CONST_BOOL","CONST_INT","CONST_FLOAT","CONST_STRING","CONST_OBJECT","FUNCTION","ARRAY_START","THIS","ARGUMENT",
    "VARIABLE","PROPERTY","PROPERTY_REF","PROPERTY_ASSIGN","PROPERTY_DELETE","OBJECT_PROPERTY","INDEX",nullptr,"INDEX_ASSIGN",
    nullptr,"ARRAY_APPEND","JUMP","JUMP_TRUE","JUMP_FALSE","CALL","METHOD_CALL","NEW","RETURN",
    nullptr,nullptr,nullptr,nullptr,nullptr,"END",nullptr,nullptr,nullptr,"ARRAY_LITERAL"
};
}
const QStringList& vsmxRuntimeGlobals() {
    static const QStringList globals{"Actor","Camera","Light","IntervalTimer","OneShotTimer","Date","Math","System"};
    return globals;
}
bool disassembleVsmx(const QByteArray& data,VsmxProgram* out,QString* error) {
    if(error)error->clear();
    const auto fail=[&](const QString& message){if(error)*error=message;return false;};
    if(!out)return fail(QStringLiteral("no output"));
    *out={};
    if(data.size()<0x34 || !data.startsWith("VSMX"))return fail(QStringLiteral("Invalid VSMX header"));
    const auto le=[&](qsizetype p){return qFromLittleEndian<quint32>(data.constData()+p);};
    const auto region=[&](quint32 off,quint32 len){return quint64(off)+len<=quint64(data.size());};
    const quint32 code=le(8),size=le(12);
    if(!region(code,size) || size%8)return fail(QStringLiteral("Invalid VSMX code region"));
    const auto table=[&](int header,bool wide,QStringList& strings)->bool {
        const quint32 off=le(header),len=le(header+4);
        if(!region(off,len) || (wide && len%2))return false;
        if(!wide){for(const auto& s:data.mid(off,len).split('\0'))strings.append(QString::fromLatin1(s));return true;}
        QString s;
        for(quint32 i=0;i<len;i+=2){const ushort c=qFromLittleEndian<quint16>(data.constData()+off+i);if(c)s.append(QChar(c));else {strings.append(s);s.clear();}}
        if(!s.isEmpty())strings.append(s);
        return true;
    };
    if(!table(0x10,true,out->text) || !table(0x1c,true,out->properties) || !table(0x28,false,out->names))
        return fail(QStringLiteral("Invalid VSMX string table region"));
    const auto resolved=[](const QStringList& strings,quint32 index) {
        return index<quint32(strings.size())?QStringLiteral("%1: %2").arg(index).arg(strings[int(index)]):QStringLiteral("%1: <invalid index>").arg(index);
    };
    out->code.reserve(int(size/8));
    for(quint32 i=0;i<size/8;++i) {
        VsmxInstruction x;x.index=i;x.id=le(qsizetype(code)+qsizetype(i)*8);x.value=le(qsizetype(code)+qsizetype(i)*8+4);x.op=int(x.id&0xff);
        const char* name=x.op<int(std::size(kMnemonic))?kMnemonic[x.op]:nullptr;
        x.mnemonic=name?QString::fromLatin1(name):QStringLiteral("UNKNOWN");
        const quint32 v=x.value;const int op=x.op;
        if(!name)x.operands=QStringLiteral("raw=%1").arg(QString::fromLatin1(data.mid(qsizetype(code)+qsizetype(i)*8,8).toHex()));
        else if(op==0x25)x.operands=v?QStringLiteral("true"):QStringLiteral("false");
        else if(op==0x26)x.operands=QString::number(qint32(v));
        else if(op==0x27)x.operands=QString::number(double(std::bit_cast<float>(v)),'g',9);
        else if(op==0x28)x.operands=resolved(out->text,v);
        else if(op==0x2e)x.operands=resolved(out->names,v);
        else if(op>=0x2f && op<=0x33)x.operands=resolved(out->properties,v);
        else if(op==0x2a)x.operands=QStringLiteral("target=%1 args=%2 locals=%3").arg(v).arg((x.id>>8)&0xff).arg(x.id>>24);
        else if(op==0x2d)x.operands=QStringLiteral("index=%1").arg(v);
        else if(op>=0x39 && op<=0x3b)x.operands=QStringLiteral("target=%1").arg(v);
        else if((op>=0x3c && op<=0x3e) || op==0x49)x.operands=QStringLiteral("count=%1").arg(v);
        out->code.append(x);
    }
    return true;
}
VsmxUsage vsmxUsage(const VsmxProgram& program) {
    VsmxUsage u;
    QStringList stack; // symbolic labels: a variable name, ".method" for a property reference, "" for anything else
    const auto pop=[&](int n){for(int i=0;i<n && !stack.isEmpty();++i)stack.removeLast();};
    const auto at=[&](int fromTop){return fromTop<stack.size()?stack[stack.size()-1-fromTop]:QStringLiteral("?");};
    const auto add=[](QStringList& list,const QString& s){if(!list.contains(s))list.append(s);};
    const auto property=[&](quint32 v){return v<quint32(program.properties.size())?program.properties[int(v)]:QStringLiteral("?");};
    // A function literal is FUNCTION, JUMP past the body, body, RETURN: the body is swept with its own stack and the
    // enclosing expression's stack comes back at the jump target (so "new OneShotTimer(t, function(){...})" resolves).
    QMap<quint32,QStringList> resume;int previous=-1;
    for(const auto& x:program.code) {
        const int op=x.op;const quint32 v=x.value;
        if(resume.contains(x.index))stack=resume.take(x.index);
        const int before=previous;previous=op;
        if(op==0x39 && before==0x2a && v>x.index){resume.insert(v,stack);stack.clear();}
        u.opcodes[QStringLiteral("0x%1 %2").arg(op,2,16,QChar('0')).arg(x.mnemonic)]++;
        if(x.mnemonic==QStringLiteral("UNKNOWN"))add(u.unknownOpcodes,QStringLiteral("0x%1").arg(op,2,16,QChar('0')));
        if(op==0x22){stack.clear();continue;}
        if(op==0x2e) {
            const QString name=v<quint32(program.names.size())?program.names[int(v)]:QStringLiteral("?");
            if(vsmxRuntimeGlobals().contains(name))add(u.globals,name);
            stack.append(name);continue;
        }
        if(op==0x2a){++u.functions;stack.append(QStringLiteral("function"));continue;}
        if(op==0x20){stack.append(at(0));continue;}
        if((op>=0x23 && op<=0x29) || op==0x2b || op==0x2c || op==0x2d){stack.append(QString{});continue;}
        if(op==0x2f){add(u.propertiesRead,property(v));pop(1);stack.append(QString{});continue;}
        if(op==0x30){pop(1);stack.append(QStringLiteral(".")+property(v));continue;}
        if(op==0x31){add(u.propertiesWritten,property(v));pop(2);stack.append(QString{});continue;}
        if(op==0x36){pop(3);stack.append(QString{});continue;}
        if(op==0x49){pop(int(v));stack.append(QString{});continue;}
        if(op>=0x3c && op<=0x3e) {
            const QString callee=at(int(v));
            const QString label=callee.isEmpty()?QStringLiteral("?"):callee;
            if(op==0x3e)u.constructors[label]++;
            else if(op==0x3d)u.methods[label.startsWith(QLatin1Char('.'))?label.mid(1):label]++;
            else u.calls[label]++;
            pop(int(v)+1);stack.append(QString{});continue;
        }
        if(op==0x3a || op==0x3b || op==0x3f){pop(1);continue;}
        if(op==0x01 || (op>=0x02 && op<=0x06) || (op>=0x0e && op<=0x15) || (op>=0x19 && op<=0x1b) || (op>=0x1d && op<=0x1f) || op==0x34){pop(2);stack.append(QString{});continue;}
        if((op>=0x07 && op<=0x0d) || op==0x18 || op==0x1c){pop(1);stack.append(QString{});continue;}
    }
    return u;
}
}
