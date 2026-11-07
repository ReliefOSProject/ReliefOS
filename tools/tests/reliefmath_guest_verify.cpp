// Test-only fixture writer and independent persisted-output verifier.
#include "model.h"
#include "document.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
using namespace rm;
static bool write_file(const char *directory,const char *name,const void *data,size_t size){char path[4096];snprintf(path,sizeof path,"%s/%s",directory,name);FILE *file=fopen(path,"wb");if(!file)return false;bool okay=fwrite(data,1,size,file)==size;return fclose(file)==0&&okay;}
static bool read_file(const char *directory,const char *name,Bytes &bytes){char path[4096];snprintf(path,sizeof path,"%s/%s",directory,name);FILE *f=fopen(path,"rb");if(!f)return false;uint8_t chunk[8192];size_t count;while((count=fread(chunk,1,sizeof chunk,f))){if(bytes.size()+count>128u*1024u*1024u||!bytes.append(chunk,count)){fclose(f);return false;}}bool okay=!ferror(f);fclose(f);return okay;}
static const char *names[]={"二次方程求根公式","高斯积分","矩阵与中文","分段函数","多行对齐"};
static const char *sources[]={"x=\\frac{-b\\pm\\sqrt{b^2-4ac}}{2a}","\\int_{-\\infty}^{\\infty}e^{-x^2}\\,dx=\\sqrt{\\pi}","\\text{矩阵}:\\quad A=\\begin{pmatrix}a&b\\\\c&d\\end{pmatrix}","f(x)=\\begin{cases}x^2 & x\\geq 0\\\\-x & x<0\\end{cases}","\\begin{aligned}a^2+b^2&=c^2\\\\E&=mc^2\\end{aligned}"};
int main(int argc,char **argv){
 if(argc!=3)return 2;
 Error e;
 if(!strcmp(argv[1],"prepare")){
  Document doc;doc.title.set("ReliefMath 验收公式");FormulaId id;
  for(size_t n=0;n<5;n++)assert(add_formula(doc,names[n],sources[n],id,e));
  Bytes encoded;assert(encode_document(doc,encoded,e));assert(write_file(argv[2],"acceptance.rmath",encoded.data(),encoded.size()));
  assert(write_file(argv[2],"broken.rmath","rmath bad",9));
  Document custom;assert(add_formula(custom,"30pt display regression","x^2",id,e));custom.formulas[0].style.point_size=30;assert(encode_document(custom,encoded,e));assert(write_file(argv[2],"custom-size.rmath",encoded.data(),encoded.size()));
  Document bad;assert(add_formula(bad,"失败公式","\\unsupported{x}",id,e));assert(add_formula(bad,"有效公式","\\frac{1}{2}",id,e));assert(encode_document(bad,encoded,e));assert(write_file(argv[2],"partial.rmath",encoded.data(),encoded.size()));
  puts("ReliefMath native fixtures prepared");return 0;
 }
 if(!strcmp(argv[1],"verify-style")){
  Bytes bytes;assert(read_file(argv[2],"custom-size-saved.rmath",bytes));Document doc;assert(decode_document(bytes.data(),bytes.size(),doc,e));assert(doc.formulas.size()==1);assert(doc.formulas[0].style.point_size==30);assert(!doc.formulas[0].style.display);assert(doc.formulas[0].source=="x^2");puts("Nonpreset point size retained after display-style change");return 0;
 }
 if(!strcmp(argv[1],"verify-recovery")){
  Bytes bytes;assert(read_file(argv[2],"recovered.rmath",bytes));Document doc;assert(decode_document(bytes.data(),bytes.size(),doc,e));
  assert(doc.formulas.size()==5);assert(doc.formulas[0].source=="\\text{Recovery marker 2468}+x^2");
  for(size_t n=1;n<5;n++){assert(doc.formulas[n].name==names[n]);assert(doc.formulas[n].source==sources[n]);}
  puts("ReliefMath crash recovery persisted into a new document verified");return 0;
 }
 if(!strcmp(argv[1],"verify")){
  Bytes bytes;assert(read_file(argv[2],"saved.rmath",bytes));Document doc;assert(decode_document(bytes.data(),bytes.size(),doc,e));assert(doc.formulas.size()==5);
  for(size_t n=0;n<5;n++){assert(doc.formulas[n].name==names[n]);assert(doc.formulas[n].source==sources[n]);}
  for(size_t n=1;n<=5;n++){
   char name[64];snprintf(name,sizeof name,"%03zu.tex",n);Bytes tex;assert(read_file(argv[2],name,tex));{assert(tex.size()==strlen(sources[n-1])+1);assert(!memcmp(tex.data(),sources[n-1],tex.size()-1));}
  }
  puts("ReliefMath native saved document and source snapshots verified");return 0;
 }
 return 2;
}
