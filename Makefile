# ---------------------------------------------------------------------------
# graph_database_server —— 一键整体编译（只需 g++ / clang++ + make，无需 cmake）
#
#   make           编译全部目标到 bin/
#   make run       编译并启动图数据库服务（127.0.0.1:8080，数据目录 data/）
#   make test      编译后运行冒烟测试（JSON 自测 + API 回归 + 边权专项）
#   make bridge    编译并启动控制台程序 HTTP 桥接（桥接 bin/calc_worker，端口 8081）
#   make smoke     编译后运行桥接端到端冒烟测试
#   make clean     清理构建产物
#
# 说明：本仓库由 minigraph（图数据库内核）、cpp_http_bridge（HTTP 层）、
#       graph_server（业务层）三个项目合并而来，三者共用同一套 include/
#       与 src/ 目录，由本 Makefile 统一编译，不再各自独立构建。
# ---------------------------------------------------------------------------

CXX      ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra
LDFLAGS  ?= -pthread
INCLUDES := -Iinclude

BINDIR   := bin
OBJDIR   := build/obj

# ---- 源码分组（三层） -------------------------------------------------------
MINIGRAPH_SRCS := src/minigraph/Value.cpp \
                  src/minigraph/Storage.cpp \
                  src/minigraph/GraphDB.cpp

HTTP_SRCS      := src/http/http_server.cpp \
                  src/http/child_process.cpp

SERVICE_SRCS   := src/graph_server/JsonValue.cpp \
                  src/graph_server/GraphService.cpp

# 各可执行文件所需的源码
GRAPH_SERVER_SRCS := src/graph_server_main.cpp $(SERVICE_SRCS) src/http/http_server.cpp $(MINIGRAPH_SRCS)
BRIDGE_SRCS       := src/cpp_bridge_main.cpp $(HTTP_SRCS)
DEMO_SRCS         := examples/minigraph_demo.cpp $(MINIGRAPH_SRCS)
EMBEDDED_SRCS     := examples/embedded_server.cpp src/http/http_server.cpp

# 把一组 .cpp 映射到 build/obj 下的 .o
objs-for = $(addprefix $(OBJDIR)/,$(1:.cpp=.o))

ALL_SRCS := $(sort $(GRAPH_SERVER_SRCS) $(BRIDGE_SRCS) $(DEMO_SRCS) \
                   $(EMBEDDED_SRCS) examples/echo_worker.cpp \
                   examples/calc_worker.cpp tests/test_json.cpp)
ALL_OBJS := $(call objs-for,$(ALL_SRCS))

TARGETS := $(BINDIR)/graph_server \
           $(BINDIR)/cpp_bridge \
           $(BINDIR)/minigraph_demo \
           $(BINDIR)/echo_worker \
           $(BINDIR)/calc_worker \
           $(BINDIR)/embedded_server \
           $(BINDIR)/json_tests

.PHONY: all dirs run bridge test smoke clean

all: dirs $(TARGETS)
	@echo "==> 全部目标编译完成，产物见 $(BINDIR)/"

dirs:
	@mkdir -p $(BINDIR) data

# ---- 编译规则：目标文件按源目录结构镜像到 build/obj -------------------------
$(OBJDIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

# ---- 链接规则 ---------------------------------------------------------------
$(BINDIR)/graph_server: $(call objs-for,$(GRAPH_SERVER_SRCS))
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

$(BINDIR)/cpp_bridge: $(call objs-for,$(BRIDGE_SRCS))
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

$(BINDIR)/minigraph_demo: $(call objs-for,$(DEMO_SRCS))
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

$(BINDIR)/embedded_server: $(call objs-for,$(EMBEDDED_SRCS))
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

$(BINDIR)/echo_worker: $(call objs-for,examples/echo_worker.cpp)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

$(BINDIR)/calc_worker: $(call objs-for,examples/calc_worker.cpp)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

$(BINDIR)/json_tests: $(call objs-for,tests/test_json.cpp)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

# ---- 运行 / 测试 ------------------------------------------------------------
run: $(BINDIR)/graph_server
	@mkdir -p data
	./$(BINDIR)/graph_server --host 127.0.0.1 --port 8080 --dir data --web web/index.html

bridge: $(BINDIR)/cpp_bridge $(BINDIR)/calc_worker
	./$(BINDIR)/cpp_bridge --port 8081 --cmd ./$(BINDIR)/calc_worker --cwd .

test: all
	@$(BINDIR)/json_tests
	@bash scripts/api_test.sh
	@bash scripts/weight_test.sh

smoke: all
	@bash scripts/smoke_test.sh

clean:
	rm -rf $(OBJDIR) $(BINDIR) build
