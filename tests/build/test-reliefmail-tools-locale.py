#!/usr/bin/env python3
"""Check that Task8A tools and action messages resolve to Chinese in the built MO."""

import ast
import gettext
import pathlib
import re
import sys


ROOT = pathlib.Path(__file__).resolve().parents[2]
T_CALL = re.compile(r'\bT\(("(?:\\.|[^"\\])*")\)')
REQUIRED = {
    "Uncertain delivery": "投递待复核",
    "Queued; not sent": "已排队，尚未发送",
    "Archive pending": "等待归档",
    "Archived in Sent": "已归档到已发送",
    "Delivery history loaded.": "已加载投递历史。",
    "Send queued": "发送排队项",
    "Operation #%llu\nMessage-ID: %s\nStage %u · delivery count %u\nFrom: %s\nTo: ":
        "操作 #%llu\n邮件 ID：%s\n阶段 %u · 投递次数 %u\n发件人：%s\n收件人：",
    "\nCurrent account generation %llu\nExact IMAP destination: %s\nFolder generation %llu · UIDVALIDITY %llu\n":
        "\n当前账户配置代次 %llu\n确切 IMAP 目标：%s\n文件夹代次 %llu · UID 有效标识 %llu\n",
    "Review Sent-folder destination": "复核已发送文件夹目标",
    "SMTP delivery is already accepted or explicitly reviewed. This confirmation only binds a current IMAP folder; it never resends SMTP.":
        "SMTP 投递已确认或已明确复核。本确认仅绑定当前 IMAP 文件夹，不会重新发送 SMTP。",
    "Confirm archive destination": "确认归档目标",
    "Type the operation review phrase to save this exact IMAP destination.":
        "输入操作复核短语以保存此确切 IMAP 目标。",
    "Choose destination folder": "选择目标文件夹",
    "Choose": "选择",
    "Type the text above": "输入上方显示的口令",
    "Archive retry did not reach the archived state; SMTP was not retried.":
        "归档重试尚未完成；未重试 SMTP。",
    "The original message selection changed while refreshing the destination. Select it again.":
        "刷新目标文件夹期间，原邮件选择已变化。请重新选择。",
    "The original message selection changed while refreshing the destination.":
        "刷新目标文件夹期间，原邮件选择已变化，请重新选择。",
    "The account changed while destination folders were being refreshed. Review the current account and select messages again.":
        "刷新目标文件夹期间，账户已变化。请检查当前账户并重新选择邮件。",
    "The original message selection could not be retained. Select the messages again.":
        "无法保留原邮件选择。请重新选择邮件。",
    "The destination folder is no longer available; choose it again.":
        "目标文件夹已不可用，请重新选择。",
    "Account %llu · %s · UIDVALIDITY %llu · UID %u\n":
        "账户 %llu · %s · UID 有效标识 %llu · UID %u\n",
    "Destination account %llu · %s · folder generation %llu · UIDVALIDITY %llu\n":
        "目标账户 %llu · %s · 文件夹代次 %llu · UID 有效标识 %llu\n",
    "Account %llu\nServer folder: %s\nFolder generation %llu · UIDVALIDITY %llu\nDeletes are targeted by exact UIDPLUS identity.":
        "账户 %llu\n服务器文件夹：%s\n文件夹代次 %llu · UID 有效标识 %llu\n删除操作将严格针对 UIDPLUS 标识。",
    "Operation #%llu · %s · delivery count %u\nMessage-ID: %s\nFrom: %s\nTo: %s\nArchive target: %s · account generation %llu · folder generation %llu · UIDVALIDITY %llu\n%s":
        "操作 #%llu · %s · 投递次数 %u\n邮件 ID：%s\n发件人：%s\n收件人：%s\n归档目标：%s · 账户配置代次 %llu · 文件夹代次 %llu · UID 有效标识 %llu\n%s",
}


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: test-reliefmail-tools-locale.py PATH_TO_MO", file=sys.stderr)
        return 2
    catalog_path = pathlib.Path(sys.argv[1])
    try:
        with catalog_path.open("rb") as catalog_file:
            catalog = gettext.GNUTranslations(catalog_file)
    except (OSError, EOFError) as error:
        print(f"cannot load ReliefMail locale catalog: {error}", file=sys.stderr)
        return 2

    source_messages = set(REQUIRED)
    for relative in (
        "userland/apps/reliefmail/tools.cpp",
        "userland/apps/reliefmail/window_actions.cpp",
    ):
        source = (ROOT / relative).read_text(encoding="utf-8")
        source_messages.update(ast.literal_eval(match) for match in T_CALL.findall(source))

    missing = sorted(message for message in source_messages if catalog.gettext(message) == message)
    incorrect = sorted(
        (message, expected, catalog.gettext(message))
        for message, expected in REQUIRED.items()
        if catalog.gettext(message) != expected
    )
    if missing or incorrect:
        if missing:
            print("untranslated Task8A tools/action messages:", file=sys.stderr)
            for message in missing:
                print(f"  {message!r}", file=sys.stderr)
        if incorrect:
            print("Task8A Chinese labels do not match the reviewed text:", file=sys.stderr)
            for message, expected, actual in incorrect:
                print(f"  {message!r}: expected {expected!r}, got {actual!r}", file=sys.stderr)
        return 1

    print(f"ReliefMail Task8A tools/action Chinese catalog passed ({len(source_messages)} messages)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
