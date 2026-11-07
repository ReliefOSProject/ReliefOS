#ifndef RELIEFOS_CALC_MODEL_H
#define RELIEFOS_CALC_MODEL_H

#include <stddef.h>
#include <stdint.h>

namespace calc {
constexpr size_t ExpressionLimit = 1024;
enum class Error {
    Ok, Empty, Syntax, TooLong, DivideByZero, Domain, Overflow,
    InvalidDigit, ShiftRange, InvalidDate, TooManyValues, NotEnoughValues
};
struct Number {
    double value = 0;
    Error error = Error::Ok;
    size_t position = 0;
    bool ok() const { return error == Error::Ok; }
};
struct Options { bool degrees = true; double answer = 0; double memory = 0; };
Number evaluate(const char *text, Options options = {});
void format_number(double value, char *buffer, size_t capacity);

struct IntegerOptions { unsigned base = 10; unsigned bits = 64; bool signed_value = false; };
struct Integer {
    uint64_t value = 0;
    Error error = Error::Ok;
    size_t position = 0;
    bool wrapped = false;
    bool ok() const { return error == Error::Ok; }
};
Integer evaluate_integer(const char *text, IntegerOptions options = {});
uint64_t word_mask(unsigned bits);
bool format_integer(uint64_t value, unsigned base, unsigned bits, bool signed_value,
                    char *buffer, size_t capacity, bool grouped = false);

struct Unit { const char *id; const char *label; double scale; double offset; };
struct Category {
    const char *id; const char *label; const Unit *units; size_t count;
    bool negative_allowed; bool temperature;
};
const Category *categories(size_t *count);
Number convert(const Category &category, size_t from, size_t to, double value);

struct Statistics {
    size_t count = 0;
    double sum = 0, mean = 0, median = 0, minimum = 0, maximum = 0;
    double population_variance = 0, sample_variance = 0;
    double population_deviation = 0, sample_deviation = 0;
    Error error = Error::Ok;
    size_t position = 0;
    bool ok() const { return error == Error::Ok; }
};
Statistics statistics(const char *text);

struct Date { int year = 0, month = 0, day = 0; };
bool parse_date(const char *text, Date *date);
bool valid_date(Date date);
bool date_difference(Date first, Date second, int *days, int *weekdays);
bool date_add(Date start, int days, Date *result);
int weekday(Date date); // Monday = 0. Invalid dates return -1.
void format_date(Date date, char *buffer, size_t capacity);
}
#endif
