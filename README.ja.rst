========
ReliefOS
========

`简体中文 <README.rst>`_ | `English <README.en.rst>`_ | 日本語

ReliefOS は x86_64 向けのオペレーティングシステムプロジェクトです。
カーネルは kernel/reliefnt サブモジュールにあります。標準デスクトップには
Xorg、XDM、IceWM を使用し、中国語を扱うネイティブアプリケーション、
コンソールのみのインストール、BIOS/UEFI 起動に対応しています。
現行の原文マニュアルの入口は
`Documentation/index.rst <Documentation/index.rst>`_ です。

ビルドの入口はリポジトリルートの GNU Make 4.3+ です。独自のソースは固定した
Linux Kbuild/Kconfig でコンパイルします。サブモジュールを初期化し、
バージョンを固定した依存関係を明示的に取得してからビルドしてください。
詳しい手順は `ビルドマニュアル <Documentation/kbuild/building.rst>`_ を参照してください。
HTML マニュアルは次のコマンドで生成します::

    make O=out/docs htmldocs

簡体字中国語、英語、日本語を同時に生成します。既存の中国語 URL を既定の入口とし、
出力の en/ に英語、ja/ に日本語を配置します。各ページから他言語の同じページへ
切り替えられます。

コンパイル、イメージ生成、実際の動作は別々に検証します。既知の制限は
`対応範囲 <Documentation/admin-guide/limitations.rst>`_ を参照してください。
VMware での検証は未完了です。貢献する前に AGENT.md をお読みください。
ライセンスは LICENSE を参照してください。上流のライセンスと帰属情報は原文を保持します。
Linux ABI、musl などへの VasilyZa の貢献に感謝します。

ReliefNT のカーネルマニュアルは kernel/reliefnt/Documentation/ で独立して管理します。
完全な Pages/RPR サイトは docs/reliefos/ と docs/reliefnt/ にそれぞれのマニュアルを
公開します。
