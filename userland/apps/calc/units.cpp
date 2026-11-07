#include "model.h"
#include <math.h>
#define N_(text) text

namespace calc {
namespace {
// SI and international customary definitions: NIST SP 811 Appendix B.
// Binary prefixes: https://physics.nist.gov/cuu/Units/binary.html
// Customary volumes are explicitly US liquid or UK imperial, never ambiguous.
constexpr double Pi=3.1415926535897932384626433832795;
const Unit length[]={
    {"m",N_("Metre (m)"),1,0}, {"km",N_("Kilometre (km)"),1000,0},
    {"cm",N_("Centimetre (cm)"),.01,0}, {"mm",N_("Millimetre (mm)"),.001,0},
    {"um",N_("Micrometre (um)"),1e-6,0}, {"nm",N_("Nanometre (nm)"),1e-9,0},
    {"dm",N_("Decimetre (dm)"),.1,0}, {"in",N_("Inch (in)"),.0254,0},
    {"ft",N_("Foot (ft)"),.3048,0}, {"yd",N_("Yard (yd)"),.9144,0},
    {"mi",N_("Mile (mi)"),1609.344,0}, {"nmi",N_("Nautical mile (nmi)"),1852,0}
};
const Unit area[]={
    {"m2",N_("Square metre (m2)"),1,0}, {"km2",N_("Square kilometre (km2)"),1e6,0},
    {"cm2",N_("Square centimetre (cm2)"),1e-4,0}, {"mm2",N_("Square millimetre (mm2)"),1e-6,0},
    {"ha",N_("Hectare (ha)"),10000,0}, {"acre",N_("Acre"),4046.8564224,0},
    {"ft2",N_("Square foot (ft2)"),.09290304,0}, {"in2",N_("Square inch (in2)"),.00064516,0},
    {"yd2",N_("Square yard (yd2)"),.83612736,0}
};
const Unit volume[]={
    {"L",N_("Litre (L)"),.001,0}, {"mL",N_("Millilitre (mL)"),1e-6,0},
    {"m3",N_("Cubic metre (m3)"),1,0}, {"cm3",N_("Cubic centimetre (cm3)"),1e-6,0},
    {"ft3",N_("Cubic foot (ft3)"),.028316846592,0}, {"in3",N_("Cubic inch (in3)"),.000016387064,0},
    {"gal-us",N_("US liquid gallon"),.003785411784,0}, {"gal-uk",N_("UK imperial gallon"),.00454609,0},
    {"qt-us",N_("US liquid quart"),.000946352946,0}, {"pt-us",N_("US liquid pint"),.000473176473,0},
    {"floz-us",N_("US fluid ounce"),.0000295735295625,0},
    {"tbsp-us",N_("US tablespoon"),.00001478676478125,0},
    {"tsp-us",N_("US teaspoon"),.00000492892159375,0},
    {"cup-us",N_("US customary cup"),.0002365882365,0}
};
const Unit mass[]={
    {"kg",N_("Kilogram (kg)"),1,0}, {"g",N_("Gram (g)"),.001,0},
    {"mg",N_("Milligram (mg)"),1e-6,0}, {"ug",N_("Microgram (ug)"),1e-9,0},
    {"t",N_("Tonne (t)"),1000,0}, {"lb",N_("Pound (lb)"),.45359237,0},
    {"oz",N_("Ounce (oz)"),.028349523125,0}, {"st",N_("Stone (st)"),6.35029318,0},
    {"ton-us",N_("US short ton"),907.18474,0}, {"ton-uk",N_("UK long ton"),1016.0469088,0},
    {"ct",N_("Metric carat (ct)"),.0002,0}, {"gr",N_("Grain (gr)"),.00006479891,0}
};
const Unit temperature[]={
    {"C",N_("Celsius (C)"),1,273.15}, {"F",N_("Fahrenheit (F)"),5.0/9,273.15-32*5.0/9},
    {"K",N_("Kelvin (K)"),1,0}, {"R",N_("Rankine (R)"),5.0/9,0}
};
const Unit time_units[]={
    {"s",N_("Second (s)"),1,0}, {"ms",N_("Millisecond (ms)"),.001,0},
    {"us",N_("Microsecond (us)"),1e-6,0}, {"ns",N_("Nanosecond (ns)"),1e-9,0},
    {"min",N_("Minute (min)"),60,0}, {"h",N_("Hour (h)"),3600,0},
    {"day",N_("Day (24 h)"),86400,0}, {"week",N_("Week (7 d)"),604800,0},
    {"year-j",N_("Julian year (365.25 d)"),31557600,0}
};
const Unit speed[]={
    {"ms",N_("Metres per second (m/s)"),1,0}, {"kmh",N_("Kilometres per hour (km/h)"),1.0/3.6,0},
    {"mph",N_("Miles per hour (mph)"),.44704,0}, {"fts",N_("Feet per second (ft/s)"),.3048,0},
    {"knot",N_("Knot (kn)"),1852.0/3600,0}, {"cms",N_("Centimetres per second (cm/s)"),.01,0}
};
const Unit acceleration[]={
    {"ms2",N_("Metres per second squared (m/s2)"),1,0},
    {"cms2",N_("Centimetres per second squared (cm/s2)"),.01,0},
    {"fts2",N_("Feet per second squared (ft/s2)"),.3048,0},
    {"g0",N_("Standard gravity (g0)"),9.80665,0}
};
const Unit angle[]={
    {"rad",N_("Radian (rad)"),1,0}, {"deg",N_("Degree (deg)"),Pi/180,0},
    {"grad",N_("Gradian (gon)"),Pi/200,0}, {"turn",N_("Turn (rev)"),2*Pi,0},
    {"arcmin",N_("Arcminute"),Pi/10800,0}, {"arcsec",N_("Arcsecond"),Pi/648000,0}
};
const Unit pressure[]={
    {"Pa",N_("Pascal (Pa)"),1,0}, {"kPa",N_("Kilopascal (kPa)"),1000,0},
    {"MPa",N_("Megapascal (MPa)"),1e6,0}, {"bar",N_("Bar"),1e5,0},
    {"mbar",N_("Millibar (mbar)"),100,0}, {"atm",N_("Standard atmosphere (atm)"),101325,0},
    {"Torr",N_("Torr"),101325.0/760,0}, {"psi",N_("Pounds per square inch (psi)"),.45359237*9.80665/.00064516,0}
};
const Unit energy[]={
    {"J",N_("Joule (J)"),1,0}, {"kJ",N_("Kilojoule (kJ)"),1000,0},
    {"MJ",N_("Megajoule (MJ)"),1e6,0}, {"Wh",N_("Watt hour (Wh)"),3600,0},
    {"kWh",N_("Kilowatt hour (kWh)"),3600000,0},
    {"cal",N_("Thermochemical calorie (cal)"),4.184,0},
    {"kcal",N_("Thermochemical kilocalorie (kcal)"),4184,0},
    {"eV",N_("Electronvolt (eV)"),1.602176634e-19,0},
    {"Btu",N_("British thermal unit (IT)"),1055.05585262,0},
    {"ft-lbf",N_("Foot pound-force (ft lbf)"),.3048*.45359237*9.80665,0}
};
const Unit power[]={
    {"W",N_("Watt (W)"),1,0}, {"kW",N_("Kilowatt (kW)"),1000,0},
    {"MW",N_("Megawatt (MW)"),1e6,0},
    {"hp",N_("Mechanical horsepower (hp)"),550*.3048*.45359237*9.80665,0},
    {"PS",N_("Metric horsepower (PS)"),75*9.80665,0},
    {"Btuh",N_("BTU (IT) per hour"),1055.05585262/3600,0}
};
const Unit force[]={
    {"N",N_("Newton (N)"),1,0}, {"kN",N_("Kilonewton (kN)"),1000,0},
    {"dyn",N_("Dyne (dyn)"),1e-5,0}, {"lbf",N_("Pound-force (lbf)"),.45359237*9.80665,0},
    {"kgf",N_("Kilogram-force (kgf)"),9.80665,0}
};
const Unit frequency[]={
    {"Hz",N_("Hertz (Hz)"),1,0}, {"kHz",N_("Kilohertz (kHz)"),1000,0},
    {"MHz",N_("Megahertz (MHz)"),1e6,0}, {"GHz",N_("Gigahertz (GHz)"),1e9,0},
    {"rpm",N_("Revolutions per minute (rpm)"),1.0/60,0}
};
const Unit data[]={
    {"B",N_("Byte (B)"),1,0}, {"bit",N_("Bit (bit)"),.125,0},
    {"kB",N_("Kilobyte (kB, 1000 B)"),1e3,0}, {"MB",N_("Megabyte (MB, 1000 kB)"),1e6,0},
    {"GB",N_("Gigabyte (GB, 1000 MB)"),1e9,0}, {"TB",N_("Terabyte (TB, 1000 GB)"),1e12,0},
    {"PB",N_("Petabyte (PB, 1000 TB)"),1e15,0},
    {"KiB",N_("Kibibyte (KiB, 1024 B)"),1024,0}, {"MiB",N_("Mebibyte (MiB, 1024 KiB)"),1048576,0},
    {"GiB",N_("Gibibyte (GiB, 1024 MiB)"),1073741824,0},
    {"TiB",N_("Tebibyte (TiB, 1024 GiB)"),1099511627776.0,0},
    {"PiB",N_("Pebibyte (PiB, 1024 TiB)"),1125899906842624.0,0},
    {"Kibit",N_("Kibibit (Kibit)"),128,0}, {"Mibit",N_("Mebibit (Mibit)"),131072,0},
    {"Gibit",N_("Gibibit (Gibit)"),134217728,0}
};
const Unit data_rate[]={
    {"bps",N_("Bits per second (bit/s)"),.125,0},
    {"kbps",N_("Kilobits per second (kbit/s)"),125,0},
    {"Mbps",N_("Megabits per second (Mbit/s)"),125000,0},
    {"Gbps",N_("Gigabits per second (Gbit/s)"),125000000,0},
    {"Bps",N_("Bytes per second (B/s)"),1,0},
    {"kBps",N_("Kilobytes per second (kB/s)"),1000,0},
    {"MBps",N_("Megabytes per second (MB/s)"),1e6,0},
    {"GBps",N_("Gigabytes per second (GB/s)"),1e9,0},
    {"KiBps",N_("Kibibytes per second (KiB/s)"),1024,0},
    {"MiBps",N_("Mebibytes per second (MiB/s)"),1048576,0},
    {"GiBps",N_("Gibibytes per second (GiB/s)"),1073741824,0}
};
const Unit density[]={
    {"kgm3",N_("Kilograms per cubic metre (kg/m3)"),1,0},
    {"gcm3",N_("Grams per cubic centimetre (g/cm3)"),1000,0},
    {"gL",N_("Grams per litre (g/L)"),1,0},
    {"lbft3",N_("Pounds per cubic foot (lb/ft3)"),.45359237/.028316846592,0}
};
const Unit torque[]={
    {"Nm",N_("Newton metre (N m)"),1,0}, {"Ncm",N_("Newton centimetre (N cm)"),.01,0},
    {"kNm",N_("Kilonewton metre (kN m)"),1000,0},
    {"lbfft",N_("Pound-force foot (lbf ft)"),.45359237*9.80665*.3048,0},
    {"lbfin",N_("Pound-force inch (lbf in)"),.45359237*9.80665*.0254,0}
};
#define CATEGORY(id,label,array,negative,temp) {id,label,array,sizeof(array)/sizeof(array[0]),negative,temp}
const Category catalog[]={
    CATEGORY("length",N_("Length"),length,true,false),
    CATEGORY("area",N_("Area"),area,false,false),
    CATEGORY("volume",N_("Volume (capacity)"),volume,false,false),
    CATEGORY("mass",N_("Mass"),mass,false,false),
    CATEGORY("temperature",N_("Temperature"),temperature,true,true),
    CATEGORY("time",N_("Time"),time_units,true,false),
    CATEGORY("speed",N_("Speed"),speed,true,false),
    CATEGORY("acceleration",N_("Acceleration"),acceleration,true,false),
    CATEGORY("angle",N_("Angle"),angle,true,false),
    CATEGORY("pressure",N_("Pressure"),pressure,true,false),
    CATEGORY("energy",N_("Energy"),energy,true,false),
    CATEGORY("power",N_("Power units"),power,true,false),
    CATEGORY("force",N_("Force units"),force,true,false),
    CATEGORY("frequency",N_("Frequency"),frequency,false,false),
    CATEGORY("data",N_("Data storage"),data,false,false),
    CATEGORY("data-rate",N_("Data transfer rate"),data_rate,false,false),
    CATEGORY("density",N_("Density"),density,false,false),
    CATEGORY("torque",N_("Torque"),torque,true,false)
};
}
const Category *categories(size_t *count) {
    if (count) *count=sizeof(catalog)/sizeof(catalog[0]);
    return catalog;
}
Number convert(const Category &category,size_t from,size_t to,double value) {
    if (from>=category.count || to>=category.count || !category.units) return {0,Error::Domain,0};
    if (!isfinite(value)) return {0,Error::Overflow,0};
    if (!category.negative_allowed && value<0) return {0,Error::Domain,0};
    const Unit &a=category.units[from], &b=category.units[to];
    if (!isfinite(a.scale) || !isfinite(b.scale) || a.scale<=0 || b.scale<=0) return {0,Error::Domain,0};
    // Extended intermediates preserve representable final results when a
    // temporary value in the base unit would exceed the double exponent range.
    long double base=static_cast<long double>(value)*a.scale+a.offset;
    if (category.temperature) {
        if (base < -1e-10) return {0,Error::Domain,0};
        if (base < 0) base=0;
    }
    if (!isfinite(base)) return {0,Error::Overflow,0};
    if (from==to) return {value,Error::Ok,0};
    long double precise=(base-b.offset)/b.scale;
    double result=static_cast<double>(precise);
    if (!isfinite(result) || (precise!=0 && result==0)) return {0,Error::Overflow,0};
    return {result,Error::Ok,0};
}
}
