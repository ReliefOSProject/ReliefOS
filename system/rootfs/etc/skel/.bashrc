case $- in
    *i*) ;;
    *) return ;;
esac
if [ "${RELIEFOS_BASHRC_LOADED:-}" != 1 ] && [ -r /etc/bash/bashrc ]; then
    . /etc/bash/bashrc
fi
