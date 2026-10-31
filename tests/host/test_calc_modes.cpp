#include "model.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

static unsigned checks, failures;
#define CHECK(expression) do { ++checks; if (!(expression)) { ++failures; \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expression); } } while (0)
static bool near(double actual, double expected) {
    return isfinite(actual) && fabs(actual - expected) <= 1e-10 * fmax(1, fabs(expected));
}
static void real(const char *text, double expected, calc::Options options = {}) {
    auto value = calc::evaluate(text, options);
    CHECK(value.ok()); CHECK(near(value.value, expected));
}
static void real_error(const char *text, calc::Error expected) {
    auto result=calc::evaluate(text);
    if (result.error!=expected) fprintf(stderr,"Expression %s: error=%d expected=%d value=%.17g\n",text,int(result.error),int(expected),result.value);
    CHECK(result.error == expected);
}
static void integer(const char *text, uint64_t expected, calc::IntegerOptions options = {}) {
    auto value = calc::evaluate_integer(text, options);
    CHECK(value.ok()); CHECK(value.value == expected);
}
static void conversion(const char *category, const char *from, const char *to,
                       double input, double expected) {
    size_t count = 0; auto *all = calc::categories(&count);
    const calc::Category *found = nullptr;
    for (size_t i = 0; i < count; ++i) if (!strcmp(category, all[i].id)) found = &all[i];
    CHECK(found); if (!found) return;
    size_t a = found->count, b = found->count;
    for (size_t i = 0; i < found->count; ++i) {
        if (!strcmp(from, found->units[i].id)) a = i;
        if (!strcmp(to, found->units[i].id)) b = i;
    }
    CHECK(a < found->count && b < found->count);
    auto result = calc::convert(*found, a, b, input);
    CHECK(result.ok()); CHECK(near(result.value, expected));
}
int main() {
    real("1.5+2.25", 3.75); real("7/2", 3.5); real("2+3*4", 14);
    real("(2+3)*4", 20); real("2^3^2", 512); real("-2^2", -4);
    real("2^-3", .125); real("200*10%", 20); real("5!", 120);
    real("sqrt(81)+cbrt(-8)", 7); real("sin(30)+cos(60)", 1);
    real("asin(0.5)", 30); real("atan2(1,1)", 45);
    real("sin(pi/2)", 1, {false, 0, 0});
    real("ln(e)+log(100)+exp(0)", 4);
    real("pow(2,5)+mod(17,5)", 34); real("abs(-3)+floor(2.9)+ceil(1.1)", 7);
    real("min(2,3)+max(5,7)+hypot(3,4)", 14);
    real("ans+mem", 42, {true, 40, 2}); real("1e-3*1E3", 1);
    real_error("1/0", calc::Error::DivideByZero);
    real_error("sqrt(-1)", calc::Error::Domain);
    real_error("ln(0)", calc::Error::Domain);
    real_error("171!", calc::Error::Overflow);
    real_error("1e309", calc::Error::Overflow);
    real_error("1e-999", calc::Error::Overflow);
    real_error("exp(-10000)", calc::Error::Overflow);
    real_error("pow(1e-200,2)", calc::Error::Overflow);
    real_error("1e-200^2", calc::Error::Overflow);
    real_error("1e-323%", calc::Error::Overflow);
    real_error("sin(1e-323)", calc::Error::Overflow);
    real_error("tan(1e-323)", calc::Error::Overflow);
    real_error("atan2(1e-310,1e308)", calc::Error::Overflow);
    real("sin(0)+tan(0)",0);
    real_error("1e-200*1e-200", calc::Error::Overflow);
    CHECK(calc::evaluate("1e-310").ok() && calc::evaluate("1e-310").value>0);
    real_error("2+", calc::Error::Syntax);
    real_error("2 3", calc::Error::Syntax);
    real_error("nan", calc::Error::Syntax);
    real_error("sin(1,2)", calc::Error::Syntax);
    char excessive[1200]; memset(excessive, '1', sizeof(excessive)); excessive[1199] = 0;
    CHECK(calc::evaluate(excessive).error == calc::Error::TooLong);
    char deep[220]; memset(deep, '(', 100); deep[100] = '1'; memset(deep+101, ')', 100); deep[201] = 0;
    CHECK(!calc::evaluate(deep).ok());
    char formatted[128]; calc::format_number(-0.0, formatted, sizeof(formatted));
    CHECK(!strcmp(formatted, "0"));

    integer("18446744073709551615", UINT64_MAX);
    integer("0xFF+0b1010", 265); integer("FF AND 0F", 15, {16, 64, false});
    integer("0o77+0d1", 64); integer("(1 << 63) | 1", UINT64_C(0x8000000000000001));
    integer("~0", UINT64_MAX); integer("0xAA XOR 0xFF", 0x55);
    integer("NOT 0", 255, {10, 8, false}); integer("255+1", 0, {10, 8, false});
    CHECK(calc::evaluate_integer("255+1", {10,8,false}).wrapped);
    CHECK(calc::evaluate_integer("127+1", {10,8,true}).wrapped);
    CHECK(calc::evaluate_integer("50*3", {10,8,true}).wrapped);
    CHECK(calc::evaluate_integer("-128-1", {10,8,true}).wrapped);
    CHECK(!calc::evaluate_integer("-1+1", {10,8,true}).wrapped);
    integer("7/2", 3); integer("17%5", 2); integer("-7/3", 254, {10,8,true});
    integer("-128 >> 1", 192, {10,8,true});
    integer("-128 >> 1", 64, {10,8,false});
    integer("rol(129,1)", 3, {10,8,false}); integer("ror(1,1)", 128, {10,8,false});
    integer("-9223372036854775808/-1", UINT64_C(0x8000000000000000), {10,64,true});
    CHECK(calc::evaluate_integer("1 << 64").error == calc::Error::ShiftRange);
    CHECK(calc::evaluate_integer("10/0").error == calc::Error::DivideByZero);
    CHECK(calc::evaluate_integer("256", {10,8,false}).error == calc::Error::Overflow);
    CHECK(calc::evaluate_integer("18446744073709551616").error == calc::Error::Overflow);
    CHECK(!calc::evaluate_integer("102", {2,8,false}).ok());
    CHECK(!calc::evaluate_integer("0x").ok()); CHECK(!calc::evaluate_integer("1.5").ok());
    CHECK(!calc::evaluate_integer("1", {3,64,false}).ok());
    CHECK(!calc::evaluate_integer(deep).ok());
    CHECK(calc::format_integer(UINT64_MAX,10,64,false,formatted,sizeof(formatted)));
    CHECK(!strcmp(formatted,"18446744073709551615"));
    CHECK(calc::format_integer(255,10,8,true,formatted,sizeof(formatted)));
    CHECK(!strcmp(formatted,"-1"));
    CHECK(calc::format_integer(255,16,8,true,formatted,sizeof(formatted)));
    CHECK(!strcmp(formatted,"FF"));
    CHECK(calc::format_integer(255,2,8,false,formatted,sizeof(formatted),true));
    CHECK(!strcmp(formatted,"1111 1111"));
    CHECK(!calc::format_integer(255,2,8,false,formatted,3));
    CHECK(calc::word_mask(64) == UINT64_MAX && calc::word_mask(8) == 255);

    conversion("length","in","cm",1,2.54); conversion("length","mi","km",1,1.609344);
    conversion("mass","lb","kg",1,.45359237);
    conversion("area","acre","m2",1,4046.8564224);
    conversion("volume","gal-us","L",1,3.785411784);
    conversion("volume","gal-uk","L",1,4.54609);
    conversion("temperature","C","F",0,32); conversion("temperature","F","C",212,100);
    conversion("temperature","C","K",-273.15,0);
    conversion("speed","kmh","ms",36,10); conversion("time","day","h",2,48);
    conversion("pressure","atm","Pa",1,101325);
    conversion("energy","kWh","J",1,3600000);
    conversion("angle","deg","rad",180,acos(-1));
    conversion("data","MiB","B",1,1048576); conversion("data","MB","B",1,1000000);
    conversion("data-rate","Mbps","MBps",8,1);
    conversion("length","km","mi",1e308,1e308/1.609344);
    size_t count = 0, unit_count = 0; auto *all = calc::categories(&count);
    CHECK(count >= 16);
    for (size_t c = 0; c < count; ++c) {
        if (!strcmp(all[c].id,"energy")) {
            for (size_t u=0;u<all[c].count;++u) if (!strcmp(all[c].units[u].id,"eV")) {
                auto tiny=calc::convert(all[c],u,u,1e-310);
                CHECK(tiny.ok() && tiny.value==1e-310);
            }
        }
        unit_count += all[c].count;
        for (size_t u = 0; u < all[c].count; ++u) {
            auto same = calc::convert(all[c], u, u, 12.5);
            CHECK(same.ok() && near(same.value,12.5));
            auto forward = calc::convert(all[c], u, 0, 12.5);
            auto back = calc::convert(all[c], 0, u, forward.value);
            CHECK(forward.ok() && back.ok() && near(back.value,12.5));
            for (size_t v = u+1; v < all[c].count; ++v)
                CHECK(strcmp(all[c].units[u].id,all[c].units[v].id));
        }
        CHECK(!calc::convert(all[c],all[c].count,0,1).ok());
        CHECK(!calc::convert(all[c],0,0,INFINITY).ok());
        if (all[c].temperature) CHECK(calc::convert(all[c],0,0,-300).error == calc::Error::Domain);
    }
    CHECK(unit_count >= 100);

    auto stats = calc::statistics("1, 2;3\n4 5");
    CHECK(stats.ok() && stats.count == 5);
    CHECK(near(stats.sum,15) && near(stats.mean,3) && near(stats.median,3));
    CHECK(near(stats.minimum,1) && near(stats.maximum,5));
    CHECK(near(stats.population_variance,2) && near(stats.sample_variance,2.5));
    stats = calc::statistics("1000000000001 1000000000002 1000000000003");
    CHECK(stats.ok() && near(stats.sample_variance,1));
    stats = calc::statistics("8 2 4 6"); CHECK(stats.ok() && near(stats.median,5));
    CHECK(calc::statistics("1").ok());
    CHECK(calc::statistics("").error == calc::Error::Empty);
    CHECK(!calc::statistics("1 2bad").ok()); CHECK(!calc::statistics("nan").ok());
    CHECK(!calc::statistics("1e309").ok());
    CHECK(calc::statistics("1e-999").error == calc::Error::Overflow);

    calc::Date first{}, second{}, result{}; int days=0, weekdays=0;
    CHECK(calc::parse_date("2024-02-29", &first));
    CHECK(!calc::parse_date("2023-02-29", &result));
    CHECK(!calc::parse_date("1900-02-29", &result));
    CHECK(calc::parse_date("2000-02-29", &result));
    CHECK(!calc::parse_date("2024-01-01 extra", &result));
    CHECK(!calc::parse_date("0000-01-01", &result));
    CHECK(calc::parse_date("2024-03-01", &second));
    CHECK(calc::date_difference(first,second,&days,&weekdays) && days==1 && weekdays==1);
    CHECK(calc::date_add(first,1,&result) && result.month==3 && result.day==1);
    CHECK(calc::date_add(first,-59,&result) && result.month==1 && result.day==1);
    CHECK(calc::parse_date("2026-10-23",&first) && calc::parse_date("2026-10-26",&second));
    CHECK(calc::date_difference(first,second,&days,&weekdays) && days==3 && weekdays==1);
    CHECK(calc::date_difference(second,first,&days,&weekdays) && days==-3 && weekdays==-1);
    CHECK(calc::weekday({1970,1,1}) == 3);
    CHECK(!calc::date_add({9999,12,31},1,&result));
    CHECK(!calc::date_add({1,1,1},-1,&result));
    CHECK(calc::date_add({2000,1,1},366,&result) && result.year==2001 && result.month==1 && result.day==1);
    calc::format_date({2024,2,9}, formatted, sizeof(formatted)); CHECK(!strcmp(formatted,"2024-02-09"));
    printf("Calculator: %u checks, %u failures (%zu categories, %zu units)\n",checks,failures,count,unit_count);
    return failures ? 1 : 0;
}
