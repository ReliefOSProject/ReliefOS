# sl for ReliefOS

ReliefOS ports upstream `mtoyoda/sl` (Steam Locomotive) at commit
`923e7d7ebc5c1f009755bdeb789ac25658ccce03`. It is a joke command that draws
a train across the terminal when `sl` is typed instead of `ls`.

The port keeps the upstream animation and options while replacing curses with
a small ANSI terminal layer:

```text
sl          Draw a D51 steam locomotive
sl -l       Draw the long locomotive
sl -a       Add people waving at the train
sl -F       Make the train fly diagonally
sl -c       Draw the C51 locomotive
```

The command is installed as `/usr/bin/sl` and is available from
BusyBox Ash. The upstream copyright and license are kept
in the adjacent `LICENSE` file.
