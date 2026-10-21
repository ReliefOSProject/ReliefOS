#!/usr/bin/env python3
"""Analyze ReliefOS kernel and dynamic-loader boot logs.

Examples:
    python tools/analyze_boot_log.py serial.log
    qemu-system-x86_64 ... 2>&1 | python tools/analyze_boot_log.py -
    python tools/analyze_boot_log.py serial.log --json tools/dist/boot-report.json
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Iterable


SEVERITY_ORDER = {"信息": 0, "警告": 1, "错误": 2, "致命": 3}
EXCEPTION_NAMES = {
    0: "除零错误", 6: "无效操作码", 8: "双重错误", 13: "通用保护错误",
    14: "页错误", 16: "x87 浮点错误", 17: "对齐检查", 18: "机器检查",
    19: "SIMD 浮点错误", 21: "控制保护错误",
}


@dataclass(frozen=True)
class Finding:
    """One diagnosed log event, including evidence and a practical next step."""

    identifier: str
    severity: str
    category: str
    title: str
    lines: tuple[int, ...]
    evidence: tuple[str, ...]
    explanation: str
    action: str
    source_hint: str


@dataclass(frozen=True)
class LogAnalysis:
    """Structured result returned by :func:`analyze_lines`."""

    line_count: int
    components: dict[str, int]
    boot_complete_line: int | None
    status: str
    findings: tuple[Finding, ...]


@dataclass(frozen=True)
class Rule:
    """A direct log pattern whose fields do not need cross-line correlation."""

    identifier: str
    severity: str
    category: str
    expression: str
    title: str
    explanation: str
    action: str
    source_hint: str


DIRECT_RULES = (
    Rule(
        "STORAGE-ROOT", "错误", "存储",
        r"^\[(?:reliefnt|ntclks)\] no block-backed root filesystem available for userland$",
        "用户态没有可用的根文件系统",
        "内核无法找到可供用户态加载的块设备根文件系统。",
        "检查磁盘控制器、ext2 根分区、ESP/FAT32 启动分区、挂载策略和虚拟机磁盘连接。",
        "kernel/reliefnt/kernel/exec/userland.c:636",
    ),
    Rule(
        "USERLAND-LOAD", "错误", "用户态启动",
        r"^\[(?:reliefnt|ntclks)\] failed to load (?:init\.elf|desktop\.elf).*",
        "关键用户态程序未能加载",
        "init.elf 或 desktop.elf 在创建任务时失败，桌面启动无法完成。",
        "先查看前后是否有 ELF 验证、动态解释器或文件查找错误；再检查镜像中对应程序是否完整。",
        "kernel/reliefnt/kernel/exec/userland.c:654",
    ),
    Rule(
        "ELF-HEADER", "错误", "ELF 装载",
        r"^\[(?:reliefnt|ntclks)\] ELF (?:main|interpreter) header (?:read|validation) failed$",
        "ELF 头部读取或验证失败",
        "程序或动态解释器不是当前 ReliefOS 接受的完整 x86_64 ELF，或 ABI note、程序头和段约束未通过。",
        "用 readelf 检查 ELF 类型、PT_INTERP、ABI note 和段权限；确认镜像未截断且构建产物来自当前工具链。",
        "kernel/reliefnt/kernel/exec/elf.c:848",
    ),
    Rule(
        "ELF-INTERPRETER", "错误", "动态链接",
        r"^\[(?:reliefnt|ntclks)\] ELF interpreter lookup failed path=",
        "动态 ELF 缺少解释器",
        "动态应用声明的 ELF 解释器无法从系统镜像读取。",
        "检查日志中的解释器路径；musl 镜像应包含 /lib/ld-musl-x86_64.so.1，并检查读取权限与完整性。",
        "kernel/reliefnt/kernel/exec/elf.c:892",
    ),
    Rule(
        "ELF-ABI", "错误", "动态链接",
        r"^\[(?:reliefnt|ntclks)\] ELF ABI mismatch main=",
        "动态应用与解释器 ABI 主版本不匹配",
        "检测到旧私有 ABI 程序与解释器不匹配。",
        "使用当前 musl SDK 从源码重建应用，避免混用旧私有 ABI 输出。",
        "kernel/reliefnt/kernel/exec/elf.c:904",
    ),
    Rule(
        "ELF-MAP", "错误", "ELF 装载",
        r"^\[(?:reliefnt|ntclks)\] failed to (?:map executable|load .* into private address space|prepare argv/envp for) ",
        "用户程序映射到地址空间失败",
        "内核在 ELF 验证、段映射、私有地址空间或启动参数准备阶段失败。",
        "结合相邻的 ELF 细节日志检查段布局、VMA 容量、页权限、解释器和镜像文件内容。",
        "kernel/reliefnt/kernel/exec/userland.c:417",
    ),
    Rule(
        "DYNLINK-MISSING", "错误", "动态链接",
        r"^\[dynlinkerror\.elf\] unable to start .*: missing ",
        "动态链接库缺失",
        "静态链接的恢复程序已确认应用缺少必需共享库。",
        "从匹配系统镜像恢复该 .so 文件到 /usr/lib/reliefos、兼容目录 /usr/lib/leonos 或 /usr/lib，或重新打包应用私有库。",
        "userland/apps/dynlinkerror/main.c:52",
    ),
    Rule(
        "DYNLINK-RUNTIME", "错误", "动态链接",
        r"(?:shared object not found|shared object cannot be opened|unresolved dynamic symbol|dynamic application must depend on libleonos\.so\.1)",
        "动态运行时无法解析应用依赖",
        "动态加载器拒绝或无法装载依赖库，或者无法解析应用所需符号。",
        "检查 DT_NEEDED、库名、/usr/lib/leonos 与 /lib 内容、ABI note 和导出符号；使用 ELF/动态链接检查器确认依赖树。",
        "userland/runtime/ld_leonos.c:692",
    ),
    Rule(
        "ASLR-WEAK-ENTROPY", "警告", "内存保护",
        r"^\[(?:reliefnt|ntclks)\] ASLR active with weak entropy ",
        "ASLR 正在使用弱熵源",
        "系统仍会随机化布局，但当前启动没有获得 RDRAND 硬件熵。",
        "在支持硬件熵的 CPU/虚拟机上启用 RDRAND，或检查启动时序和输入熵是否正常累积。",
        "kernel/reliefnt/kernel/exec/elf.c:156",
    ),
    Rule(
        "POWER-ACPI", "警告", "电源管理",
        r"^\[(?:reliefnt|ntclks)\] ACPI (?:RSDP|FADT|DSDT|_S5_|PM1 control block) unavailable$",
        "ACPI 关机路径不可用",
        "固件没有提供完成 ACPI S5 关机所需的表或寄存器，关机可能只能停止 CPU。",
        "检查虚拟机 ACPI 选项、固件类型和硬件平台；在 VMware 中验证 ACPI 电源管理已启用。",
        "kernel/reliefnt/arch/x86_64/platform/power.c:532",
    ),
)

BUGCHECK_LINE = re.compile(r"^\[bugcheck\] (.*)$")
EXCEPTION_LINE = re.compile(
    r"^\[(?:reliefnt|ntclks)\] exception vector=(?P<vector>\d+) error=(?P<error>0x[0-9a-fA-F]+) "
    r"rip=(?P<rip>0x[0-9a-fA-F]+).*"
)
BOOT_COMPLETE = re.compile(r"^\[(?:reliefnt|ntclks)\] boot complete:")
TASK_EXIT = re.compile(r"^\[(?:reliefnt|ntclks)\] scheduler task exited pid=(?P<pid>\d+) name=(?P<name>.+) code=(?P<code>\d+)$")
MAP_FAILURE = re.compile(r"^\[(?:reliefnt|ntclks)\] failed to map executable (?P<name>.+)$")
COMPONENT = re.compile(r"^\[(?P<component>[^\]]+)\]")


def _context(lines: list[str], indexes: Iterable[int], radius: int) -> tuple[str, ...]:
    """Return de-duplicated, one-based line-numbered context for an event."""
    positions: set[int] = set()
    for index in indexes:
        positions.update(range(max(0, index - radius), min(len(lines), index + radius + 1)))
    return tuple(f"{index + 1}: {lines[index]}" for index in sorted(positions))


def _finding_from_rule(rule: Rule, lines: list[str], index: int, radius: int) -> Finding:
    """Instantiate a direct-rule finding at one matching log line."""
    return Finding(
        rule.identifier, rule.severity, rule.category, rule.title, (index + 1,),
        _context(lines, (index,), radius), rule.explanation, rule.action, rule.source_hint,
    )


def _bugcheck_findings(lines: list[str], radius: int) -> list[Finding]:
    """Collapse each multi-line bugcheck report into a single fatal finding."""
    findings: list[Finding] = []
    index = 0
    while index < len(lines):
        match = BUGCHECK_LINE.match(lines[index])
        if not match:
            index += 1
            continue
        indexes = [index]
        details = [match.group(1)]
        cursor = index + 1
        while cursor < len(lines):
            next_match = BUGCHECK_LINE.match(lines[cursor])
            if not next_match:
                break
            indexes.append(cursor)
            details.append(next_match.group(1))
            cursor += 1
        reason = details[0] if details else "未知 bugcheck"
        findings.append(Finding(
            "BUGCHECK", "致命", "内核崩溃", f"Bugcheck: {reason}",
            tuple(item + 1 for item in indexes), _context(lines, indexes, radius),
            "内核进入不可恢复的停止状态；同一段日志通常包含陷阱向量、任务名、RIP、CR2 和栈指针。",
            "优先检查本条之前最近的 exception、page fault、ELF 或驱动错误；使用 RIP 和任务名定位对应模块。",
            "kernel/reliefnt/lib/bugcheck.c:422",
        ))
        index = cursor
    return findings


def _exception_findings(lines: list[str], radius: int) -> list[Finding]:
    """Diagnose CPU exception records and link nearby bugchecks when present."""
    findings: list[Finding] = []
    for index, line in enumerate(lines):
        match = EXCEPTION_LINE.match(line)
        if not match:
            continue
        vector = int(match.group("vector"))
        fatal = any(BUGCHECK_LINE.match(candidate) for candidate in lines[index + 1:index + 6])
        name = EXCEPTION_NAMES.get(vector, "CPU 异常")
        severity = "致命" if fatal else "错误"
        suffix = "，随后触发 bugcheck" if fatal else ""
        findings.append(Finding(
            "CPU-EXCEPTION", severity, "CPU 异常",
            f"异常向量 {vector}: {name}{suffix}", (index + 1,),
            _context(lines, (index,), radius),
            f"CPU 在 RIP={match.group('rip')} 触发向量 {vector}，错误码为 {match.group('error')}。",
            "依据 RIP、当前 pid/task 和异常模式检查近期映射、重定位、用户指针或特权级转换；页错误还需检查 CR2 与标志位。",
            "kernel/reliefnt/arch/x86_64/interrupt/idt.c:156",
        ))
    return findings


def _task_exit_findings(lines: list[str], radius: int) -> list[Finding]:
    """Report failed essential process exits without flagging ordinary exits."""
    map_failures: dict[str, int] = {}
    for index, line in enumerate(lines):
        match = MAP_FAILURE.match(line)
        if match:
            map_failures[match.group("name")] = index
    findings: list[Finding] = []
    for index, line in enumerate(lines):
        match = TASK_EXIT.match(line)
        if not match or int(match.group("code")) != 127:
            continue
        name = match.group("name")
        related = map_failures.get(name)
        locations = (related, index) if related is not None else (index,)
        explanation = "进程以 127 退出，通常表示可执行文件未被成功装载。"
        if related is not None:
            explanation += " 同一任务此前已报告 executable map failure。"
        findings.append(Finding(
            "TASK-EXIT-127", "错误", "用户态启动", f"任务 {name} 因启动失败退出 (127)",
            tuple(location + 1 for location in locations), _context(lines, locations, radius),
            explanation,
            "查看该任务之前的 ELF 头部、解释器、ABI、段映射或动态库错误；init.elf 和 desktop.elf 失败会阻断系统桌面。",
            "kernel/reliefnt/kernel/sched/core.c",
        ))
    return findings


def _deduplicate(findings: Iterable[Finding]) -> tuple[Finding, ...]:
    """Keep one finding for each diagnostic and exact evidence line set."""
    seen: set[tuple[str, tuple[int, ...]]] = set()
    output: list[Finding] = []
    for finding in findings:
        key = (finding.identifier, finding.lines)
        if key not in seen:
            seen.add(key)
            output.append(finding)
    return tuple(sorted(output, key=lambda item: (-SEVERITY_ORDER[item.severity], item.lines[0])))


def analyze_lines(raw_lines: Iterable[str], context: int = 1) -> LogAnalysis:
    """Analyze a sequence of raw log lines and return a structured diagnosis."""
    lines = [line.rstrip("\r\n") for line in raw_lines]
    components: dict[str, int] = {}
    for line in lines:
        match = COMPONENT.match(line)
        if match:
            name = match.group("component")
            components[name] = components.get(name, 0) + 1

    findings: list[Finding] = _bugcheck_findings(lines, context)
    findings.extend(_exception_findings(lines, context))
    findings.extend(_task_exit_findings(lines, context))
    for index, line in enumerate(lines):
        for rule in DIRECT_RULES:
            if re.search(rule.expression, line):
                findings.append(_finding_from_rule(rule, lines, index, context))
    complete = next((index + 1 for index, line in enumerate(lines) if BOOT_COMPLETE.match(line)), None)
    normalized = _deduplicate(findings)
    highest = max((SEVERITY_ORDER[item.severity] for item in normalized), default=0)
    if not lines:
        status = "空日志"
    elif highest >= SEVERITY_ORDER["致命"]:
        status = "启动失败"
    elif complete is None:
        status = "启动未完成"
    elif highest >= SEVERITY_ORDER["错误"]:
        status = "启动完成但存在错误"
    elif highest >= SEVERITY_ORDER["警告"]:
        status = "启动完成但存在警告"
    else:
        status = "启动正常"
    return LogAnalysis(len(lines), components, complete, status, normalized)


def _print_text(analysis: LogAnalysis) -> None:
    """Render a concise human-readable report to standard output."""
    counts = {severity: 0 for severity in SEVERITY_ORDER}
    for finding in analysis.findings:
        counts[finding.severity] += 1
    print("ReliefOS 启动日志分析")
    print(f"状态: {analysis.status}")
    print(f"日志行: {analysis.line_count}")
    print("启动完成: " + (f"第 {analysis.boot_complete_line} 行" if analysis.boot_complete_line else "未检测到"))
    print("问题统计: " + "，".join(f"{severity} {counts[severity]}" for severity in ("致命", "错误", "警告", "信息")))
    if analysis.components:
        print("日志组件: " + "，".join(f"{name} {count}" for name, count in sorted(analysis.components.items())))
    if not analysis.findings:
        print("\n未检测到已知的启动、ELF、动态链接或内核崩溃问题。")
        return
    for number, finding in enumerate(analysis.findings, 1):
        print(f"\n{number}. [{finding.severity}] {finding.identifier}: {finding.title}")
        print("   行号: " + ", ".join(str(line) for line in finding.lines))
        print("   判断: " + finding.explanation)
        print("   建议: " + finding.action)
        print("   源码: " + finding.source_hint)
        print("   日志:")
        for evidence in finding.evidence:
            print("     " + evidence)


def _self_test() -> int:
    """Exercise correlations for the ELF and CPU-fault failures seen in practice."""
    sample = """[ntclks] boot complete: version=4.4.0 root=/ fs=ext2 desktop=desktop.elf
[ntclks] ELF main header validation failed
[ntclks] failed to map executable init.elf
[ntclks] scheduler task exited pid=1 name=init.elf code=127
[ntclks] exception vector=13 error=0x0 rip=0x1211c18 cs=0x23 rflags=0x10206 rsp=0x6ffe188 ss=0x1b
[ntclks] exception mode=user cr2=0x1211c00 ticks=10164

[bugcheck] General Protection Fault
[bugcheck] detail=Unhandled CPU exception
"""
    expected = {"ELF-HEADER", "ELF-MAP", "TASK-EXIT-127", "CPU-EXCEPTION", "BUGCHECK"}
    # Both kernel log prefixes must parse: [ntclks] is the pre-rename spelling.
    for lines in (sample.splitlines(),
                  sample.replace("[ntclks]", "[reliefnt]").splitlines()):
        analysis = analyze_lines(lines)
        identifiers = {finding.identifier for finding in analysis.findings}
        missing = expected - identifiers
        if missing or analysis.status != "启动失败":
            print(f"boot-log analyzer self-test failed: missing={sorted(missing)} status={analysis.status}", file=sys.stderr)
            return 1
    print("boot-log analyzer self-test passed")
    return 0


def main(argv: list[str] | None = None) -> int:
    """Parse arguments, analyze one log, and optionally write JSON output."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", nargs="?", default="-", help="log file path, or - for standard input")
    parser.add_argument("--context", type=int, default=1, help="context lines around each finding (default: 1)")
    parser.add_argument("--json", dest="json_output", type=Path, help="write the report as JSON; use - for stdout")
    parser.add_argument("--strict", action="store_true", help="return 1 when an error or fatal finding is present")
    parser.add_argument("--self-test", action="store_true", help="run built-in parser checks")
    args = parser.parse_args(argv)
    if args.self_test:
        return _self_test()
    if args.context < 0:
        parser.error("--context must be non-negative")
    if args.log == "-":
        lines = sys.stdin.read().splitlines()
    else:
        try:
            lines = Path(args.log).read_text(encoding="utf-8", errors="replace").splitlines()
        except OSError as exc:
            parser.error(f"cannot read log {args.log}: {exc}")
    analysis = analyze_lines(lines, args.context)
    payload = asdict(analysis)
    if args.json_output:
        rendered = json.dumps(payload, ensure_ascii=False, indent=2) + "\n"
        if str(args.json_output) == "-":
            sys.stdout.write(rendered)
        else:
            args.json_output.parent.mkdir(parents=True, exist_ok=True)
            args.json_output.write_text(rendered, encoding="utf-8", newline="\n")
            _print_text(analysis)
    else:
        _print_text(analysis)
    return 1 if args.strict and any(SEVERITY_ORDER[item.severity] >= SEVERITY_ORDER["错误"] for item in analysis.findings) else 0


if __name__ == "__main__":
    raise SystemExit(main())
