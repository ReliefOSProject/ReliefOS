/* SPDX-License-Identifier: Apache-2.0 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <reliefos/launch.h>
#include <reliefos/layout.h>
#include <reliefos/app.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

static int desktop_suffix(const char *path)
{
    size_t n=strlen(path);
    return n>=8 && !strcasecmp(path+n-8,".desktop");
}

static const char *desktop_basename(const char *path)
{
    const char *base=strrchr(path,'/');
    return base ? base+1 : path;
}

static int desktop_application(const char *path, const struct stat *st)
{
    return S_ISREG(st->st_mode) && (desktop_suffix(path) || (st->st_mode&0111));
}

void reliefos_launch_default_shortcut_name(const char *target, char *out, uint32_t capacity)
{
    if (!out || !capacity) return;
    out[0]=0;
    if (!target || !*target) return;
    struct stat st;
    if (stat(target,&st)<0) return;
    const char *base=desktop_basename(target);
    size_t n=strlen(base);
    int app=desktop_application(target,&st);
    if (app && desktop_suffix(base)) n-=8;
    else if (app && n>=4 && !strcasecmp(base+n-4,".elf")) n-=4;
    const char *suffix=app ? ".desktop" : "";
    if (!n || n+strlen(suffix)>=capacity) { errno=ENAMETOOLONG; return; }
    memcpy(out,base,n);strcpy(out+n,suffix);
}

/* Desktop Entry string escaping is separate from Exec argument escaping. */
static void desktop_string(FILE *out, const char *text)
{
    for (;*text;++text) {
        switch (*text) {
        case '\\': fputs("\\\\",out); break;
        case '\n': fputs("\\n",out); break;
        case '\r': fputs("\\r",out); break;
        case '\t': fputs("\\t",out); break;
        case ' ': fputs("\\s",out); break;
        default: fputc((unsigned char)*text,out); break;
        }
    }
}

static void desktop_exec(FILE *out, const char *path)
{
    fputc('"',out);
    for (;*path;++path) {
        switch (*path) {
        case '%': fputs("%%",out); break;
        case '\\': fputs("\\\\\\\\",out); break;
        case '"': case '$': case '`': fputs("\\\\",out); fputc(*path,out); break;
        case '\n': fputs("\\n",out); break;
        case '\r': fputs("\\r",out); break;
        case '\t': fputs("\\t",out); break;
        default: fputc((unsigned char)*path,out); break;
        }
    }
    fputc('"',out);
}

/* Only reuse an installed entry for its exact packaged executable. A user's
 * unrelated program with the same basename must keep its own Exec target. */
static FILE *desktop_template_in(const char *directory, size_t length,
                                 const char *id, size_t id_length)
{
    if (!length || directory[0]!='/') return NULL;
    char *path=NULL;
    if (asprintf(&path,"%.*s/applications/reliefos-%.*s.desktop",(int)length,directory,
                 (int)id_length,id)<0) return NULL;
    FILE *file=fopen(path,"r");free(path);return file;
}

static FILE *desktop_template(const char *target)
{
    const char prefix[]=RELIEFOS_LAYOUT_RELIEFOS_APPS "/";
    if (strncmp(target,prefix,sizeof(prefix)-1)) return NULL;
    const char *id=target+sizeof(prefix)-1, *slash=strchr(id,'/');
    if (!slash || slash==id) return NULL;
    size_t n=(size_t)(slash-id);
    if (strlen(slash+1)!=n+4 || strncmp(id,slash+1,n) || strcmp(slash+1+n,".elf")) return NULL;
    const char *data=getenv("XDG_DATA_HOME"), *home=getenv("HOME");
    FILE *file=NULL;
    if (data && data[0]=='/') file=desktop_template_in(data,strlen(data),id,n);
    else if (home && home[0]=='/') {
        char *local=NULL;
        if (asprintf(&local,"%s/.local/share",home)>=0) {
            file=desktop_template_in(local,strlen(local),id,n);free(local);
        }
    }
    if (file) return file;
    data=getenv("XDG_DATA_DIRS");
    if (!data || !*data) data="/usr/local/share:/usr/share";
    for (;;) {
        const char *end=strchr(data,':');size_t length=end ? (size_t)(end-data) : strlen(data);
        file=desktop_template_in(data,length,id,n);
        if (file || !end) return file;
        data=end+1;
    }
}

static int desktop_write_application(const char *destination, const char *target)
{
    FILE *source=desktop_suffix(target) ? fopen(target,"r") : desktop_template(target);
    if (desktop_suffix(target) && !source) return -errno;
    int fd=open(destination,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0700);
    if (fd<0) { int e=errno;if(source)fclose(source);return -e; }
    FILE *out=fdopen(fd,"w");
    if (!out) { int e=errno;close(fd);unlink(destination);if(source)fclose(source);return -e; }
    int error=0;
    if (source) {
        char data[8192];size_t count;
        while ((count=fread(data,1,sizeof(data),source))>0)
            if (fwrite(data,1,count,out)!=count) { error=errno ? errno : EIO; break; }
        if (ferror(source) && !error) error=errno ? errno : EIO;
        fclose(source);
    } else {
        const char *base=desktop_basename(target);
        struct reliefos_app_info app;
        int terminal=reliefos_app_registry_find(target,&app)==0 && (app.flags&RELIEFOS_APP_FLAG_TERMINAL);
        fputs("[Desktop Entry]\nType=Application\nName=",out);desktop_string(out,base);
        /* Keep the executable token constant. GIO checks that token before
         * expanding %% field codes; a literal percent in the program path
         * would otherwise fail its availability check. The shell receives
         * only constant code, and forwards the target as a positional argv. */
        fputs("\nExec=",out);desktop_exec(out,"/bin/sh");
        fputc(' ',out);desktop_exec(out,"-c");
        fputc(' ',out);desktop_exec(out,"exec \"$@\"");
        fputc(' ',out);desktop_exec(out,"desktop-launch");
        fputc(' ',out);desktop_exec(out,target);fputs(" %F",out);
        fprintf(out,"\nIcon=application-x-executable\nTerminal=%s\n",terminal ? "true" : "false");
    }
    if ((fflush(out)<0 || ferror(out)) && !error) error=errno ? errno : EIO;
    if (!error && fsync(fd)<0) error=errno;
    if (fclose(out)<0 && !error) error=errno;
    if (error) unlink(destination);
    return -error;
}

int reliefos_launch_create_shortcut(const char *destination, const char *target)
{
    if (!destination || !*destination || !target || !*target) return -EINVAL;
    char *absolute=realpath(target,NULL);
    if (!absolute) return -errno;
    struct stat st;
    int result=stat(absolute,&st)<0 ? -errno : 0;
    if (!result && desktop_application(absolute,&st)) {
        if (!desktop_suffix(destination)) result=-EINVAL;
        else result=desktop_write_application(destination,absolute);
    } else if (!result) result=symlink(absolute,destination)<0 ? -errno : 0;
    free(absolute);
    return result;
}

int reliefos_launch_create_shortcut_in_dir(const char *directory, const char *target,
                                          char *out, uint32_t capacity)
{
    if (out && capacity) out[0]=0;
    if (!directory || !*directory || !target || !*target || (out && !capacity)) return -EINVAL;
    char name[NAME_MAX+1], path[PATH_MAX];
    reliefos_launch_default_shortcut_name(target,name,sizeof(name));
    if (!*name) return -(errno ? errno : ENAMETOOLONG);
    size_t length=strlen(name), stem=length;
    if (desktop_suffix(name)) stem-=8;
    for (unsigned number=1;number<UINT_MAX;++number) {
        int n=number==1 ? snprintf(path,sizeof(path),"%s/%s",directory,name) :
            snprintf(path,sizeof(path),"%s/%.*s (%u)%s",directory,(int)stem,name,number,name+stem);
        if (n<0 || (size_t)n>=sizeof(path) || (out && (size_t)n>=capacity)) return -ENAMETOOLONG;
        int result=reliefos_launch_create_shortcut(path,target);
        if (result==-EEXIST) continue;
        if (!result && out) memcpy(out,path,(size_t)n+1);
        return result;
    }
    return -EEXIST;
}

int reliefos_launch_desktop_file(char *const arguments[])
{
    if (!arguments || !arguments[0] || !*arguments[0]) return -EINVAL;
    char *absolute=realpath(arguments[0],NULL);
    if (!absolute) return -errno;
    char *args[RELIEFOS_LAUNCH_MAX_ARGS+3]={"/usr/bin/gio","launch",absolute};
    unsigned i=1;
    for (;arguments[i];++i) {
        if (i>=RELIEFOS_LAUNCH_MAX_ARGS) { free(absolute);return -E2BIG; }
        args[i+2]=arguments[i];
    }
    args[i+2]=NULL;
    int result=reliefos_spawn_argv(args[0],args);
    free(absolute);
    return result;
}

extern __typeof__(reliefos_launch_create_shortcut) leonos_launch_create_shortcut __attribute__((alias("reliefos_launch_create_shortcut")));
extern __typeof__(reliefos_launch_create_shortcut_in_dir) leonos_launch_create_shortcut_in_dir __attribute__((alias("reliefos_launch_create_shortcut_in_dir")));
extern __typeof__(reliefos_launch_default_shortcut_name) leonos_launch_default_shortcut_name __attribute__((alias("reliefos_launch_default_shortcut_name")));
