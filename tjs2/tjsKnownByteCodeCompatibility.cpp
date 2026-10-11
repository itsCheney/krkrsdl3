#include "tjsKnownByteCodeCompatibility.h"
#include "tjsCommHead.h"
#include "tjsInterCodeGen.h"
#include "../plugins/Kirikiroid2/7zip/Sha256.h"
#include <algorithm>
#include <cstring>
#include <string>

namespace TJS { namespace known_bytecode {
namespace {
struct Reader {
    const uint8_t* bytes;
    size_t length, position=0;
    bool valid=true;
    bool Skip(size_t count) {
        if(!valid || position>length || count>length-position) {valid=false;return false;}
        position+=count;return true;
    }
    uint16_t U16() {
        const auto start=position;if(!Skip(2)) return 0;
        return uint16_t(bytes[start]) | uint16_t(uint16_t(bytes[start+1])<<8);
    }
    uint32_t U32() {
        const auto start=position;if(!Skip(4)) return 0;
        return uint32_t(bytes[start]) | uint32_t(bytes[start+1])<<8 |
            uint32_t(bytes[start+2])<<16 | uint32_t(bytes[start+3])<<24;
    }
    bool Array(size_t stride,bool aligned=false) {
        const size_t count=U32();
        if(!valid || count>length/stride) {valid=false;return false;}
        const size_t size=count*stride;
        return Skip(size) && (!aligned || Skip((4-size%4)%4));
    }
};
struct Context {
    int32_t parent=-1,name=-1,type=-1;
    size_t codeOffset=0,dataOffset=0;
    std::vector<int16_t> code;
    std::vector<std::array<int16_t,2>> data;
};
struct Layout {std::vector<std::string> strings;std::vector<Context> objects;};
bool Parse(const uint8_t* bytes,size_t length,Layout& layout) {
    if(!bytes || length<28 || length>UINT32_MAX) return false;
    Reader r{bytes,length};
    if(r.U32()!=0x32534a54 || r.U32()!=0x00303031 || r.U32()!=length || r.U32()!=0x41544144) return false;
    const size_t dataSize=r.U32();
    if(dataSize<8 || dataSize>length-12) return false;
    const size_t dataEnd=12+dataSize;
    if(!r.Array(1,true) || !r.Array(2,true) || !r.Array(4) || !r.Array(8) || !r.Array(8)) return false;
    const auto strings=r.U32();if(!r.valid || strings>length/4) return false;
    layout.strings.reserve(strings);
    for(uint32_t i=0;i<strings;++i) {
        const size_t count=r.U32();if(!r.valid || count>length/2) return false;
        std::string text;bool ascii=true;text.reserve(std::min<size_t>(count,128));
        for(size_t j=0;j<count;++j) {
            const auto unit=r.U16();if(!r.valid) return false;
            if(unit==0 || unit>127 || j>=128) ascii=false;
            if(ascii) text+=char(unit);
        }
        if(!r.Skip((count&1)*2)) return false;
        layout.strings.push_back(ascii ? text : std::string());
    }
    const auto octets=r.U32();if(!r.valid || octets>length/4) return false;
    for(uint32_t i=0;i<octets;++i) if(!r.Array(1,true)) return false;
    if(r.position!=dataEnd || r.U32()!=0x534a424f) return false;
    const size_t objectSize=r.U32();
    if(objectSize<16 || objectSize!=length-dataEnd) return false;
    const auto top=int32_t(r.U32());const auto count=r.U32();
    if(!r.valid || count>length/64 || top< -1 || (top>=0 && uint32_t(top)>=count)) return false;
    layout.objects.reserve(count);
    for(uint32_t i=0;i<count;++i) {
        if(r.U32()!=0x32534a54) return false;
        const size_t size=r.U32(),start=r.position;
        if(!r.valid || size>length-start || size<68) return false;
        Context c;c.parent=int32_t(r.U32());c.name=int32_t(r.U32());c.type=int32_t(r.U32());
        if(!r.Skip(9*4) || !r.Array(8)) return false;
        const size_t words=r.U32();if(!r.valid || words>length/2) return false;
        c.codeOffset=r.position;c.code.reserve(words);
        for(size_t j=0;j<words;++j) c.code.push_back(int16_t(r.U16()));
        if(!r.Skip((words&1)*2)) return false;
        const size_t constants=r.U32();if(!r.valid || constants>length/4) return false;
        c.dataOffset=r.position;c.data.reserve(constants);
        for(size_t j=0;j<constants;++j) c.data.push_back({int16_t(r.U16()),int16_t(r.U16())});
        if(!r.Array(4) || !r.Array(8) || !r.valid || r.position!=start+size ||
            c.parent< -1 || (c.parent>=0 && uint32_t(c.parent)>=count) || c.name< -1 ||
            (c.name>=0 && size_t(c.name)>=layout.strings.size())) return false;
        layout.objects.push_back(std::move(c));
    }
    return r.valid && r.position==length;
}
// Decode only instruction boundaries and constant operands. This does not run
// the VM, translate addresses, access properties, or retain script objects.
bool Instruction(const std::vector<int16_t>& code,size_t ip,size_t& length,int& datum) {
    const int op=code[ip];datum=-1;
    if(op>=VM_INC && op<=VM_DEC+3) {
        const int form=(op-VM_INC)%4;
        length=form==0?2:form==3?3:4;if(form==1) datum=3;
    } else if(op>=VM_LOR && op<=VM_MUL+3) {
        const int form=(op-VM_LOR)%4;
        length=form==0?3:form==3?4:5;if(form==1) datum=3;
    } else switch(op) {
        case VM_NOP:case VM_NF:case VM_RET:case VM_EXTRY:case VM_REGMEMBER:case VM_DEBUGGER:length=1;break;
        case VM_CONST:length=3;datum=2;break;
        case VM_CP:case VM_CEQ:case VM_CDEQ:case VM_CLT:case VM_CGT:case VM_CHKINS:
        case VM_CCL:case VM_SETP:case VM_GETP:case VM_ENTRY:case VM_CHGTHIS:case VM_ADDCI:length=3;break;
        case VM_TT:case VM_TF:case VM_SETF:case VM_SETNF:case VM_LNOT:case VM_BNOT:
        case VM_ASC:case VM_CHR:case VM_NUM:case VM_CHS:case VM_CL:case VM_INV:case VM_CHKINV:
        case VM_TYPEOF:case VM_EVAL:case VM_EEXP:case VM_INT:case VM_REAL:case VM_STR:case VM_OCTET:
        case VM_JF:case VM_JNF:case VM_JMP:case VM_SRV:case VM_THROW:case VM_GLOBAL:length=2;break;
        case VM_GPD:case VM_GPDS:case VM_DELD:case VM_TYPEOFD:length=4;datum=3;break;
        case VM_SPD:case VM_SPDE:case VM_SPDEH:case VM_SPDS:length=4;datum=2;break;
        case VM_GPI:case VM_GPIS:case VM_SPI:case VM_SPIE:case VM_SPIS:case VM_DELI:case VM_TYPEOFI:length=4;break;
        case VM_CALL:case VM_NEW:case VM_CALLD:case VM_CALLI: {
            const size_t base=(op==VM_CALLD || op==VM_CALLI)?5:4;
            if(base>code.size()-ip) return false;
            if(op==VM_CALLD) datum=3;
            const int arguments=code[ip+base-1];
            if(arguments== -1) length=base;
            else if(arguments== -2) {
                if(base+1>code.size()-ip || code[ip+base]<0) return false;
                length=base+1+size_t(code[ip+base])*2;
            } else {if(arguments<0) return false;length=base+size_t(arguments);}
            break;
        }
        default:return false;
    }
    return length<=code.size()-ip;
}
const char* Basename(const char* name) {
    if(!name) return "";
    const char* base=name;
    for(const char* p=name;*p;++p) if(*p=='/' || *p=='\\' || *p=='>') base=p+1;
    return base;
}
const OptionalMemberRule HSVPickerRule{
    "hsvcpick.tjs",35064,
    {{0x17,0xf0,0x15,0x4e,0xd6,0xf2,0x64,0x0e,0x40,0xd5,0x21,0x9f,0x27,0x57,0xc2,0xab,
      0x51,0xc2,0x7d,0xc0,0x42,0x09,0x9b,0xd3,0xfc,0x4f,0xc1,0xd8,0xe9,0xc6,0x95,0x28}},
    "OptionHSVPickerModule","updateColorSelect",334,27,{{{206,1,-2,16},{218,2,-2,16}}}
};
}
Digest SoftwareSHA256(const uint8_t* bytes,size_t length) noexcept {
    Digest digest{};if(!bytes && length) return digest;
    CSha256 state{};
    // Set the instance directly to software. No Sha256Prepare/CPU probing or
    // process-wide algorithm choice is needed for a content fingerprint.
    if(!Sha256_SetFunction(&state,SHA256_ALGO_SW)) return digest;
    Sha256_InitState(&state);Sha256_Update(&state,bytes,length);Sha256_Final(&state,digest.data());
    return digest;
}
bool TryApplyOptionalMemberRule(const OptionalMemberRule& rule,const char* name,
    const uint8_t* bytes,size_t length,std::vector<uint8_t>& copy) noexcept {
    copy.clear();
    try {
        if(!bytes || !rule.basename || !rule.parentClass || !rule.method || length!=rule.length ||
            std::strcmp(Basename(name),rule.basename) || SoftwareSHA256(bytes,length)!=rule.digest) return false;
        Layout layout;if(!Parse(bytes,length,layout)) return false;
        int direct=-1,button=-1;
        for(size_t i=0;i<layout.strings.size();++i) {
            if(layout.strings[i]=="direct") {if(direct>=0) return false;direct=int(i);}
            if(layout.strings[i]=="hsvDirectSysButton") {if(button>=0) return false;button=int(i);}
        }
        if(direct<0 || button<0 || button>INT16_MAX || rule.reads[0].localConstant!=rule.reads[1].localConstant) return false;
        const Context* target=nullptr;
        for(const auto& c:layout.objects) {
            if(c.name<0 || layout.strings[size_t(c.name)]!=rule.method || c.parent<0) continue;
            const auto& parent=layout.objects[size_t(c.parent)];
            if(parent.name<0 || layout.strings[size_t(parent.name)]!=rule.parentClass) continue;
            if(target || c.type!=ctFunction || parent.type!=ctClass) return false;
            target=&c;
        }
        if(!target || target->code.size()!=rule.codeWords || target->data.size()!=rule.localConstants) return false;
        const auto slot=rule.reads[0].localConstant;
        if(slot>=target->data.size() || target->data[slot][0]!=3 || target->data[slot][1]!=direct) return false;
        size_t oldSlots=0;
        for(const auto& d:target->data) if(d[0]==3 && d[1]==direct) ++oldSlots;
        if(oldSlots!=1) return false;
        size_t references=0;
        for(size_t ip=0;ip<target->code.size();) {
            size_t size=0;int datum=-1;
            if(!Instruction(target->code,ip,size,datum)) return false;
            if(datum>=0 && target->code[ip+size_t(datum)]==int(slot)) {
                if(references>=rule.reads.size()) return false;
                const auto& expected=rule.reads[references++];
                if(ip!=expected.ip || target->code[ip]!=VM_GPD || target->code[ip+1]!=expected.result ||
                    target->code[ip+2]!=expected.receiver || target->code[ip+3]!=int(expected.localConstant)) return false;
            }
            ip+=size;
        }
        if(references!=rule.reads.size()) return false;
        const size_t mapping=target->dataOffset+size_t(slot)*4+2;
        std::vector<uint8_t> patched(bytes,bytes+length);
        patched[mapping]=uint8_t(button);patched[mapping+1]=uint8_t(unsigned(button)>>8);
        copy.swap(patched);return true;
    } catch(...) {copy.clear();return false;}
}
bool TryApplyKnownCompatibility(const char* name,const uint8_t* bytes,size_t length,
    std::vector<uint8_t>& copy) noexcept {
    return TryApplyOptionalMemberRule(HSVPickerRule,name,bytes,length,copy);
}
} }
