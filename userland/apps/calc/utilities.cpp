#include "model.h"
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace calc {
namespace {
bool separator(char c) { return c==' ' || c=='\t' || c=='\r' || c=='\n' || c==',' || c==';'; }
int compare_double(const void *a, const void *b) {
    double x=*static_cast<const double *>(a), y=*static_cast<const double *>(b);
    return (x>y)-(x<y);
}
bool leap(int year) { return year%4==0 && (year%100!=0 || year%400==0); }
int month_days(int year,int month) {
    static const int lengths[]={31,28,31,30,31,30,31,31,30,31,30,31};
    return lengths[month-1]+(month==2 && leap(year));
}
int serial(Date date) {
    int y=date.year-1, days=365*y+y/4-y/100+y/400;
    for (int m=1;m<date.month;++m) days+=month_days(date.year,m);
    return days+date.day-1;
}
}

Statistics statistics(const char *text) {
    Statistics result;
    if (!text) { result.error=Error::Empty; return result; }
    // 1024 doubles bound sorting and work on the GUI thread, even on paste.
    if (strnlen(text,32769)>32768) { result.error=Error::TooLong; return result; }
    double values[1024], m2=0, compensation=0;
    const char *p=text;
    while (*p) {
        while (separator(*p)) ++p;
        if (!*p) break;
        result.position=size_t(p-text);
        if (result.count==1024) { result.error=Error::TooManyValues; return result; }
        char *end=nullptr; errno=0; double value=strtod(p,&end);
        if (end==p || (*end && !separator(*end))) { result.error=Error::Syntax; return result; }
        if (!isfinite(value) || (value==0 && errno==ERANGE)) { result.error=Error::Overflow; return result; }
        values[result.count++]=value;
        double corrected=value-compensation, sum=result.sum+corrected;
        compensation=(sum-result.sum)-corrected; result.sum=sum;
        double delta=value-result.mean; result.mean+=delta/double(result.count);
        m2+=delta*(value-result.mean);
        if (!isfinite(result.sum) || !isfinite(result.mean) || !isfinite(m2)) {
            result.error=Error::Overflow; return result;
        }
        p=end;
    }
    if (!result.count) { result.error=Error::Empty; return result; }
    qsort(values,result.count,sizeof(double),compare_double);
    result.minimum=values[0]; result.maximum=values[result.count-1];
    size_t mid=result.count/2;
    result.median=result.count%2 ? values[mid] : values[mid-1]/2+values[mid]/2;
    result.population_variance=fmax(0,m2/double(result.count));
    result.population_deviation=sqrt(result.population_variance);
    if (result.count>1) {
        result.sample_variance=fmax(0,m2/double(result.count-1));
        result.sample_deviation=sqrt(result.sample_variance);
    }
    return result;
}
bool valid_date(Date date) {
    return date.year>=1 && date.year<=9999 && date.month>=1 && date.month<=12 &&
        date.day>=1 && date.day<=month_days(date.year,date.month);
}
bool parse_date(const char *text,Date *date) {
    if (!text || !date || strlen(text)!=10 || text[4]!='-' || text[7]!='-') return false;
    for (unsigned i=0;i<10;++i) if (i!=4 && i!=7 && (text[i]<'0' || text[i]>'9')) return false;
    Date parsed{(text[0]-'0')*1000+(text[1]-'0')*100+(text[2]-'0')*10+text[3]-'0',
                (text[5]-'0')*10+text[6]-'0',(text[8]-'0')*10+text[9]-'0'};
    if (!valid_date(parsed)) return false;
    *date=parsed; return true;
}
int weekday(Date date) { return valid_date(date) ? serial(date)%7 : -1; }
bool date_difference(Date first,Date second,int *days,int *weekdays) {
    if (!days || !weekdays || !valid_date(first) || !valid_date(second)) return false;
    int a=serial(first), b=serial(second), sign=1;
    *days=b-a;
    if (b<a) { int temp=a; a=b; b=temp; sign=-1; }
    int length=b-a, working=(length/7)*5;
    for (int n=0;n<length%7;++n) if ((a+n)%7<5) ++working;
    *weekdays=working*sign; return true;
}
bool date_add(Date start,int days,Date *result) {
    if (!result || !valid_date(start)) return false;
    int64_t target=int64_t(serial(start))+days;
    if (target<0 || target>serial({9999,12,31})) return false;
    int low=1,high=9999;
    while (low<high) {
        int mid=(low+high+1)/2;
        if (serial({mid,1,1})<=target) low=mid; else high=mid-1;
    }
    Date date{low,1,1}; int remaining=int(target)-serial(date);
    while (remaining>=month_days(date.year,date.month)) remaining-=month_days(date.year,date.month++);
    date.day=remaining+1; *result=date; return true;
}
void format_date(Date date,char *out,size_t capacity) {
    if (!out || !capacity) return;
    snprintf(out,capacity,"%04d-%02d-%02d",date.year,date.month,date.day);
}
}
