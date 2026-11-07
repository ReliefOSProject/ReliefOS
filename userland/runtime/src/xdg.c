/* SPDX-License-Identifier: Apache-2.0 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <reliefos/xdg.h>
#include <errno.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int xdg_copy(char *out, size_t capacity, const char *value)
{
    size_t length=strlen(value);
    if (length>=capacity) return -ENAMETOOLONG;
    memcpy(out,value,length+1);
    return 0;
}

/* user-dirs.dirs uses a quoted shell-escaped absolute or HOME-relative value.
 * Treat its contents solely as path data, including escaped dollar signs. */
static int xdg_value(const char *value, const char *home, char **result)
{
    while (*value==' ' || *value=='\t') ++value;
    if (*value++!='"') return 0;
    size_t capacity=strlen(value)+strlen(home)+1, used=0;
    char *out=malloc(capacity);
    if (!out) return -ENOMEM;
    if (!strncmp(value,"$HOME",5) && (value[5]=='/' || value[5]=='"')) {
        used=strlen(home); memcpy(out,home,used); value+=5;
    } else if (!strncmp(value,"${HOME}",7) && (value[7]=='/' || value[7]=='"')) {
        used=strlen(home); memcpy(out,home,used); value+=7;
    }
    while (*value && *value!='"' && *value!='\n' && *value!='\r') {
        char c=*value++;
        if (c=='\\') {
            if (!*value || *value=='\n' || *value=='\r') goto invalid;
            if (*value=='\\' || *value=='"' || *value=='$' || *value=='`') c=*value++;
        } else if (c=='$' || c=='`') goto invalid;
        out[used++]=c;
    }
    if (*value++!='"') goto invalid;
    while (*value==' ' || *value=='\t' || *value=='\r') ++value;
    if (*value && *value!='\n' && *value!='#') goto invalid;
    out[used]=0;
    if (!used || out[0]!='/') goto invalid;
    *result=out;
    return 1;
invalid:
    free(out);
    return 0;
}

int reliefos_xdg_user_dir(const char *kind, char *path, size_t capacity)
{
    static const char *const names[]={"DESKTOP","DOWNLOAD","DOCUMENTS","MUSIC",
        "PICTURES","VIDEOS","TEMPLATES","PUBLICSHARE"};
    if (!path || !capacity) return -EINVAL;
    path[0]=0;
    int known=0;
    for (size_t i=0;kind && i<sizeof(names)/sizeof(names[0]);++i)
        known|=!strcmp(kind,names[i]);
    if (!known) return -EINVAL;
    const char *home=getenv("HOME");
    if (!home || home[0]!='/') {
        struct passwd *user=getpwuid(getuid());
        home=user ? user->pw_dir : NULL;
    }
    if (!home || home[0]!='/') return -ENOENT;
    char *home_copy=strdup(home), *config=NULL;
    if (!home_copy) return -ENOMEM;
    const char *config_home=getenv("XDG_CONFIG_HOME");
    int length=config_home && config_home[0]=='/' ?
        asprintf(&config,"%s/user-dirs.dirs",config_home) :
        asprintf(&config,"%s/.config/user-dirs.dirs",home_copy);
    if (length<0) { free(home_copy); return -ENOMEM; }
    int result=0;
    char *chosen=NULL;
    FILE *file=fopen(config,"r");
    free(config);
    if (!file && errno!=ENOENT && errno!=ENOTDIR) result=-errno;
    if (file) {
        char *line=NULL; size_t size=0;
        char key[64]; snprintf(key,sizeof(key),"XDG_%s_DIR",kind);
        size_t key_length=strlen(key);
        while (getline(&line,&size,file)>=0) {
            const char *at=line;
            while (*at==' ' || *at=='\t') ++at;
            if (strncmp(at,key,key_length)) continue;
            at+=key_length;
            while (*at==' ' || *at=='\t') ++at;
            if (*at++!='=') continue;
            char *value=NULL;
            int parsed=xdg_value(at,home_copy,&value);
            if (parsed<0) { result=parsed; break; }
            if (parsed) { free(chosen); chosen=value; }
        }
        if (ferror(file) && !result) result=-(errno ? errno : EIO);
        free(line); fclose(file);
    }
    if (!result) result=xdg_copy(path,capacity,chosen ? chosen : home_copy);
    free(chosen); free(home_copy);
    if (result) path[0]=0;
    return result;
}
