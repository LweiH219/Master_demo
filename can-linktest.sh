#!/usr/bin/env bash
# -----------------------------------------------------------------------------
#  can-linktest.sh —— CAN 链路自测（PCAN 发帧 → ECAN-Lite 网关收帧）
#
#  用法:  tools/can-linktest.sh [can0] [enp3s0] [帧数]
#         参数1 = PCAN 的 SocketCAN 接口名（默认 can0）
#         参数2 = EtherCAT 网卡名（默认 enp3s0）
#         参数3 = 发多少帧（默认 40）
#
#  它做两件事：
#    1) 在后台让 PCAN 以 25ms 间隔连发 N 帧（ID 0x123，8 字节递增数据）
#    2) 同时让网关进 ONLY_FDB 模式监听，打印收到的帧
#
#  判读：
#    看到 "CAN反馈 ch=? id=0x123 ..."  → 该通道链路正常
#    一帧都没有                      → 该通道的接线/收发器/终端电阻有问题
#
#  注意：网关四路全开，所以不管 PCAN 插在哪个座上都能收到，
#        输出里的 ch=N 就告诉你它实际走的是哪一路（0=CN4 1=CN5 2=CN9 3=CN1）。
# -----------------------------------------------------------------------------
set -u

CANIF="${1:-can0}"
ETHIF="${2:-enp3s0}"
NFrames="${3:-40}"

# 主站工具就在本脚本同级目录（独立工程，不再依赖固件 demo 路径）
# 用 readlink -f 解析符号链接：这样从 ~/HPM/tools/ 的软链调用时，
# HERE 仍指向脚本真实所在的 Master_demo/，才能找到 ecat2can_master
HERE="$(cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")" && pwd)"
MASTER="${ECAT2CAN_MASTER:-$HERE/ecat2can_master}"

[ -x "$MASTER" ] || { echo "找不到 $MASTER，先 make" >&2; exit 2; }

echo "== 自检 =="
if ! ip link show "$CANIF" >/dev/null 2>&1; then
    echo "!! 没有 $CANIF 接口。PCAN 插好了吗？驱动加载了吗？" >&2
    exit 2
fi
# 注意：ip -details 的 "state UP/DOWN" 是**网卡层**状态，
#       CAN 层状态要抓 "can state XXXX"，两者不是一回事（踩过）。
flags=$(ip -br link show "$CANIF" | awk '{print $2}')
cstate=$(ip -details link show "$CANIF" | grep -o 'can state [A-Z-]*' | head -1 | awk '{print $3}')
echo "  $CANIF : 网卡=$flags  CAN层=${cstate:-未知}"

if ! printf '%s' "$flags" | grep -q 'UP'; then
    echo "!! $CANIF 是 DOWN 的（PCAN 重新插拔后会掉回 DOWN）。先跑：" >&2
    echo "   sudo ip link set $CANIF type can bitrate 1000000 sample-point 0.75" >&2
    echo "   sudo ip link set $CANIF up" >&2
    exit 2
fi
case "$cstate" in
    ERROR-ACTIVE|ERROR-WARNING) ;;
    BUS-OFF) echo "!! BUS-OFF，先：sudo ip link set $CANIF down && sudo ip link set $CANIF up" >&2; exit 2 ;;
    STOPPED) echo "!! 网卡已 up 但 CAN 层 STOPPED，重新配一次 bitrate" >&2; exit 2 ;;
esac

rc0=$(ip -statistics link show "$CANIF" | awk '/TX:/{getline; print $2}')
echo "  起始 TX 字节数 = ${rc0:-0}"
echo

echo "== 开始：PCAN 发 $NFrames 帧，网关同时监听 =="
# 逐帧统计成功/失败：cansend 在"发不出去"（队列满/无 ACK 导致重传堆积）时返回非 0。
# 接收端 ACK 正常时几乎不应该失败 —— 失败率高就是链路质量问题的直接证据。
SENDFILE=$(mktemp)
( ok=0; fail=0
  for i in $(seq 1 "$NFrames"); do
      if cansend "$CANIF" "123#1122334455667788" 2>/dev/null; then
          ok=$((ok+1))
      else
          fail=$((fail+1))
      fi
      sleep 0.025
  done
  echo "$ok $fail" > "$SENDFILE"
) &
SENDER=$!

# 网关侧全程记录到文件，稍后统计"到底收到了几帧我们的 0x123"
GWLOG=$(mktemp)
timeout 30 stdbuf -oL "$MASTER" "$ETHIF" fdb 12000 > "$GWLOG" 2>&1 &
GWPID=$!

wait $SENDER 2>/dev/null
wait $GWPID 2>/dev/null

grep -E "扫到|从站已进 OP|CAN反馈|error_flags=0x0[1-9]" "$GWLOG" | head -20

# ★ 关键统计：网关收到的、ID 为 0x123 的帧数（这才是链路真的通了的证据）
# 注意：grep -c 在无匹配时也会打印 0，但退出码是 1 ——
# 所以这里只能用 || true 吞掉退出码，不能 || echo 0（会多出一行）
GOT=$(grep -c "id=0x123" "$GWLOG" 2>/dev/null || true)
[ -z "$GOT" ] && GOT=0
GOT=$(printf '%s' "$GOT" | head -1)
CH=$(grep -o "CAN反馈 ch=[0-9]* id=0x123" "$GWLOG" 2>/dev/null | head -1 | grep -o "ch=[0-9]*" || true)
[ -z "$CH" ] && CH="ch=?"

rc1=$(ip -statistics link show "$CANIF" | awk '/TX:/{getline; print $2}')
read -r SOK SFAIL < "$SENDFILE" 2>/dev/null || { SOK="?"; SFAIL="?"; }
rm -f "$SENDFILE"
echo
echo "════════════════════════════════════════════════════════════"
echo "  测试结果"
echo "════════════════════════════════════════════════════════════"
echo "  PCAN 发出      : $SOK 成功 / $SFAIL 失败  (共 $NFrames 帧)"
echo "  网关实收 0x123 : $GOT 帧   ${CH}"
echo
if [ "${GOT:-0}" -gt 0 ]; then
    echo "  ✅ 链路正常 —— PCAN 的帧确实到达了网关并被上报"
    echo "     通道号见上面的 ${CH}，这就是 PCAN 实际所在的那一路"
    rc=0
else
    echo "  ❌ 链路不通 —— 网关一帧 0x123 都没收到"
    echo
    echo "  ⚠ 注意：上面 'cansend 成功 $SOK' 并不能说明链路通！"
    echo "     cansend 返回 0 只表示帧进了发送队列，不代表总线上有人 ACK。"
    echo "     唯一有效的判据是第二行的『网关实收』。"
    echo
    echo "  请依次检查："
    echo "    1) 断电量 CANH-CANL 电阻：开路=两端都没终端电阻（1Mbps 下基本通不了）"
    echo "    2) 上电量 CANH/CANL 对 GND：健康时应各约 2.5V；"
    echo "       若一边 3.5V、一边 1.5V，说明该路被持续驱成显性（收发器故障/焊接桥连）"
    echo "    3) 换一个座子试（ECAN-Lite 四路里已知 CN4/CN9/CN1 是好的，CN5 有问题）"
    rc=1
fi
echo "════════════════════════════════════════════════════════════"
rm -f "$GWLOG"
exit $rc
