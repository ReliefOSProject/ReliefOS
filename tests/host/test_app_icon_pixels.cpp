#include "app_icon_pixels.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc,char **argv) {
    assert(argc==3);
    unsigned char pixels[32*32*4];
    memset(pixels,123,sizeof(pixels));
    assert(reliefos_fltk::read_app_icon(argv[1],pixels));
    assert(pixels[3]==0);
    size_t at=(16*32+10)*4;
    assert(pixels[at]==246 && pixels[at+1]==196 && pixels[at+2]==65 && pixels[at+3]==255);
    FILE *source=fopen(argv[1],"rb"); assert(source);
    unsigned char bytes[54+32*32*4]; assert(fread(bytes,1,sizeof(bytes),source)==sizeof(bytes)); fclose(source);
    for (int failure=0;failure<5;++failure) {
        unsigned char bad[sizeof(bytes)]; memcpy(bad,bytes,sizeof(bad));
        size_t length=sizeof(bad);
        if (failure==0) bad[0]='X';
        if (failure==1) bad[18]=255;
        if (failure==2) bad[28]=24;
        if (failure==3) bad[30]=1;
        if (failure==4) length-=1;
        FILE *out=fopen(argv[2],"wb"); assert(out); assert(fwrite(bad,1,length,out)==length); fclose(out);
        memset(pixels,123,sizeof(pixels));
        assert(!reliefos_fltk::read_app_icon(argv[2],pixels));
        assert(pixels[0]==123);
    }
    // Bottom-up BMPs carry the same pixels after reversing their row order.
    bytes[22]=32; bytes[23]=bytes[24]=bytes[25]=0;
    unsigned char flipped[sizeof(bytes)]; memcpy(flipped,bytes,54);
    for(int row=0;row<32;++row) memcpy(flipped+54+row*128,bytes+54+(31-row)*128,128);
    FILE *out=fopen(argv[2],"wb"); assert(out); assert(fwrite(flipped,1,sizeof(flipped),out)==sizeof(flipped)); fclose(out);
    assert(reliefos_fltk::read_app_icon(argv[2],pixels));
    assert(pixels[at]==246 && pixels[at+1]==196 && pixels[at+2]==65 && pixels[at+3]==255);
    puts("app icon pixels: alpha, channels, row order and malformed files passed");
}
