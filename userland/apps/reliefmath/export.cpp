#include "export.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
namespace rm {
bool export_filename(const char *directory,const char *pattern,const char *name,size_t index,ExportFormat format,Text &out,Error &error){
    if(!directory||!*directory||!pattern||!*pattern||!name){error.set(Code::Format,"Choose an export directory and filename pattern");return false;}
    char safe[512];size_t at=0;
    for(size_t n=0;name[n]&&at<sizeof(safe)-1;n++){
        unsigned char ch=static_cast<unsigned char>(name[n]);
        if(ch=='/'||ch=='\\'||ch=='.'||ch==':'||ch<32||ch==127){if(!at||safe[at-1]!='_')safe[at++]='_';}
        else safe[at++]=char(ch);
    }
    safe[at]=0;if(!at)memcpy(safe,"formula",8);
    Bytes filename;
    for(size_t n=0;pattern[n];){
        const char *part=nullptr;size_t count=0;char number[32];
        if(!strncmp(pattern+n,"{name}",6)){part=safe;count=strlen(safe);n+=6;}
        else if(!strncmp(pattern+n,"{index}",7)){snprintf(number,sizeof number,"%03zu",index);part=number;count=strlen(number);n+=7;}
        else {unsigned char c=static_cast<unsigned char>(pattern[n]);if(c=='/'||c=='\\'||c=='{'||c=='}'||c<32||c==127){error.set(Code::Format,"Use {index} and {name} without path separators");return false;}part=pattern+n++;count=1;}
        if(filename.size()+count>240||!filename.append(part,count)){error.set(Code::Limit,"Export filename is too long");return false;}
    }
    if(!filename.size()||!filename.append("",1)){error.set(Code::NoMemory,"Cannot create export filename");return false;}
    const char *base=reinterpret_cast<const char*>(filename.data());
    if(base[0]=='.'||strstr(base,"..")){error.set(Code::Format,"Export filenames cannot contain parent path components");return false;}
    const char *ext=format==ExportFormat::Svg?"svg":format==ExportFormat::Png?"png":"tex";
    char path[4096];int length=snprintf(path,sizeof path,"%s%s%s.%s",directory,directory[strlen(directory)-1]=='/'?"":"/",base,ext);
    if(length<0||length>=int(sizeof path)||!out.set(path)){error.set(Code::Limit,"Export path is too long");return false;}error.clear();return true;
}
bool ExportQueue::start(const char *worker,const Document &doc,const FormulaId *selected,size_t count,uint64_t session,const ExportOptions &options,Error &error){
    if(active_){error.set(Code::Conflict,"An export is already running");return false;}
    if(!isfinite(options.scale)||options.scale<.05||options.scale>16||options.dpi<36||options.dpi>1200){error.set(Code::Limit,"Export scale or DPI is outside the supported range");return false;}
    Array<ExportItem> pending;
    for(size_t n=0;n<doc.formulas.size();n++){
        bool include=count==0;for(size_t s=0;s<count;s++)if(selected[s]==doc.formulas[n].id)include=true;
        if(!include)continue;
        ExportItem item;item.snapshot.formula=doc.formulas[n];item.snapshot.session_id=session;
        if(next_request_==UINT64_MAX){error.set(Code::Limit,"Export request identifiers exhausted");return false;}
        item.snapshot.request_id=++next_request_;item.snapshot.scale=options.scale;item.snapshot.dpi=options.dpi;
        item.snapshot.artifacts=options.format==ExportFormat::Svg?WantSvg:WantPng;
        if(!export_filename(options.directory.c_str(),options.pattern.c_str(),doc.formulas[n].name.c_str(),pending.size()+1,options.format,item.path,error))return false;
        for(size_t j=0;j<pending.size();j++)if(pending[j].path==item.path){error.set(Code::Conflict,"The filename pattern creates duplicate output paths; include {index}");return false;}
        if(!pending.push(static_cast<ExportItem&&>(item))){error.set(Code::NoMemory,"Cannot prepare export snapshots");return false;}
    }
    if(pending.empty()){error.set(Code::NotFound,"No formulas selected for export");return false;}
    if(options.format!=ExportFormat::Latex&&!renderer_.start(worker,error))return false;
    items_=static_cast<Array<ExportItem>&&>(pending);options_=options;cursor_=0;cancelled_=false;active_=true;error.clear();return true;
}
void ExportQueue::write(Bytes &&bytes){
    ExportItem &item=items_[cursor_];
    if(writer_.write(item.path.c_str(),static_cast<Bytes&&>(bytes),options_.overwrite,item.error))item.status=ExportStatus::Writing;
    else {item.status=ExportStatus::Failed;next();}
}
void ExportQueue::next(){++cursor_;if(cursor_>=items_.size())active_=false;}
void ExportQueue::pump(){
    renderer_.pump();
    if(!active_)return;
    ExportItem &item=items_[cursor_];
    if(item.status==ExportStatus::Writing){
        IoResult result;if(!writer_.take(result))return;
        item.error=result.error;item.status=result.success?ExportStatus::Done:(result.error.code==Code::Cancelled?ExportStatus::Cancelled:ExportStatus::Failed);next();return;
    }
    if(cancelled_){item.status=ExportStatus::Cancelled;next();return;}
    if(item.status==ExportStatus::Pending){
        if(options_.format==ExportFormat::Latex){Bytes bytes;const Text &s=item.snapshot.formula.source;if(!bytes.append(s.c_str(),s.size())||!bytes.append("\n",1)){item.error.set(Code::NoMemory,"Cannot allocate LaTeX output");item.status=ExportStatus::Failed;next();}else write(static_cast<Bytes&&>(bytes));}
        else if(renderer_.submit(item.snapshot,item.error))item.status=ExportStatus::Rendering;
        else {item.status=ExportStatus::Failed;next();}
        return;
    }
    Rendered rendered;Error error;
    if(renderer_.take(rendered,error)){
        if(error.code!=Code::None){item.error=error;item.status=ExportStatus::Failed;next();return;}
        if(!same_snapshot(rendered.snapshot,item.snapshot)||rendered.request_id!=item.snapshot.request_id){item.error.set(Code::Protocol,"Export result does not match its source snapshot");item.status=ExportStatus::Failed;next();return;}
        write(options_.format==ExportFormat::Svg?static_cast<Bytes&&>(rendered.svg):static_cast<Bytes&&>(rendered.png));
    }
}
void ExportQueue::cancel(){
    if(!active_)return;
    cancelled_=true;renderer_.cancel();writer_.cancel();
    for(size_t n=cursor_;n<items_.size();n++)if(items_[n].status!=ExportStatus::Writing)items_[n].status=ExportStatus::Cancelled;
    if(!writer_.pending()){cursor_=items_.size();active_=false;}
}
}
