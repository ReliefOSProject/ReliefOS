#include "worker/renderer.h"
#include "protocol.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static void test_svg_limit(rm::Renderer &renderer) {
    char source[65537];
    const char *opening="\\begin{matrix}",*closing="\\end{matrix}";
    size_t length=strlen(opening);
    memcpy(source,opening,length);
    for(unsigned row=0;row<16;row++) {
        for(unsigned column=0;column<16;column++) {
            memset(source+length,'M',240);length+=240;
            if(column<15)source[length++]='&';
        }
        if(row<15){source[length++]='\\';source[length++]='\\';}
    }
    memcpy(source+length,closing,strlen(closing));length+=strlen(closing);
    assert(length==61736);
    rm::Snapshot input;rm::Rendered out;rm::Error error;
    input.artifacts=rm::WantSvg;input.scale=0.001;
    assert(input.formula.source.set(source,length));
    const bool okay=renderer.render(input,out,error);
    if(okay)fprintf(stderr,"SVG limit regression: render succeeded with %zu bytes (limit %llu)\n",
                    out.svg.size(),static_cast<unsigned long long>(rm::kMaxSvgBytes));
    assert(!okay);
    assert(error.code==rm::Code::Limit);
    assert(out.svg.size()==0&&out.png.size()==0&&out.raster.rgba.size()==0);
    assert(input.formula.source.set("x"));
    assert(renderer.render(input,out,error));
    assert(error.code==rm::Code::None&&out.svg.size()>0&&out.svg.size()<=rm::kMaxSvgBytes);
}

static void test_stream_limits(const char *root,bool png) {
    char math[4096],text[4096];
    snprintf(math,sizeof math,"%s/STIXTwoMath-Regular.otf",root);
    snprintf(text,sizeof text,"%s/simsun.ttc",root);
    rm::FontBook fonts;rm::Error error;rm::Box box;
    assert(fonts.open(math,text,error));
    assert(rm::parse_latex("x",1,32,false,fonts,box,error));
    rm::Snapshot input;input.artifacts=png?rm::WantPng:rm::WantSvg;
    rm::Rendered sample;
    assert(rm::output_scene(fonts,box,input,sample,error));
    const size_t encoded_size=png?sample.png.size():sample.svg.size();
    const size_t limit=static_cast<size_t>(png?rm::kMaxPngBytes:rm::kMaxSvgBytes);
    assert(encoded_size>20&&encoded_size<limit);
    for(unsigned excess=0;excess<2;excess++) {
        rm::Rendered out;
        rm::Bytes &bytes=png?out.png:out.svg;
        // Prefilling isolates the cumulative writer boundary without needing
        // an incompressible 16 Mpx formula for the 64 MiB PNG limit.
        assert(bytes.resize(limit-encoded_size+excess));
        error.clear();
        const bool okay=rm::output_scene(fonts,box,input,out,error);
        if(excess==0)assert(okay&&bytes.size()==limit);
        else {
            if(okay)fprintf(stderr,"%s stream exceeded limit: %zu > %zu\n",png?"PNG":"SVG",bytes.size(),limit);
            assert(!okay);
            assert(error.code==rm::Code::Limit);
            assert(bytes.size()<=limit);
        }
    }
    if(!png) {
        rm::Rendered out;
        assert(out.svg.resize(limit-1));
        error.clear();
        assert(!rm::output_scene(fonts,box,input,out,error));
        assert(error.code==rm::Code::Limit&&out.svg.size()==limit-1);
    }
}

static void test_render_sizes(rm::Renderer &renderer) {
    struct Settings {uint16_t points;uint32_t dpi;double scale;const char *source;};
    const Settings settings[]={
        {512,96,1,"x"},{512,192,1,"x"},{512,24,4,"x"},
        {24,2400,1,"\\sqrt[3]{x^2}+\\hat{y}"},{1,2400,64,"x"},
        {512,2400,1,"."},{512,2400,64,""}
    };
    for(const Settings &setting:settings) {
        rm::Snapshot input;rm::Rendered out;rm::Error error;
        input.formula.style.point_size=setting.points;input.formula.style.padding=0;
        input.dpi=setting.dpi;input.scale=setting.scale;input.artifacts=7;
        assert(input.formula.source.set(setting.source));
        const bool okay=renderer.render(input,out,error);
        if(!okay)fprintf(stderr,"valid size rejected: %upt %uDPI %.8gx: %s\n",
                         setting.points,setting.dpi,setting.scale,error.message.c_str());
        assert(okay);
        assert(rm::valid_raster_extent(out.raster.width,out.raster.height));
        assert(out.svg.size()>20&&out.png.size()>20&&isfinite(out.baseline));
    }
    rm::Snapshot huge;rm::Rendered out;rm::Error error;
    huge.formula.style.point_size=512;huge.formula.style.padding=0;
    huge.dpi=2400;huge.scale=64;huge.artifacts=7;
    assert(huge.formula.source.set("x"));
    assert(!renderer.render(huge,out,error));
    assert(error.code==rm::Code::Limit);
    assert(strstr(error.message.c_str(),"dimensions or pixel limit"));
    assert(out.svg.size()==0&&out.png.size()==0&&out.raster.rgba.size()==0);
}

int main(int argc,char **argv) {
    assert(argc==3||argc==4);
    rm::Renderer r; rm::Error e;
    assert(r.initialize(argv[1],e));
    if(argc==4) {
        if(!strcmp(argv[3],"svg-limit"))test_svg_limit(r);
        else if(!strcmp(argv[3],"svg-stream"))test_stream_limits(argv[1],false);
        else if(!strcmp(argv[3],"png-stream"))test_stream_limits(argv[1],true);
        else if(!strcmp(argv[3],"sizes"))test_render_sizes(r);
        else assert(false);
        puts("ReliefMath backend regression passed");
        return 0;
    }
    const char *cases[]={"x^2+\\frac{1}{y}","\\sqrt[3]{a+b}","\\int_0^1 x\\,dx",
        "\\begin{pmatrix}a&b\\\\c&d\\end{pmatrix}","\\text{中文公式}+\\alpha"};
    for(unsigned i=0;i<5;i++) {
        rm::Snapshot s; rm::Rendered out;
        s.request_id=i+1; s.formula.id=i+1; s.artifacts=7;
        assert(s.formula.source.set(cases[i]));
        if(!r.render(s,out,e)) { fprintf(stderr,"render failed: case %u: %s [%zu,%zu]\n",i,e.message.c_str(),e.begin,e.end); return 1; }
        assert(out.svg.size()>20 && out.png.size()>20 && out.raster.width>0);
        char path[4096]; snprintf(path,sizeof path,"%s/probe-%u.svg",argv[2],i);
        FILE *f=fopen(path,"wb"); assert(f);
        assert(fwrite(out.svg.data(),1,out.svg.size(),f)==out.svg.size()); assert(!fclose(f));
        snprintf(path,sizeof path,"%s/probe-%u.png",argv[2],i);
        f=fopen(path,"wb"); assert(f);
        assert(fwrite(out.png.data(),1,out.png.size(),f)==out.png.size()); assert(!fclose(f));
        bool transparent=false,ink=false;
        for(size_t k=3;k<out.raster.rgba.size();k+=4) {
            unsigned a=out.raster.rgba.data()[k]; transparent|=a==0; ink|=a>0;
        }
        assert(transparent && ink);
    }
    rm::Snapshot bad; rm::Rendered result; assert(bad.formula.source.set("\\input{secret}"));
    assert(!r.render(bad,result,e));
    test_svg_limit(r);
    test_stream_limits(argv[1],false);
    test_stream_limits(argv[1],true);
    test_render_sizes(r);
    puts("ReliefMath backend probe passed: math, matrix, Chinese outlines, SVG, transparent PNG, rejected input");
}
