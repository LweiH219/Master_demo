# Makefile —— ECAT2CAN 主机侧工具（独立工程）
#
#   ecat2can_master  协议/链路测试工具（info / loop / scan / work / workall / fdb / reset）
#   motor_pos        电机位置闭环上位机（M3508+C620 / GM6020）
#
# 这是一个**独立**的 Linux 主机侧工程：不依赖任何具体固件 demo 目录，
# 只要从站跑的是港科大 ECAT2CAN 的 192 字节 PDO 协议即可通用。
#   - hpm5e3ykit 单路版  → 只有通道 3
#   - ECAN-Lite / 港科广 5e3y 四路版 → 通道 0~3（CN4=0 CN5=1 CN9=2 CN1=3）
#
# 常用:
#   make                      编译
#   make setcap               给两个程序加 raw socket 权限（需要 sudo 密码）
#   make run ARGS="enp3s0 --motor gm6020 --ch 0 --monitor"
#                             加权限并直接运行 motor_pos
#
# SOEM 依赖（默认 ~/HPM/tools/SOEM，换别处就覆盖：make SOEM_DIR=/path/to/SOEM）

CC       ?= gcc
CFLAGS   ?= -O2 -Wall -Wextra -g
CFLAGS   += -std=gnu11

SOEM_DIR   ?= $(HOME)/HPM/tools/SOEM
SOEM_BUILD ?= $(SOEM_DIR)/build

CFLAGS   += -I$(SOEM_DIR)/include
# ec_options.h 是 CMake 配置阶段生成的，在 build/include 下
CFLAGS   += -I$(SOEM_BUILD)/include
# SOEM 2.0 的 osal / oshw 头文件不在 include/ 下，需要单独加
CFLAGS   += -I$(SOEM_DIR)/osal -I$(SOEM_DIR)/osal/linux
CFLAGS   += -I$(SOEM_DIR)/oshw/linux

SOEM_LIB  = $(SOEM_BUILD)/libsoem.a
LDLIBS    = $(SOEM_LIB) -lpthread -lrt -lm

TARGETS   = ecat2can_master motor_pos

all: $(TARGETS)
	@echo
	@echo ">> 注意：重新链接会产生新文件，内核会把 capability 清掉。"
	@echo ">> 如果程序报「打不开网卡 / 需要 CAP_NET_RAW」，跑一次："
	@echo ">>     make setcap"
	@echo

ecat2can_master: ecat2can_master.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

motor_pos: motor_pos.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

%.o: %.c ecat2can_proto.h
	$(CC) $(CFLAGS) -c -o $@ $<

# 让当前用户无需 sudo 就能开 raw socket（每次重新编译后都要跑一次）
#
# 注意：setcap 一次只能给**一个**文件设权限。写成
#     setcap <caps> file1 file2
# 的话，file2 会被当成第二个 caps 字符串去解析，报
#     argument: file2
#     fatal error: Invalid argument
# 所以这里必须循环，一个文件一次。用 sh -c 包一层，sudo 只问一次密码。
setcap: $(TARGETS)
	@sudo sh -c 'for f in $(TARGETS); do \
	    setcap cap_net_raw,cap_net_admin+eip "$$f" || exit 1; \
	    echo "  ✓ $$f"; \
	done'
	@echo ">> getcap 确认："
	@getcap $(TARGETS)

# 加权限后直接运行 motor_pos，例：
#   make run ARGS="enp3s0 --motor gm6020 --id 1 --monitor"
run: setcap
	./motor_pos $(ARGS)

clean:
	rm -f *.o $(TARGETS)

.PHONY: all clean setcap run
