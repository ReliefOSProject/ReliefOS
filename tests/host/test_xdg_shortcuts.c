#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <reliefos/launch.h>
#include <reliefos/app.h>
#include <reliefos/xdg.h>

int reliefos_app_registry_find(const char *path, struct reliefos_app_info *out)
{ (void)path; (void)out; return -ENOENT; }

int reliefos_spawn_argv(const char *path, char *const arguments[])
{
    assert(!strcmp(path,"/usr/bin/gio"));
    assert(!strcmp(arguments[0],path) && !strcmp(arguments[1],"launch"));
    assert(arguments[2][0]=='/' && !strcmp(arguments[3],"literal ; $() %f"));
    assert(!arguments[4]);
    return 1234;
}

static void put(const char *path, const char *text, mode_t mode)
{
    FILE *file=fopen(path,"w");assert(file);
    assert(fputs(text,file)>=0 && fclose(file)==0 && chmod(path,mode)==0);
}

static void directories(const char *home)
{
    char config[4096], value[4096], expected[4096];
    assert(setenv("HOME",home,1)==0 && unsetenv("XDG_CONFIG_HOME")==0);
    snprintf(config,sizeof(config),"%s/.config",home);assert(mkdir(config,0700)==0);
    strcat(config,"/user-dirs.dirs");
    assert(reliefos_xdg_user_dir("DESKTOP",value,sizeof(value))==0 && !strcmp(value,home));
    put(config,"XDG_DESKTOP_DIR=\"$HOME/桌面 space\"\nXDG_DOWNLOAD_DIR=\"/tmp/absolute download\"\nXDG_DOCUMENTS_DIR=\"$HOME\"\n",0600);
    snprintf(expected,sizeof(expected),"%s/桌面 space",home);
    assert(reliefos_xdg_user_dir("DESKTOP",value,sizeof(value))==0 && !strcmp(value,expected));
    assert(reliefos_xdg_user_dir("DOWNLOAD",value,sizeof(value))==0 && !strcmp(value,"/tmp/absolute download"));
    assert(reliefos_xdg_user_dir("DOCUMENTS",value,sizeof(value))==0 && !strcmp(value,home));
    assert(reliefos_xdg_user_dir("OTHER",value,sizeof(value))==-EINVAL && !*value);
    assert(reliefos_xdg_user_dir("DESKTOP",value,2)==-ENAMETOOLONG && !*value);
    assert(setenv("XDG_CONFIG_HOME","relative-is-ignored",1)==0);
    assert(reliefos_xdg_user_dir("DESKTOP",value,sizeof(value))==0 && !strcmp(value,expected));
    put(config,"XDG_DESKTOP_DIR=\"$HOME/$(touch injected)\"\n",0600);
    assert(reliefos_xdg_user_dir("DESKTOP",value,sizeof(value))==0 && !strcmp(value,home));
    put(config,"XDG_DESKTOP_DIR=\"$HOME/quote \\\" \\\\ \\$literal\"\n",0600);
    snprintf(expected,sizeof(expected),"%s/quote \" \\ $literal",home);
    assert(reliefos_xdg_user_dir("DESKTOP",value,sizeof(value))==0 && !strcmp(value,expected));
    snprintf(config,sizeof(config),"%s/custom config",home);assert(mkdir(config,0700)==0);
    assert(setenv("XDG_CONFIG_HOME",config,1)==0);strcat(config,"/user-dirs.dirs");
    put(config,"XDG_DESKTOP_DIR=\"/tmp/custom\"\n",0600);
    assert(reliefos_xdg_user_dir("DESKTOP",value,sizeof(value))==0 && !strcmp(value,"/tmp/custom"));
    unsetenv("XDG_CONFIG_HOME");
    puts("ok - XDG user directories, localized paths, custom config and literal escaping");
}

int main(int argc, char **argv)
{
    assert(argc==2);
    char target[4096], shortcut[4096], name[256], actual[4096];
    directories(argv[1]);
    assert(snprintf(target,sizeof(target),"%s/中文 notes.txt",argv[1])<(int)sizeof(target));
    int fd=open(target,O_WRONLY|O_CREAT|O_EXCL,0600);
    assert(fd>=0 && write(fd,"content\n",8)==8 && close(fd)==0);
    reliefos_launch_default_shortcut_name(target,name,sizeof(name));
    assert(!strcmp(name,"中文 notes.txt"));
    assert(reliefos_launch_create_shortcut_in_dir(argv[1],target,shortcut,sizeof(shortcut))==0);
    struct stat st;
    assert(lstat(shortcut,&st)==0 && S_ISLNK(st.st_mode));
    ssize_t n=readlink(shortcut,actual,sizeof(actual)-1);
    assert(n>0);actual[n]=0;assert(!strcmp(actual,target));
    puts("ok - standard file shortcut is a POSIX symlink");
    assert(reliefos_launch_create_shortcut(shortcut,target)==-EEXIST);
    snprintf(target,sizeof(target),"%s/目录",argv[1]);assert(mkdir(target,0700)==0);
    assert(reliefos_launch_create_shortcut_in_dir(argv[1],target,shortcut,sizeof(shortcut))==0);
    assert(stat(shortcut,&st)==0 && S_ISDIR(st.st_mode));
    assert(lstat(shortcut,&st)==0 && S_ISLNK(st.st_mode));
    puts("ok - folder shortcut follows the standard symbolic link");
    snprintf(target,sizeof(target),"%s/工具 \"q\" $HOME 100%% `literal` \\name.elf",argv[1]);
    put(target,"#!/bin/sh\nprintf launched > \"$XDG_SHORTCUT_MARKER\"\n",0700);
    assert(reliefos_launch_create_shortcut_in_dir(argv[1],target,shortcut,sizeof(shortcut))==0);
    assert(stat(shortcut,&st)==0 && S_ISREG(st.st_mode) && (st.st_mode&S_IXUSR));
    assert(strstr(shortcut,".desktop"));
    FILE *file=fopen(shortcut,"r");assert(file);
    n=(ssize_t)fread(actual,1,sizeof(actual)-1,file);assert(n>0 && fclose(file)==0);actual[n]=0;
    assert(!strncmp(actual,"[Desktop Entry]\n",16));
    assert(strstr(actual,"Type=Application\n") && strstr(actual,"\nExec=\"") && strstr(actual,"100%%"));
    char *arguments[]={shortcut,"literal ; $() %f",NULL};
    assert(reliefos_launch_desktop_file(arguments)==1234);
    char marker[4096];snprintf(marker,sizeof(marker),"%s/application.desktop",argv[1]);
    put(marker,shortcut,0600); /* The shell harness launches this exact generated file with real GIO. */
    puts("ok - generated Desktop Entry preserves literal executable characters");
    snprintf(target,sizeof(target),"%s/Original.desktop",argv[1]);
    const char *entry="[Desktop Entry]\nType=Application\nName=保留\nIcon=example\nExec=/usr/bin/true %F\nTerminal=false\n";
    put(target,entry,0600);
    assert(reliefos_launch_create_shortcut_in_dir(argv[1],target,shortcut,sizeof(shortcut))==0);
    file=fopen(shortcut,"r");assert(file);n=(ssize_t)fread(actual,1,sizeof(actual)-1,file);actual[n]=0;fclose(file);
    assert(!strcmp(actual,entry));
    assert(reliefos_launch_create_shortcut_in_dir(argv[1],target,name,2)==-ENAMETOOLONG && !*name);
    puts("ok - existing Desktop Entry metadata copied, collisions retained, short output rejected");
    return 0;
}
