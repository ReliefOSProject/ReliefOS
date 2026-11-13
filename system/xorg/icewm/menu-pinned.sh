#!/bin/sh
# Emit IceWM menu entries in the session's message language.
case ${LC_ALL:-${LC_MESSAGES:-${LANG:-C}}} in
    ja|ja_*|ja.*|ja@*)
        terminal=端末 fileman=ファイルマネージャー calculator=電卓
        information=システム情報 editor=テキストエディター browser=Webブラウザー
        about=バージョン情報 logout=ログアウト ;;
    zh|zh_*|zh.*|zh@*)
        terminal=终端 fileman=文件管理器 calculator=计算器
        information=系统信息 editor=文本编辑器 browser=网页浏览器
        about=关于 logout=注销 ;;
    *)
        terminal=Terminal fileman='File Manager' calculator=Calculator
        information='System information' editor='Text editor' browser='Web browser'
        about=About logout='Log out' ;;
esac
printf 'prog "%s" xterm sh /etc/reliefos/icewm/terminal.sh\n' "$terminal"
printf 'prog "%s" reliefos-fileman fileman\n' "$fileman"
printf 'prog "%s" reliefos-calc calc\n' "$calculator"
printf 'prog "%s" reliefos-osver osver\n' "$information"
printf 'prog "%s" file nedit\n' "$editor"
printf 'prog "%s" app netsurf\n' "$browser"
printf '%s\n' 'prog "xeyes" app xeyes'
printf '%s\n' 'includeprog icewm-menu-fdo --sep-before -d1400 -D1650 -S --limit-max-len=42' 'separator'
printf 'prog "%s" about xterm -title "%s" -geometry 62x12 -e sh /etc/reliefos/icewm/about.sh\n' "$about" "$about"
printf '%s\n' 'separator'
printf 'prog "%s" logout killall icewm\n' "$logout"
