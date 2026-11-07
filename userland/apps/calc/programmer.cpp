#include "model.h"
#include <string.h>

namespace calc {
namespace {
bool valid_bits(unsigned bits) { return bits==8 || bits==16 || bits==32 || bits==64; }
bool valid_base(unsigned base) { return base==2 || base==8 || base==10 || base==16; }
int digit(char c) {
    if (c>='0' && c<='9') return c-'0';
    if (c>='a' && c<='f') return c-'a'+10;
    if (c>='A' && c<='F') return c-'A'+10;
    return -1;
}
char lower(char c) { return c>='A' && c<='Z' ? char(c-'A'+'a') : c; }
bool alnum(char c) { return (lower(c)>='a' && lower(c)<='z') || (c>='0' && c<='9') || c=='_'; }
enum class Op { None, Or, Xor, And, Left, Right, Add, Subtract, Multiply, Divide, Mod };
struct Operation { Op op; int precedence; unsigned length; };

class Parser {
    const char *start_,*p_;
    IntegerOptions options_;
    uint64_t mask_;
    Error error_=Error::Ok;
    size_t position_=0;
    unsigned depth_=0;
    bool wrapped_=false;
    void space() { while (*p_==' ' || *p_=='\t' || *p_=='\r' || *p_=='\n') ++p_; }
    uint64_t fail(Error error) {
        if (error_==Error::Ok) { error_=error; position_=size_t(p_-start_); }
        return 0;
    }
    bool word(const char *text) const {
        size_t n=0;
        while (text[n]) { if (lower(p_[n])!=text[n]) return false; ++n; }
        return !alnum(p_[n]);
    }
    Operation operation() {
        space();
        if (p_[0]=='<' && p_[1]=='<') return {Op::Left,4,2};
        if (p_[0]=='>' && p_[1]=='>') return {Op::Right,4,2};
        if (*p_=='|') return {Op::Or,1,1};
        if (*p_=='^') return {Op::Xor,2,1};
        if (*p_=='&') return {Op::And,3,1};
        if (*p_=='+') return {Op::Add,5,1};
        if (*p_=='-') return {Op::Subtract,5,1};
        if (*p_=='*') return {Op::Multiply,6,1};
        if (*p_=='/') return {Op::Divide,6,1};
        if (*p_=='%') return {Op::Mod,6,1};
        if (word("or")) return {Op::Or,1,2};
        if (word("xor")) return {Op::Xor,2,3};
        if (word("and")) return {Op::And,3,3};
        if (word("mod")) return {Op::Mod,6,3};
        return {Op::None,0,0};
    }
    bool negative(uint64_t value) const {
        return options_.signed_value && (value & (UINT64_C(1)<<(options_.bits-1)));
    }
    uint64_t magnitude(uint64_t value) const { return negative(value) ? ((~value+1)&mask_) : value; }
    uint64_t apply(Op op,uint64_t a,uint64_t b) {
        switch (op) {
        case Op::Or: return a|b;
        case Op::Xor: return a^b;
        case Op::And: return a&b;
        case Op::Add: {
            uint64_t result=(a+b)&mask_,sign=UINT64_C(1)<<(options_.bits-1);
            wrapped_ |= options_.signed_value ? ((~(a^b)&(a^result)&sign)!=0) : a>mask_-b;
            return result;
        }
        case Op::Subtract: {
            uint64_t result=(a-b)&mask_,sign=UINT64_C(1)<<(options_.bits-1);
            wrapped_ |= options_.signed_value ? (((a^b)&(a^result)&sign)!=0) : a<b;
            return result;
        }
        case Op::Multiply: {
            uint64_t x=options_.signed_value ? magnitude(a) : a;
            uint64_t y=options_.signed_value ? magnitude(b) : b;
            uint64_t limit=mask_;
            if (options_.signed_value) limit=(mask_>>1)+(negative(a)!=negative(b));
            wrapped_ |= y && x>limit/y;
            return (a*b)&mask_;
        }
        case Op::Divide:
        case Op::Mod: {
            if (!b) return fail(Error::DivideByZero);
            uint64_t x=magnitude(a), y=magnitude(b);
            uint64_t value=op==Op::Divide ? x/y : x%y;
            bool minus=op==Op::Divide ? negative(a)!=negative(b) : negative(a);
            if (options_.signed_value && op==Op::Divide && !minus && value>(mask_>>1)) wrapped_=true;
            return (minus ? (~value+1) : value)&mask_;
        }
        case Op::Left:
            if (b>=options_.bits) return fail(Error::ShiftRange);
            wrapped_ |= b && a>(mask_>>b);
            return (a<<b)&mask_;
        case Op::Right:
            if (b>=options_.bits) return fail(Error::ShiftRange);
            if (!b) return a;
            return (a>>b) | (negative(a) ? mask_^(mask_>>b) : 0);
        default: return fail(Error::Syntax);
        }
    }
    uint64_t primary() {
        space();
        if (*p_=='(') {
            ++p_; uint64_t value=expression(1); space();
            if (*p_!=')') return fail(Error::Syntax);
            ++p_; return value;
        }
        if (word("rol") || word("ror")) {
            bool left=lower(p_[2])=='l'; p_+=3; space();
            if (*p_!='(') return fail(Error::Syntax);
            ++p_; uint64_t a=expression(1); space();
            if (*p_!=',') return fail(Error::Syntax);
            ++p_; uint64_t b=expression(1); space();
            if (*p_!=')') return fail(Error::Syntax);
            ++p_;
            if (error_!=Error::Ok) return 0;
            if (b>=options_.bits) return fail(Error::ShiftRange);
            if (!b) return a;
            return left ? ((a<<b)|(a>>(options_.bits-b)))&mask_
                        : ((a>>b)|(a<<(options_.bits-b)))&mask_;
        }
        unsigned base=options_.base;
        if (p_[0]=='0') {
            char prefix=lower(p_[1]);
            if (prefix=='x' || prefix=='b' || prefix=='o' || prefix=='d') {
                base=prefix=='x' ? 16 : prefix=='b' ? 2 : prefix=='o' ? 8 : 10;
                p_+=2;
            }
        }
        uint64_t value=0; bool any=false;
        for (;;) {
            int d=digit(*p_);
            if (d<0 || unsigned(d)>=base) break;
            if (value>(mask_-unsigned(d))/base) return fail(Error::Overflow);
            value=value*base+unsigned(d); ++p_; any=true;
        }
        if (!any) return fail(Error::InvalidDigit);
        if (*p_=='.' || (*p_>='0' && *p_<='9')) return fail(Error::InvalidDigit);
        return value;
    }
    uint64_t unary() {
        if (++depth_>64) { --depth_; return fail(Error::TooLong); }
        space(); uint64_t value;
        if (*p_=='+' || *p_=='-' || *p_=='~') {
            char op=*p_++; value=unary();
            if (op=='-') value=(~value+1)&mask_;
            if (op=='~') value=(~value)&mask_;
        } else if (word("not")) { p_+=3; value=(~unary())&mask_; }
        else value=primary();
        --depth_; return value;
    }
    uint64_t expression(int minimum) {
        // Precedence recursion is bounded by the six operator levels. Nested
        // parentheses and unary chains share the separately bounded depth.
        uint64_t value=unary();
        while (error_==Error::Ok) {
            Operation op=operation();
            if (op.op==Op::None || op.precedence<minimum) break;
            p_+=op.length; uint64_t rhs=expression(op.precedence+1);
            if (error_==Error::Ok) value=apply(op.op,value,rhs);
        }
        return value;
    }
public:
    Parser(const char *text,IntegerOptions options) : start_(text),p_(text),options_(options),mask_(word_mask(options.bits)) {}
    Integer run() {
        space(); if (!*p_) return {0,Error::Empty,0,false};
        uint64_t value=expression(1); space();
        if (*p_ && error_==Error::Ok) fail(Error::Syntax);
        return {value,error_,position_,wrapped_};
    }
};
}
uint64_t word_mask(unsigned bits) {
    return !valid_bits(bits) ? 0 : bits==64 ? UINT64_MAX : (UINT64_C(1)<<bits)-1;
}
Integer evaluate_integer(const char *text,IntegerOptions options) {
    if (!text) return {0,Error::Empty,0,false};
    if (!valid_base(options.base) || !valid_bits(options.bits)) return {0,Error::Domain,0,false};
    if (strnlen(text,ExpressionLimit+1)>ExpressionLimit) return {0,Error::TooLong,ExpressionLimit,false};
    return Parser(text,options).run();
}
bool format_integer(uint64_t value,unsigned base,unsigned bits,bool signed_value,
                    char *buffer,size_t capacity,bool grouped) {
    if (!buffer || !capacity) return false;
    buffer[0]=0;
    if (!valid_base(base) || !valid_bits(bits)) return false;
    value &= word_mask(bits);
    bool negative=base==10 && signed_value && (value & (UINT64_C(1)<<(bits-1)));
    if (negative) value=(~value+1)&word_mask(bits);
    char reverse[96]; size_t n=0; unsigned digits=0;
    do {
        if (grouped && base==2 && digits && digits%4==0) reverse[n++]=' ';
        reverse[n++]="0123456789ABCDEF"[value%base]; value/=base; ++digits;
    } while (value || (grouped && base==2 && digits<bits));
    if (negative) reverse[n++]='-';
    if (n+1>capacity) return false;
    for (size_t i=0;i<n;++i) buffer[i]=reverse[n-i-1];
    buffer[n]=0; return true;
}
}
