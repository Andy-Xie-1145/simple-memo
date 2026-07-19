# 简单备忘录 (Simple Memo)

C++ 命令行备忘录：存储 **标题 + 文本**，支持**多关键词搜索**与**向量语义搜索**（经本地 llama.cpp 的 `llama-server --embedding` + embeddinggemma-300m，**首次自动下载**，无需 LM Studio / Ollama）。

## 功能

- **写入/读取**：标题 + 正文，子命令或 REPL 操作
- **多关键词搜索**：默认 AND，`--any` 切 OR；子串匹配、不区分大小写、搜标题+正文（中文友好）
- **语义搜索**：embeddinggemma-300m 嵌入 + 余弦相似度，返回 Top-K 并带相似度百分比
- **首次自动下载**：首次 embedding 自动下载 llama.cpp（`llama-server.exe`）+ embeddinggemma-300m 模型，开箱即用
- **lazy + 常驻 server**：首次 `add`/`find` 启动本地 llama-server（加载模型），之后复用；程序退出自动清理。服务不可用时写入标 pending，`reindex` 补算

## 编译

需要 MinGW g++（C++17）：

```bash
g++.exe -Wall -Wextra -g3 -O2 -std=c++17 "简单备忘录.cpp" -o "output/简单备忘录.exe"
```

## 使用

子命令模式：

```bash
./output/简单备忘录.exe add "购物清单" "牛奶 面包 鸡蛋"
./output/简单备忘录.exe list
./output/简单备忘录.exe search 牛奶 鸡蛋         # AND：全命中
./output/简单备忘录.exe search 牛奶 --any        # OR：任一命中
./output/简单备忘录.exe find "今晚做什么菜" -n5   # 语义搜索
./output/简单备忘录.exe show 1
./output/简单备忘录.exe edit 1 "新标题" "新正文"
./output/简单备忘录.exe delete 1
./output/简单备忘录.exe reindex                  # 补算 pending 向量
```

REPL 模式（无参数进入）：

```
$ ./output/简单备忘录.exe
简单备忘录 REPL（输入 help 查看，quit 退出）
memo> add 标题
请输入正文（单独一行输入 :end 或 end 结束；或按 Ctrl+Z 回车结束）：
...
memo> quit
```

## 语义搜索（首次自动下载，无需 LM Studio）

首次 `add`/`find` 触发 embedding 时，程序自动：

1. 从 [llama.cpp release](https://github.com/ggml-org/llama.cpp/releases) 下载 Windows 预编译包，解压 `llama-server.exe` 到 `~/.simple_memo/llama/`
2. 启动本地 `llama-server --embedding`，首次自动从 HuggingFace 下载 [`embeddinggemma-300M-Q8_0.gguf`](https://huggingface.co/ggml-org/embeddinggemma-300M-GGUF)（329 MB，默认走 hf-mirror 加速）
3. 之后所有 `add`/`find` 复用该 server，程序退出自动清理

> ⚠️ 下载需访问 github.com 与 huggingface，国内建议开 WARP/代理。
>
> ⚠️ llama.cpp 新版 Windows 包不含独立的 `llama-embedding`，embedding 经 `llama-server --embedding` 提供（OpenAI 兼容 `/v1/embeddings`）。

无网络时 `add`/`search`/`list` 等仍正常工作，仅 `find`（语义搜索）不可用，写入标 pending。

## 环境变量

| 变量 | 默认 | 说明 |
|---|---|---|
| `MEMO_DATA_DIR` | `~/.simple_memo` | 数据/缓存目录 |
| `MEMO_LLAMA_SERVER_EXE` | （自动下载） | 已有 llama-server.exe 路径，跳过下载 |
| `MEMO_SERVER_PORT` | `8732` | 本地 embedding server 端口 |
| `MEMO_EMBED_HF_REPO` | `ggml-org/embeddinggemma-300M-GGUF` | 嵌入模型 HF repo |
| `MEMO_EMBED_HF_FILE` | `embeddinggemma-300M-Q8_0.gguf` | 嵌入模型文件 |
| `MEMO_HF_ENDPOINT` | `https://hf-mirror.com` | HF 镜像（国内加速；置空用官方） |

## 存储

- `memos.jsonl`：一行一条，字段 `id` / `title` / `body` / `ctime` / `vec_ok`，人可读、可 grep
- `vectors.bin`：向量按 id 索引，紧凑二进制
- `llama/`：自动下载的 llama.cpp（`llama-server.exe` + 依赖 dll）
