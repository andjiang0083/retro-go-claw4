#!/usr/bin/env python3
"""R1 隔离门禁：共享层里不许出现"板名条件块"和"板专属标识符"。

为什么（本项目的头号架构病）：两块板（Tab5 / Metalio Claw4）长期挤在**同一条代码线**里，
板级差异靠"在共享文件里写 `#if defined(RG_TARGET_CLAW4)`"来分叉 ⇒ 给一块板加东西要动
共享文件，还要证明另一块板二进制零位移。本门禁把这条规矩变成**机器可判**，并且用
**棘轮（ratchet）**收敛：现存违例记进 allowlist 带上限，**只许减少**；新违例即红。

判据（任一不满足 ⇒ 退出码 1）：
  A. 条件块：共享文件的 `#if/#ifdef/#ifndef/#elif/#else/#endif` 行里出现板名/板专属宏；
  B. 标识符：共享文件的**代码行**（跳过注释行）里出现板专属标识符
     （`claw4_*` / `tab5l_*` / `tab5p_*` / `mipi_dsi_tab5*` / `mipi_dsi_claw4` / `es8388`）。
超出 allowlist 上限、或未登记的文件 ⇒ 红。数量下降 **不** 报错（鼓励收敛），用
`--update` 可以把上限**下调**；`--init/--update` 拒绝上调（要用 `--force` 显式承认倒退）。
自检：打印"扫描了多少个共享文件"，避免"0 个文件也算 PASS"的假绿。

用法：
  python3 tools/check-board-isolation.py                 # 门禁（CI 里跑这个）
  python3 tools/check-board-isolation.py --list          # 只列现状
  python3 tools/check-board-isolation.py --update        # 按下调棘轮更新 allowlist
"""
import re
import sys
import pathlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
COMP = ROOT / 'retro-go-p4/components/retro-go'
ALLOW = ROOT / 'tools/board-isolation-allowlist.txt'

COND = re.compile(
    r'^\s*#\s*(?:if|ifdef|ifndef|elif|else|endif)\b.*'
    r'\b(?:RG_TARGET_CLAW4|RG_TARGET_TAB5|RG_GAMEPAD_TOUCH_MAP_GB|RG_CLAW4_\w+|RG_TAB5_\w+)\b')
SYM = re.compile(r'\b(?:claw4_[a-z0-9_]+|tab5[lp]_[a-z0-9_]+|mipi_dsi_claw4|mipi_dsi_tab5|es8388)\b')

# 板级自家文件（允许出现板名）：targets/、port/ 契约层、板专属驱动/包装
BOARD_OWNED_PREFIXES = ('targets/', 'port/')
BOARD_OWNED_NAMES = ('tab5_', 'claw4_', 'mipi_dsi_tab5', 'mipi_dsi_claw4')


def is_shared(rel: str, name: str) -> bool:
    if rel.startswith(BOARD_OWNED_PREFIXES):
        return False
    if name.startswith(BOARD_OWNED_NAMES):
        return False
    return name.endswith(('.c', '.h'))


def is_comment(line: str) -> bool:
    s = line.strip()
    return s.startswith(('//', '/*', '*', '*/'))


def scan():
    hits = {}
    scanned = 0
    for p in sorted(COMP.rglob('*')):
        if not p.is_file():
            continue
        rel = str(p.relative_to(COMP))
        if not is_shared(rel, p.name):
            continue
        scanned += 1
        text = p.read_text(encoding='utf-8', errors='replace')
        cond, sym = [], []
        for i, line in enumerate(text.splitlines(), 1):
            if COND.search(line):
                cond.append(i)
            elif SYM.search(line) and not is_comment(line):
                sym.append(i)
        if cond or sym:
            hits[rel] = {'cond': cond, 'sym': sym}
    return scanned, hits


def load_allow():
    table = {}
    if ALLOW.exists():
        for line in ALLOW.read_text(encoding='utf-8').splitlines():
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            parts = line.split('|')
            if len(parts) >= 3:
                path, kind, cap = parts[0], parts[1], int(parts[2])
                table[(path, kind)] = cap
    return table


def write_allow(hits, table, force):
    lines = [
        '# R1 隔离门禁的棘轮上限：共享层允许残留的违例（只许减少，新增即红）',
        '# 格式：相对路径|类型(cond/sym)|上限|理由',
        '# 每清一处就把上限下调（--update）；想上调必须显式 --force（= 承认架构倒退）',
    ]
    for rel in sorted(hits):
        for kind, key in (('cond', 'cond'), ('sym', 'sym')):
            n = len(hits[rel][kind])
            if not n:
                continue
            old = table.get((rel, kind), 0)
            if old and n > old and not force:
                print(f"拒绝：{rel} 的 {kind} 从 {old} 涨到 {n}（架构倒退）。"
                      f"确认要接受就加 --force，否则先修掉新增的那处。")
                sys.exit(2)
            lines.append(f"{rel}|{kind}|{n}|")
    ALLOW.write_text('\n'.join(lines) + '\n', encoding='utf-8')
    print(f"已写入 {ALLOW.relative_to(ROOT)}（{len(lines)-3} 条）")


def main():
    args = sys.argv[1:]
    scanned, hits = scan()
    table = load_allow()

    if '--init' in args or '--update' in args:
        write_allow(hits, table, '--force' in args)
        return 0

    print(f"扫描了 {scanned} 个共享文件（retro-go-p4/components/retro-go 下、排除板级自家文件）")
    if scanned < 20:
        print(f"FAIL: 只扫到 {scanned} 个文件，太少了 —— 路径写错或仓库结构变了，"
              f"这种'0 项检查'式的通过不算通过")
        return 1

    if '--list' in args:
        for rel in sorted(hits):
            print(f"  cond={len(hits[rel]['cond']):2d} sym={len(hits[rel]['sym']):2d} 行号 "
                  f"cond{hits[rel]['cond']} sym{hits[rel]['sym']}  {rel}")
        return 0

    fail = []
    for rel in sorted(hits):
        for kind in ('cond', 'sym'):
            n = len(hits[rel][kind])
            if not n:
                continue
            cap = table.get((rel, kind))
            if cap is None:
                fail.append(f"{rel}: {kind} {n} 处**未登记**（新违例）行号 {hits[rel][kind][:6]}")
            elif n > cap:
                fail.append(f"{rel}: {kind} {n} 处 > 上限 {cap}（棘轮只许减少）")
    if fail:
        print("FAIL:")
        for f in fail:
            print("  " + f)
        print("\n修法：板级差异搬到 targets/<板>/ 或 port/ 契约层；确需保留则 --update 登记（并写下理由）")
        return 1

    total = sum(len(h[k]) for h in hits.values() for k in ('cond', 'sym'))
    reduced = sum(1 for rel in hits for k in ('cond', 'sym')
                  if len(hits[rel][k]) < table.get((rel, k), 0))
    print(f"PASS: 无未登记违例；现存 {total} 处已登记（其中 {reduced} 处已低于上限 ⇒ 可 --update 收紧棘轮）")
    return 0


if __name__ == '__main__':
    sys.exit(main())
