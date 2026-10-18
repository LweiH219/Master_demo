# Makefile —— ECAT2CAN 主机侧工具包（CMake 的便捷包装）
#
#   make              一次编译主站和 DJI 电机示例（内部调用 CMake）
#   make setcap       给两个程序加 raw socket 权限（需要 sudo 密码）
#   make run ARGS="enp3s0 --motor gm6020 --ch 0 --monitor"
#   make linktest ARGS="can0 enp3s0 40"
#   make install      装到 /usr/local
#   make clean        清理
#
# 本工具包**完全自包含**：SOEM 已内置在 third_party/SOEM，
# 不依赖任何外部路径，拷到任何地方都能构建。
#
# 生成器：优先 ninja，没有则退回 make。

BUILD_DIR  ?= build
BUILD_TYPE ?= RelWithDebInfo
PREFIX     ?= /usr/local

# 优先 ninja
ifeq ($(shell command -v ninja 2>/dev/null),)
  GEN      := Unix Makefiles
  GENFLAG  :=
  BUILDER  := $(MAKE) -C $(BUILD_DIR)
else
  GEN      := Ninja
  GENFLAG  := -GNinja
  BUILDER  := ninja -C $(BUILD_DIR)
endif

TARGETS = ecat2can_master motor_pos
BINS    = $(addprefix $(BUILD_DIR)/,$(TARGETS))

all: $(BINS)
	@echo
	@echo ">> 注意：重新链接会产生新文件，内核会把 capability 清掉。"
	@echo ">> 如果程序报「打不开网卡 / 需要 CAP_NET_RAW」，跑一次："
	@echo ">>     make setcap"
	@echo

$(BINS): $(wildcard src/*.c src/*.h) CMakeLists.txt
	@cmake $(GENFLAG) -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=$(BUILD_TYPE) -DBUILD_DJI_MOTOR_TEST=ON > /dev/null
	@$(BUILDER)

# 让当前用户无需 sudo 就能开 raw socket（每次重新编译后都要跑一次）
#
# 注意：setcap 一次只能给**一个**文件设权限。写成
#     setcap <caps> file1 file2
# 的话，file2 会被当成第二个 caps 字符串去解析，报
#     argument: file2 / fatal error: Invalid argument
# 所以这里必须循环，一个文件一次。用 sh -c 包一层，sudo 只问一次密码。
setcap: $(BINS)
	@sudo sh -c 'for f in $(BINS); do \
	    setcap cap_net_raw,cap_net_admin+eip "$$f" || exit 1; \
	    echo "  ✓ $$f"; \
	done'
	@echo ">> getcap 确认："
	@getcap $(BINS)

run: setcap
	./$(BUILD_DIR)/motor_pos $(ARGS)

linktest:
	./scripts/can-linktest.sh $(ARGS)

install: $(BINS)
	@cmake --install $(BUILD_DIR) --prefix $(PREFIX)
	@echo ">> 已安装到 $(PREFIX)/bin：$(TARGETS)"
	@echo ">> 别忘了给装好的程序加权限："
	@echo ">>   sudo setcap cap_net_raw,cap_net_admin+eip $(PREFIX)/bin/ecat2can_master"

clean:
	rm -rf $(BUILD_DIR)

distclean: clean
	rm -f compile_commands.json

.PHONY: all setcap run linktest install clean distclean
