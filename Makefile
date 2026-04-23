# 1. 编译器与选项
CC      = gcc
CFLAGS  = -Wall -Wextra -g -Iinclude # 包含头文件目录
LIBS    = -lpthread -lcrypto -lmysqlclient  # 链接线程库、加密库和 MySQL 客户端库

# 2. 目录定义
SRC_DIR    = src
INC_DIR    = include
BUILD_DIR  = build
BIN_DIR    = bin

# 3. 源文件定位
# 获取各个模块的 .c 文件
SRCS_COMMON = $(wildcard $(SRC_DIR)/common/*.c)
SRCS_CLIENT = $(wildcard $(SRC_DIR)/client/*.c)
SRCS_SERVER = $(wildcard $(SRC_DIR)/server/*.c)

# 4. 目标文件定位 (将 .c 替换为 .o，并存入 build 目录)
OBJS_COMMON = $(SRCS_COMMON:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)
OBJS_CLIENT = $(SRCS_CLIENT:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)
OBJS_SERVER = $(SRCS_SERVER:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)

# 5. 最终生成的可执行文件名
TARGET_CLIENT = $(BIN_DIR)/client_app
TARGET_SERVER = $(BIN_DIR)/server_app

# 默认执行全部编译
all: $(TARGET_CLIENT) $(TARGET_SERVER)

# 编译客户端：需要客户端自身的 .o 和公共部分的 .o
$(TARGET_CLIENT): $(OBJS_CLIENT) $(OBJS_COMMON)
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)
	@echo "--- Client Build Success :$@---"

# 编译服务端：需要服务端自身的 .o 和公共部分的 .o
$(TARGET_SERVER): $(OBJS_SERVER) $(OBJS_COMMON)
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)
	@echo "--- Server Build Success :$@---"

# 6. 模式规则：如何从 src 中的 .c 生成 build 中的 .o
# $(BUILD_DIR)/%.o 对应 src/%.c
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)  # 自动创建 build 下的子目录
	$(CC) $(CFLAGS) -c $< -o $@

# 7. 清理
clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR)
	@echo "--- Clean Done ---"

.PHONY: all clean
