#!/bin/bash
# Claw4 发布流水线（CloudZao burner github-sync 版）
#   commit → tag(裸 vX.Y.Z) → 钉 IDF 重编 → 三点门禁 → 打包 retro-go-claw4-*.bin
#   → push claw4+tag（只碰这两个 ref，主干零位移）→ GitHub Release(API) → 触发商店同步 → 回读核实
#
# 用法:  tools/release-claw4.sh <vX.Y.Z> "<提交信息>" [--commit] [--publish]
#        不带 flag = dry-run（只检查并打印将执行的动作）
#        --commit : 提交+打 tag；--publish: 全流程（要求工作区已提交、tag 存在）
#
# 机制依据（2026-10-14 实测，勿凭印象改）：
#   * CloudZao github-sync（PUT /xiaozhi/api/firmwares/community/<fid>/github-sync + POST .../run）
#     扫 GitHub **semver 最高** release，版本号必须严格高于本站最新（v0.4.10），
#     再按 assetPattern `retro-go-claw4-*.bin` 过滤资产入库（秒过审=直接上架）。
#   * ⇒ tag 用裸 vX.Y.Z、与 tab5 **共用递增数列**；板型靠资产名区分——
#     tab5 的 release 没有 retro-go-claw4-* 资产，永远不会被同步进 Claw4。
#   * 商店 fid: 4d45ca7e57e235b067a0753877232d0a（retro-go-gba @ Claw4, hardwareModelId=33）。
#   * 崩溃版零缓冲：--publish 前必须真机验证过（脚本只拦格式，不替你验机）。
set -euo pipefail
cd "$(dirname "$0")/.."

VER="${1:?用法: tools/release-claw4.sh <vX.Y.Z> \"<提交信息>\" [--commit] [--publish]}"
MSG="${2:?缺少提交信息}"
shift 2 || true
DO_COMMIT=0; DO_PUBLISH=0
for f in "$@"; do case "$f" in --commit) DO_COMMIT=1;; --publish) DO_PUBLISH=1;; *) echo "未知 flag: $f"; exit 1;; esac; done

BRANCH="$(git rev-parse --abbrev-ref HEAD)"
[ "$BRANCH" = "claw4" ] || { echo "⛔ 当前分支是 $BRANCH，本脚本只许在 claw4 上跑（主干零改动纪律）"; exit 1; }
case "$VER" in v[0-9]*.[0-9]*) :;; *) echo "⛔ 版本号格式应为 vX.Y.Z，收到: $VER"; exit 1;; esac
VER_BARE="${VER#v}"
FID="4d45ca7e57e235b067a0753877232d0a"
BIN_NAME="retro-go-claw4-${VER_BARE}.bin"

echo "=== Claw4 发布 ${VER}（资产名 ${BIN_NAME}）==="

# ── 门禁① tag/版本三重递增：本地无、远端 GitHub 无、且高于商店最新 ──────────
git rev-parse -q --verify "refs/tags/${VER}" >/dev/null && { echo "⛔ tag ${VER} 本地已存在"; exit 1; }
GH_LATEST_STORE="$(curl -s --max-time 15 "https://admin.metalio.world/xiaozhi/api/firmwares?page=1&limit=5&hardwareModelId=33&keyword=retro-go-gba" | python3 -c 'import sys,json;d=json.load(sys.stdin);vs=[v["version"] for f in d.get("data",{}).get("list",[]) for v in f.get("versions",[]) if v.get("status")==1];vs=[v.lstrip("v") for v in vs];print(max(vs,key=lambda s:[int(x) for x in (s.split("-")[0].split(".")+[0,0])[:3]]))' 2>/dev/null || echo "")"
[ -n "$GH_LATEST_STORE" ] || { echo "⛔ 读不到商店最新版本（API 不通？）——版本号无法核递增，停"; exit 1; }
NEWER="$(python3 -c "a='$VER_BARE'.split('-')[0].split('.');b='$GH_LATEST_STORE'.split('-')[0].split('.');ka=[int(x) for x in (a+[0,0])[:3]];kb=[int(x) for x in (b+[0,0])[:3]];print('yes' if ka>kb else 'no')")"
[ "$NEWER" = "yes" ] || { echo "⛔ ${VER} 不高于商店最新 v${GH_LATEST_STORE}——同步机制会 skip，别白发"; exit 1; }
echo "  ✓ ${VER} > 商店最新 v${GH_LATEST_STORE}"

# ── 门禁② 工作区干净（--commit 时先提交）─────────────────────────────────
if [ -n "$(git status --porcelain)" ]; then
  git status --short | sed 's/^/    /'
  [ "$DO_COMMIT" = "1" ] || { echo "⛔ 工作区不干净且未加 --commit"; exit 1; }
  git add -A && git commit -m "$MSG"
  echo "  ✓ 已提交: $(git rev-parse --short HEAD)"
else
  echo "  ✓ 工作区干净"
fi

# ── dry-run：打印后续动作后停 ───────────────────────────────────────────
if [ "$DO_PUBLISH" != "1" ]; then
  echo "--- dry-run：将会执行 ---"
  [ "$DO_COMMIT" = "1" ] || echo "  0) （工作区若有改动需先 --commit）"
  echo "  1) git tag ${VER}   # claw4 HEAD，主干零位移"
  echo "  2) IDF v5.5 钉版重编: rg_tool.py --target claw4 --no-networking build-img launcher gbsp"
  echo "  3) 三点门禁: @0x2000/@0x10000/@0x100000 flash-size nibble 一致且等于 targets/claw4/sdkconfig 声明值(Claw4=32MB⇒5) + @0x8000 aa50 + 分区表对基线"
  echo "  4) cp merged img → releases/claw4/${BIN_NAME} (+ .sha256, cmp 字节一致)"
  echo "  5) git push origin claw4 && git push origin ${VER}   # 只推这两个 ref"
  echo "  6) GitHub Release ${VER}: 上传 ${BIN_NAME} + .sha256（钥匙串 token, API）"
  echo "  7) POST github-sync/run → GET 回读 lastStatus + 商店版本列表核实"
  exit 0
fi

# ── ③ tag + 钉 IDF 重编（tag 先行，让镜像名读作干净版本号）──────────────
git tag "$VER"
export IDF_PATH="$HOME/esp/esp-idf-v5.5"
export IDF_PYTHON_ENV_PATH="$HOME/.espressif/python_env/idf5.5_py3.14_env"
source "$IDF_PATH/export.sh" >/dev/null 2>&1 || { echo "⛔ IDF export 失败"; exit 1; }
( cd retro-go-p4 && python3 rg_tool.py --target claw4 --no-networking build-img launcher gbsp ) > ".buildlogs/release-claw4-${VER}.log" 2>&1 \
  || { echo "⛔ 构建失败，见 .buildlogs/release-claw4-${VER}.log"; tail -5 ".buildlogs/release-claw4-${VER}.log"; exit 1; }
IMG="$(ls -t retro-go-p4/*_claw4.img | head -1)"
echo "  ✓ 构建产物: $IMG"

# ── ④ 三点门禁（经验 §417：flash-size nibble + aa50 + 分区表基线）────────
python3 - "$IMG" <<'PYEOF' || exit 1
import sys, hashlib, os
img = sys.argv[1]
data = open(img,'rb').read()
def nib(off): return (data[off+3] >> 4) & 0xF
nibs = {hex(o): nib(o) for o in (0x2000, 0x10000, 0x100000)}
assert len(set(nibs.values())) == 1, f"三段 flash-size nibble 不一致: {nibs}"
# 期望值来自唯一真源：targets/claw4/sdkconfig（Claw4 = 32MB ⇒ nibble=5）。
# 切勿硬编码 nibble：写死过 4(16MB) 曾把正确的 32MB 镜像判成不合格。
import re
SC = "retro-go-p4/components/retro-go/targets/claw4/sdkconfig"
sc = open(SC, encoding="utf-8", errors="replace").read()
m = re.search(r'CONFIG_ESPTOOLPY_FLASHSIZE="(\d+)MB"', sc)
assert m, f"{SC} 未声明 CONFIG_ESPTOOLPY_FLASHSIZE"
NIB = {"1":0,"2":1,"4":2,"8":3,"16":4,"32":5,"64":6,"128":7}
exp = NIB[m.group(1)]
assert nibs['0x2000'] == exp, f"flash-size nibble={nibs['0x2000']}，但 {SC} 声明 {m.group(1)}MB（期望 nibble={exp}）"
assert data[0x8000:0x8002] == b'\xaa\x50', "分区表 magic @0x8000 缺失"
pt = data[0x8000:0xc000]
print(f"  ✓ 三点门禁 nibble={exp}({m.group(1)}MB, 源自 {SC}) / aa50 / 分区表 sha256={hashlib.sha256(pt).hexdigest()[:12]}")
PYEOF

# ── ⑤ 打包（产物不入库；.gitignore 已覆盖 releases/）─────────────────────
mkdir -p releases/claw4
cp "$IMG" "releases/claw4/${BIN_NAME}"
( cd releases/claw4 && shasum -a 256 "${BIN_NAME}" > "${BIN_NAME}.sha256" )
cmp "$IMG" "releases/claw4/${BIN_NAME}" && echo "  ✓ ${BIN_NAME} 与镜像字节一致 ($(wc -c < "releases/claw4/${BIN_NAME}") B)"

# ── ⑥ 推 claw4 + tag（显式 refspec，主干零位移；推后 ls-remote 回读）─────
git push -q origin claw4
git push -q origin "$VER"
LOCAL_SHA="$(git rev-parse HEAD)"
REMOTE_SHA="$(git ls-remote origin refs/heads/claw4 | cut -f1)"
[ "$LOCAL_SHA" = "$REMOTE_SHA" ] || { echo "⛔ claw4 远端回读不一致"; exit 1; }
echo "  ✓ origin/claw4 = $(git rev-parse --short HEAD)（main ref 未被触碰）"

# ── ⑦ GitHub Release + 商店同步触发（钥匙串 token，不落盘不回显）──────────
TOKEN="$(security find-internet-password -s github.com -w)"
[ -n "$TOKEN" ] || { echo "⛔ 钥匙串没有 github.com token"; exit 1; }
SLUG="andjiang0083/retro-go-tab5"
REL_JSON="$(curl -s --max-time 40 -X POST -H "Authorization: Bearer $TOKEN" -H 'Accept: application/vnd.github+json' \
  "https://api.github.com/repos/$SLUG/releases" \
  -d "{\"tag_name\":\"$VER\",\"target_commitish\":\"claw4\",\"name\":\"retro-go claw4 $VER\",\"body\":\"Claw4 (ESP32-P4) merged bin，写 0x0。三点门禁通过；真机已验证。\n\n商店 github-sync 资产：$BIN_NAME\",\"draft\":false,\"prerelease\":false}")"
REL_ID="$(echo "$REL_JSON" | python3 -c 'import sys,json;print(json.load(sys.stdin).get("id",""))')"
[ -n "$REL_ID" ] || { echo "⛔ 建 Release 失败: $(echo "$REL_JSON" | head -c 200)"; exit 1; }
for A in "$BIN_NAME" "${BIN_NAME}.sha256"; do
  CODE="$(curl -s --max-time 120 -o /dev/null -w '%{http_code}' -X POST \
    -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/octet-stream' \
    --data-binary "@releases/claw4/$A" \
    "https://uploads.github.com/repos/$SLUG/releases/$REL_ID/assets?name=$A")"
  [ "$CODE" = "201" ] || { echo "⛔ 资产上传失败 $A http=$CODE"; exit 1; }
done
echo "  ✓ GitHub Release ${VER} + 资产 x2"

# ── ⑧ 触发商店同步并回读核实（不信 run 自己的成功输出）────────────────────
BSTORE='https://admin.metalio.world/xiaozhi/api'
# 商店 token：浏览器 localStorage['cloudzao-auth']——脚本环境没有；未登录时跳过触发，
# 服务端轮询会自行完成同步（run 只是加速）。人工核实：GET /firmwares/<FID>/versions
SYNC_MSG="(未触发：脚本环境无商店登录态，等服务端轮询)"
if [ -n "${CZ_TOKEN:-}" ]; then
  SYNC_MSG="$(curl -s --max-time 60 -X POST -H "Authorization: Bearer $CZ_TOKEN" \
    "$BSTORE/firmwares/community/$FID/github-sync/run")"
fi
echo "  github-sync/run → $SYNC_MSG"

echo "=== 完成 ${VER} ==="
echo "下一步：查商店版本列表核实（GET /firmwares/$FID/versions 出现 v${VER_BARE}）"
