.PHONY: build test clean dev probes check-env docker version help

BAZEL ?= bazel

help: ## 显示帮助
	@grep -E '^[a-zA-Z_-]+:.*?## .*$$' $(MAKEFILE_LIST) | sort | \
		awk 'BEGIN {FS = ":.*?## "}; {printf "  \033[36m%-15s\033[0m %s\n", $$1, $$2}'

build: ## 构建后端 (debug)
	$(BAZEL) build //src/...

build-opt: ## 构建后端 (optimized)
	$(BAZEL) build //src/cli:illuminator --config=opt

test: ## 运行全部单元测试
	$(BAZEL) test //src/... --test_output=errors

test-v: ## 运行测试 (详细输出)
	$(BAZEL) test //src/... --test_output=all --test_summary=detailed

probes: ## 编译 eBPF 探针
	$(BAZEL) build //src/ebpf/probes:all

clean: ## 清理构建产物
	$(BAZEL) clean

dev: build ## 启动开发模式 (后端)
	./bazel-bin/src/cli/illuminator daemon --config illuminator.yaml

dev-web: ## 启动前端开发服务器
	cd web && npm run dev

check-env: ## 检测编译/运行环境
	@bash scripts/check_env.sh --all

version: ## 显示当前版本
	@tools/workspace_status.sh | grep STABLE_GIT_VERSION | cut -d' ' -f2-

docker: ## 构建 Docker 镜像 (本地，自动注入版本)
	docker build \
		--build-arg GIT_VERSION=$$(git describe --tags --always 2>/dev/null | sed 's/^v//') \
		--build-arg GIT_COMMIT=$$(git rev-parse HEAD 2>/dev/null) \
		-t illuminator:$$(git describe --tags --always 2>/dev/null || echo dev) .

asan: ## 运行 AddressSanitizer 测试
	$(BAZEL) test //src/... --config=asan --test_output=errors

tsan: ## 运行 ThreadSanitizer 测试
	$(BAZEL) test //src/... --config=tsan --test_output=errors

fmt: ## 格式化代码 (需要 clang-format)
	@find src/ -name '*.h' -o -name '*.cc' | xargs clang-format -i 2>/dev/null || \
		echo "clang-format not found, skipping"
