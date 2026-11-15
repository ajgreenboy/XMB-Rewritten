#include "osk_paste.h"

namespace xmb::oskpaste {
namespace {
bool isBreak(QChar c) { return c==QLatin1Char('\r') || c==QLatin1Char('\n') || c==QChar(0x2028) || c==QChar(0x2029); }
bool isControl(QChar c) { const ushort u=c.unicode();return u<0x20 || (u>=0x7f && u<=0x9f); }
int codePoints(const QString& text) {
    int count=0;
    for(int i=0;i<text.size();++i,++count)if(text[i].isHighSurrogate() && i+1<text.size() && text[i+1].isLowSurrogate())++i;
    return count;
}
}

Result prepare(const Request& request) {
    Result result;
    if(request.raw.size()>kMaxClipboardUnits){result.verdict=Verdict::TooLarge;return result;}
    int begin=0,end=int(request.raw.size());
    while(begin<end && isBreak(request.raw[begin]))++begin;
    while(end>begin && isBreak(request.raw[end-1]))--end;
    QString cleaned;
    cleaned.reserve(end-begin);
    for(int i=begin;i<end;++i) {
        const QChar c=request.raw[i];
        if(isBreak(c)) {
            if(c==QLatin1Char('\r') && i+1<end && request.raw[i+1]==QLatin1Char('\n'))++i;   // CRLF is one break
            cleaned+=request.multiline?QLatin1Char('\n'):QLatin1Char(' ');
        } else if(c==QLatin1Char('\t'))cleaned+=QLatin1Char(' ');
        else if(isControl(c))++result.removed;
        else cleaned+=c;
    }
    if(cleaned.isEmpty()){result.verdict=Verdict::Empty;return result;}
    const int room=request.maxLength-request.currentLength;
    if(room<=0){result.verdict=Verdict::NoRoom;return result;}
    int count=int(cleaned.size());
    result.verdict=Verdict::Inserted;
    if(count>room) {
        count=room;
        if(cleaned[count-1].isHighSurrogate() && cleaned[count].isLowSurrogate())--count;
        result.verdict=Verdict::Truncated;
    }
    result.text=cleaned.left(count);
    result.characters=codePoints(result.text);
    if(result.text.isEmpty())result.verdict=Verdict::NoRoom;
    return result;
}

QString message(const Result& result) {
    switch(result.verdict) {
    case Verdict::Inserted:return QStringLiteral("Pasted %1 character%2").arg(result.characters).arg(result.characters==1?"":"s");
    case Verdict::Truncated:return QStringLiteral("Pasted %1 character%2 (field limit reached)").arg(result.characters).arg(result.characters==1?"":"s");
    case Verdict::Empty:return QStringLiteral("Clipboard is empty");
    case Verdict::NoRoom:return QStringLiteral("The field is full");
    case Verdict::TooLarge:return QStringLiteral("Clipboard text is too large to paste");
    }
    return {};
}
QString messageNoText() { return QStringLiteral("Clipboard has no text"); }

}
