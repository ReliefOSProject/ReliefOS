RELIEFOS_BASHRC_LOADED=1
HISTSIZE=${HISTSIZE:-10000}
HISTFILESIZE=${HISTFILESIZE:-20000}
HISTCONTROL=${HISTCONTROL:-ignoreboth}
shopt -s histappend checkwinsize cmdhist
case ${PS1-} in
    '\s-\v\$ '|'\h:\w\$ ') PS1='\u@\h:\w\$ ' ;;
esac
if [[ $- == *i* && -t 1 ]] && command -v fastfetch >/dev/null 2>&1; then
    fastfetch || :
fi
