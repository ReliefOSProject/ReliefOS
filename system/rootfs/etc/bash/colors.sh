# Defaults for interactive Bash, including existing user accounts.  Auto mode
# checks each command's stdout, so pipelines and redirected files stay plain.
case $- in
    *i*) ;;
    *) return ;;
esac
case ${TERM-} in
    ''|dumb) return ;;
esac
if [[ -n ${NO_COLOR-} ]]; then
    return
fi

# Keep explicit user settings, including an intentionally empty value.
if [[ ! ${LS_COLORS+x} ]] && command -v dircolors >/dev/null 2>&1; then
    eval "$(dircolors --sh)"
fi
if [[ ! ${LESS+x} ]]; then
    export LESS=-R
fi

# These three entries are provided by GNU coreutils.  BusyBox grep accepts
# --color but ignores it; do not mistake accepting an option for color support.
for reliefos_color_command in ls dir vdir; do
    if [[ $(type -t "$reliefos_color_command") == file ]]; then
        alias "$reliefos_color_command=$reliefos_color_command --color=auto"
    fi
done
unset reliefos_color_command
