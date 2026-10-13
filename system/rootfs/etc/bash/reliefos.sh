RELIEFOS_BASHRC_LOADED=1
HISTSIZE=${HISTSIZE:-10000}
HISTFILESIZE=${HISTFILESIZE:-20000}
HISTCONTROL=${HISTCONTROL:-ignoreboth}
shopt -s histappend checkwinsize cmdhist
case ${PS1-} in
    '\s-\v\$ '|'\h:\w\$ ') PS1='\u@\h:\w\$ ' ;;
esac
