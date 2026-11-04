.PHONY: build build-opt test test-v clean demo check-env docker version help asan tsan fmt

BAZEL ?= bazel

help: ## 显示帮助
	@grep -E '^[a-zA-Z_-]+:.*?## .*$$' $(MAKEFILE_LIST) | sort | \
		awk 'BEGIN {FS = ":.*?## "}; {printf "  %-15s %s\n", $$1, $$2}'

build: ## 构建核心库与 CLI
	$(BAZEL) build //src/...

build-opt: ## 构建优化版核心 CLI
	$(BAZEL) build //src/cli:illuminator --config=opt

test: ## 运行核心、插件、存储及 CLI 测试
	$(BAZEL) test //src/... --test_output=errors

test-v: ## 运行测试（详细输出）
	$(BAZEL) test //src/... --test_output=all --test_summary=detailed

demo: build ## 运行 1 秒合成数据 Pipeline，录制到 ./data
	./bazel-bin/src/cli/illuminator demo --config illuminator.yaml.example

clean: ## 清理构建产物
	$(BAZEL) clean

check-env: ## 检测核心构建和运行环境
	@bash scripts/check_env.sh --all

version: ## 显示当前版本
	@tools/workspace_status.sh | grep STABLE_GIT_VERSION | cut -d' ' -f2-

docker: ## 构建核心 CLI 镜像
	docker build \
		--build-arg GIT_VERSION=$$(git describe --tags --always 2>/dev/null | sed 's/^v//') \
		--build-arg GIT_COMMIT=$$(git rev-parse HEAD 2>/dev/null) \
		-t illuminator:$$(git describe --tags --always 2>/dev/null || echo dev) .

asan: ## AddressSanitizer 测试
	$(BAZEL) test //src/... --config=asan --test_output=errors

tsan: ## ThreadSanitizer 测试
	$(BAZEL) test //src/... --config=tsan --test_output=errors

fmt: ## 格式化 C++ 代码
	@find src/ \( -name '*.h' -o -name '*.cc' \) -print0 | xargs -0 clang-format -i
