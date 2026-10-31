#include "model.h"
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace calc {
namespace {
constexpr double Pi = 3.1415926535897932384626433832795;
bool alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
class Parser {
    const char *start_, *p_;
    Options options_;
    Error error_ = Error::Ok;
    size_t position_ = 0;
    unsigned depth_ = 0;
    void space() { while (*p_ == ' ' || *p_ == '\t' || *p_ == '\r' || *p_ == '\n') ++p_; }
    double fail(Error error) {
        if (error_ == Error::Ok) { error_ = error; position_ = size_t(p_ - start_); }
        return 0;
    }
    double checked(double value) {
        if (isnan(value) || errno == EDOM) return fail(Error::Domain);
        if (value==0 && errno==ERANGE) return fail(Error::Overflow);
        return isfinite(value) ? value : fail(Error::Overflow);
    }
    double function(const char *name, double a, double b, bool binary) {
        const double angle = options_.degrees ? Pi / 180 : 1;
        errno = 0;
        if (binary) {
            if (!strcmp(name, "pow")) {
                double value=pow(a,b);
                return a!=0 && value==0 ? fail(Error::Overflow) : checked(value);
            }
            if (!strcmp(name, "min")) return fmin(a,b);
            if (!strcmp(name, "max")) return fmax(a,b);
            if (!strcmp(name, "hypot")) return checked(hypot(a,b));
            if (!strcmp(name, "mod")) return b ? checked(fmod(a,b)) : fail(Error::DivideByZero);
            if (!strcmp(name, "atan2")) {
                if (!a && !b) return fail(Error::Domain);
                double value=atan2(a,b)/angle;
                return a!=0 && value==0 ? fail(Error::Overflow) : checked(value);
            }
        } else {
            if (!strcmp(name, "sqrt")) return a >= 0 ? checked(sqrt(a)) : fail(Error::Domain);
            if (!strcmp(name, "cbrt")) return checked(cbrt(a));
            if (!strcmp(name, "sin")) {
                double argument=a*angle;
                return a!=0 && argument==0 ? fail(Error::Overflow) : checked(sin(argument));
            }
            if (!strcmp(name, "cos")) return checked(cos(a*angle));
            if (!strcmp(name, "tan")) {
                double argument=a*angle;
                if (a!=0 && argument==0) return fail(Error::Overflow);
                return fabs(cos(argument)) < 1e-15 ? fail(Error::Domain) : checked(tan(argument));
            }
            if (!strcmp(name, "asin")) return checked(asin(a)/angle);
            if (!strcmp(name, "acos")) return checked(acos(a)/angle);
            if (!strcmp(name, "atan")) return checked(atan(a)/angle);
            if (!strcmp(name, "sinh")) return checked(sinh(a));
            if (!strcmp(name, "cosh")) return checked(cosh(a));
            if (!strcmp(name, "tanh")) return checked(tanh(a));
            if (!strcmp(name, "ln")) return a > 0 ? checked(log(a)) : fail(Error::Domain);
            if (!strcmp(name, "log")) return a > 0 ? checked(log10(a)) : fail(Error::Domain);
            if (!strcmp(name, "exp")) {
                double value=exp(a);
                // musl reports floating exceptions rather than setting errno
                // for libm underflow. exp of a finite input is always positive.
                return value==0 ? fail(Error::Overflow) : checked(value);
            }
            if (!strcmp(name, "abs")) return fabs(a);
            if (!strcmp(name, "floor")) return floor(a);
            if (!strcmp(name, "ceil")) return ceil(a);
            if (!strcmp(name, "round")) return round(a);
        }
        return fail(Error::Syntax);
    }
    double primary() {
        space();
        if (*p_ == '(') {
            ++p_; double value = expression(); space();
            if (*p_ != ')') return fail(Error::Syntax);
            ++p_; return value;
        }
        if (alpha(*p_)) {
            char name[24]; size_t n=0;
            while (alpha(*p_) || (*p_ >= '0' && *p_ <= '9')) {
                if (n+1 >= sizeof(name)) return fail(Error::Syntax);
                char c=*p_++; name[n++]=(c >= 'A' && c <= 'Z') ? char(c-'A'+'a') : c;
            }
            name[n]=0; space();
            if (*p_ != '(') {
                if (!strcmp(name,"pi")) return Pi;
                if (!strcmp(name,"tau")) return 2*Pi;
                if (!strcmp(name,"e")) return exp(1.0);
                if (!strcmp(name,"ans")) return options_.answer;
                if (!strcmp(name,"mem")) return options_.memory;
                return fail(Error::Syntax);
            }
            ++p_; double a=expression(), b=0; bool binary=false; space();
            if (*p_ == ',') { ++p_; binary=true; b=expression(); space(); }
            if (*p_ != ')') return fail(Error::Syntax);
            ++p_; return error_ == Error::Ok ? function(name,a,b,binary) : 0;
        }
        if ((*p_ < '0' || *p_ > '9') && *p_ != '.') return fail(Error::Syntax);
        errno=0; char *end=nullptr; double value=strtod(p_,&end);
        if (end == p_) return fail(Error::Syntax);
        p_=end; return checked(value);
    }
    double postfix() {
        double value=primary(); space();
        while (error_ == Error::Ok && (*p_ == '!' || *p_ == '%')) {
            char op=*p_++;
            if (op == '%') {
                double result=value/100;
                if (value!=0 && result==0) return fail(Error::Overflow);
                value=result;
            }
            else {
                if (value < 0 || floor(value) != value) return fail(Error::Domain);
                if (value > 170) return fail(Error::Overflow);
                double result=1; for (unsigned i=2; i <= unsigned(value); ++i) result *= i;
                value=result;
            }
            space();
        }
        return value;
    }
    double power() {
        double value=postfix(); space();
        if (error_ == Error::Ok && *p_ == '^') {
            ++p_; double exponent=unary(); errno=0;
            double result=pow(value,exponent);
            value=value!=0 && result==0 ? fail(Error::Overflow) : checked(result);
        }
        return value;
    }
    double unary() {
        if (++depth_ > 64) { --depth_; return fail(Error::TooLong); }
        space(); double value;
        if (*p_ == '-' || *p_ == '+') { char op=*p_++; value=unary(); if (op == '-') value=-value; }
        else value=power();
        --depth_; return value;
    }
    double term() {
        double value=unary(); space();
        while (error_ == Error::Ok && (*p_ == '*' || *p_ == '/')) {
            char op=*p_++; double rhs=unary();
            if (op == '/' && rhs == 0 && error_ == Error::Ok) return fail(Error::DivideByZero);
            errno=0; double result=op == '*' ? value*rhs : value/rhs;
            if (result==0 && value!=0 && rhs!=0 && error_==Error::Ok) return fail(Error::Overflow);
            value=checked(result); space();
        }
        return value;
    }
    double expression() {
        double value=term(); space();
        while (error_ == Error::Ok && (*p_ == '+' || *p_ == '-')) {
            char op=*p_++; double rhs=term(); errno=0;
            value=checked(op == '+' ? value+rhs : value-rhs); space();
        }
        return value;
    }
public:
    Parser(const char *text, Options options) : start_(text), p_(text), options_(options) {}
    Number run() {
        space(); if (!*p_) return {0,Error::Empty,0};
        double value=expression(); space();
        if (*p_ && error_ == Error::Ok) fail(Error::Syntax);
        if (!isfinite(value) && error_ == Error::Ok) fail(Error::Overflow);
        return {value,error_,position_};
    }
};
}

Number evaluate(const char *text, Options options) {
    if (!text) return {0,Error::Empty,0};
    if (strnlen(text,ExpressionLimit+1) > ExpressionLimit) return {0,Error::TooLong,ExpressionLimit};
    if (!isfinite(options.answer) || !isfinite(options.memory)) return {0,Error::Domain,0};
    return Parser(text,options).run();
}
void format_number(double value, char *out, size_t capacity) {
    if (!out || !capacity) return;
    if (value == 0) value=0; // Avoid displaying a negative zero.
    snprintf(out,capacity,"%.15g",value);
}
}
