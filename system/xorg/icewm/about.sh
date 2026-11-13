#!/bin/sh
# Session identity, shown inside a terminal window.
case ${LC_ALL:-${LC_MESSAGES:-${LANG:-C}}} in
    ja|ja_*|ja.*|ja@*)
        echo 'ReliefOS Xorg セッション'
        echo 'ウィンドウマネージャー: IceWM'
        prompt='Enter キーで閉じます' ;;
    zh|zh_*|zh.*|zh@*)
        echo 'ReliefOS Xorg 会话'
        echo '窗口管理器: IceWM'
        prompt='按 Enter 键关闭' ;;
    *)
        echo 'ReliefOS Xorg session'
        echo 'Window manager: IceWM'
        prompt='Press ENTER to close' ;;
esac
uname -a
echo
printf '%s' "$prompt"
IFS= read -r answer
