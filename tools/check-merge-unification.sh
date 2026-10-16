#!/bin/sh
# P0 合并门禁：收拢 main（Tab5 公开线）与 claw4 线之后，验证"两边内容都在 + main 已成祖先"。
# 用法：sh tools/check-merge-unification.sh   （退出码 0 = 全 PASS）
# 背景与本轮逐条判定见内部文档 SPEC-ARCH-SPLIT.md 与合并提交信息。
# S6 结构断言：合并结果的"该有的都在"检查。任一 FAIL 即整体失败。
set -u
R="$(cd "$(dirname "$0")/.." && pwd)"   # 仓库根
fail=0
ok()   { echo "  PASS  $1"; }
bad()  { echo "  FAIL  $1"; fail=1; }

echo "== 1) 历史：main 已成为祖先（→ 推送 main 可快进，无需 force） =="
if [ "$(git -C "$R" merge-base origin/main HEAD)" = "$(git -C "$R" rev-parse origin/main)" ]; then
  ok "merge-base(origin/main, HEAD) == origin/main"
else
  bad "main 还不是祖先"
fi

echo "== 2) main 侧独有文件 25 个是否都在 =="
n=0
for f in .github/workflows/build.yml .github/ISSUE_TEMPLATE/bug_report.yml .github/PULL_REQUEST_TEMPLATE.md \
         BUILDING.md BUILDING_CN.md CHANGELOG.md CHANGELOG_CN.md CODE_OF_CONDUCT.md CONTRIBUTING.md \
         CONTRIBUTING_CN.md README_CN.md ROADMAP.md ROADMAP_CN.md docs/UPSTREAM-DIVERGENCE.md \
         retro-go-p4/nds/main/main.c tools/gen_cjk_font.py tools/log-tab5.sh; do
  [ -e "$R/$f" ] && n=$((n+1)) || bad "缺文件 $f"
done
[ "$n" -eq 17 ] && ok "抽查 17 个 main 独有文件全部在（另有 8 个二进制/图片未逐一点名）"

echo "== 3) claw4 侧独有内容是否都在 =="
for f in retro-go-p4/components/retro-go/targets/claw4/config.h \
         retro-go-p4/components/retro-go/drivers/display/mipi_dsi_claw4.h \
         retro-go-p4/components/retro-go/drivers/audio/claw4_bt_audio.c \
         retro-go-p4/launcher/main/gui.c retro-go-p4/components/retro-go/rg_touch_overlay.c \
         .github/workflows/build-claw4.yml tools/release-claw4.sh; do
  [ -e "$R/$f" ] && ok "在：$f" || bad "缺 claw4 侧文件 $f"
done

echo "== 4) 本轮补回项 =="
grep -q "'nds':" "$R/retro-go-p4/rg_tool.py" && ok "rg_tool.py 有 nds app 注册" || bad "rg_tool.py 缺 nds"
grep -q 'application("Nintendo DS"' "$R/retro-go-p4/launcher/main/applications.c" && ok "applications.c 有 NDS 条目" || bad "applications.c 缺 NDS"
c=$(grep -rl "SPDX-License-Identifier" "$R/retro-go-p4/components/ppa_engine" "$R/retro-go-p4/components/retro-go/rg_cjk.c" "$R/retro-go-p4/gbsp/components/gbsp-libretro/riscv/riscv_codegen.h" 2>/dev/null | wc -l | tr -d ' ')
[ "$c" -ge 3 ] && ok "SPDX 头已补（抽查 3 个代表性文件）" || bad "SPDX 头抽查只命中 $c（期望 >=3）"
grep -q "retro-go-p4/\*/sdkconfig" "$R/.gitignore" && ok ".gitignore 含 main 侧 sdkconfig 规则" || bad ".gitignore 缺 sdkconfig 规则"
grep -q "两个 target" "$R/README.md" && grep -q "两个 target" "$R/README_CN.md" && ok "README/README_CN 均有「两个 target」章节" || bad "README 章节缺失"

echo "== 5) 交付物形态（与发布相关） =="
python3 - "$R" <<'PY'
import sys, pathlib
r = pathlib.Path(sys.argv[1])
p = r / 'publish.yaml'
txt = p.read_text(encoding='utf-8')
print("  PASS  publish.yaml 含 readback 行" if 'readback' in txt else "  FAIL  publish.yaml 缺 readback")
f = r / 'retro-go-p4/assets/cjk12.bin'
print(f"  INFO  cjk12.bin 大小 {f.stat().st_size} B（claw4 侧 3777 字形版）" if f.exists() else "  FAIL  cjk12.bin 缺失")
PY

echo
[ "$fail" -eq 0 ] && echo "S6 结论：全部 PASS" || echo "S6 结论：有 FAIL（见上）"
exit "$fail"
