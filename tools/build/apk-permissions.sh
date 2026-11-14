#!/bin/sh
# Called under fakeroot by images.sh. Restore numeric owners, special mode
# bits and restricted directories affected by rootless APK extraction.
set -eu
[ "$#" = 1 ] || exit 2
root=$(CDPATH='' cd -- "$1" && pwd -P)
work=$(mktemp -d)
trap 'rm -rf "$work"' 0
trap 'exit 130' HUP INT TERM
for tree in "$root" "$root/install/root" "$root/install/root-console"; do
    database=$tree/lib/apk/db/installed
    [ -f "$database" ] || continue
    [ ! -L "$database" ] || { echo 'symlink APK database' >&2; exit 1; }
    awk '
        function emit(path,acl,isdir, n,a,i) {
            n=split(acl,a,":")
            if (n!=3 || a[1]!~/^[0-9]+$/ || a[2]!~/^[0-9]+$/ || a[3]!~/^[0-7]+$/) exit 1
            if (a[1]==0 && a[2]==0 && a[3]+0<1000 &&
                (!isdir || substr(a[3],length(a[3])-2,1) ~ /^[37]$/)) return
            if (path ~ /^\// || path ~ /[\t\r]/) exit 1
            n=split(path,p,"/")
            for (i=1;i<=n;i++) if (p[i]=="" || p[i]=="." || p[i]=="..") exit 1
            printf "%s\t%s\t%s\t%s\n",a[1],a[2],a[3],path
        }
        function flush() {if (file!="") emit((directory!="" ? directory "/" : "") file,acl,0); file=""}
        /^F:/ {flush(); directory=substr($0,3); next}
        /^M:/ {emit(directory,substr($0,3),1); next}
        /^R:/ {flush(); file=substr($0,3); acl="0:0:644"; next}
        /^a:/ {acl=substr($0,3); next}
        /^$/ {flush(); directory=""}
        END {flush()}
    ' "$database" >"$work/acl"
    while IFS="$(printf '\t')" read -r uid gid mode path; do
        # Resolve parents as well: no archive link may redirect chown/chmod
        # outside this private image tree, even for an otherwise valid path.
        parent=$(CDPATH='' cd -- "$(dirname "$tree/$path")" && pwd -P)
        case $parent in "$root"|"$root"/*) ;; *) echo "escaping APK path: $path" >&2; exit 1;; esac
        test -e "$tree/$path" || test -L "$tree/$path"
        chown -h "$uid:$gid" "$tree/$path"
        if [ ! -L "$tree/$path" ]; then chmod "$mode" "$tree/$path"; fi
    done <"$work/acl"
done
