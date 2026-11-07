#include "jobs.h"
#include <assert.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
using namespace rm;
static IoResult wait_for(IoJob &job){IoResult r;for(unsigned n=0;n<10000;n++){if(job.take(r))return r;usleep(1000);}assert(false);return r;}
int main(int argc,char **argv){assert(argc==2);Error e;Document d;FormulaId id=0;assert(add_formula(d,"公式","x^2",id,e));
 char p[4096];snprintf(p,sizeof p,"%s/document.rmath",argv[1]);IoJob job;FileStamp absent;
 assert(job.save(p,d,absent,false,5,e));assert(set_formula_source(d,id,"newer",e));auto saved=wait_for(job);assert(saved.success&&saved.session==5);assert(saved.document.formulas[0].source=="x^2");
 assert(job.load(p,6,e));auto loaded=wait_for(job);assert(loaded.success&&loaded.document.formulas[0].source=="x^2");
 assert(job.save(p,d,absent,false,6,e));auto collision=wait_for(job);assert(!collision.success&&collision.error.code==Code::Conflict);
 assert(job.load("/nonexistent/reliefmath-test",7,e));auto missing=wait_for(job);assert(!missing.success&&missing.document.formulas.empty());
 assert(job.save(p,d,saved.stamp,false,8,e));auto newer=wait_for(job);assert(newer.success&&newer.document.formulas[0].source=="newer");
 assert(job.load(p,9,e));job.cancel();auto cancelled=wait_for(job);assert(!cancelled.success&&cancelled.error.code==Code::Cancelled);
 unlink(p);puts("ReliefMath background I/O snapshot/conflict/cancel tests passed");}
