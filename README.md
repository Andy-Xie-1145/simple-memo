# 简单备忘录 (Simple Memo)

[English](README.en.md) | 中文

单文件 C++ 命令行备忘录：**标题 + 正文**，关键词搜索与**本地向量语义搜索**（llama.cpp + embeddinggemma-300m，首次自动下载）。

## 命令

| 命令 | 说明 |
|---|---|
| `add <title> [body...]` | 新增备忘录；正文空行分段，每段独立向量化 |
| `edit <id|标题片段>` | 外部编辑器编辑（`$EDITOR > $VISUAL > edit→notepad / nano→vi`），保存后重算向量 |
| `delete <id|标题片段> [-y]` | 删除，需确认（`-y` 跳过；`del` / `rm`） |
| `list` / `ls` | 列出全部 |
| `show <id|标题片段> [opts]` | 查看内容：`-f` 上一个活跃、`-m` 渲染 Markdown、`--limit=nL/nP` + `--from=nL/nP` 分页、`--all` 全部（过大自动截断并提示） |
| `search <kw...> [--any] [-l]` | 子串搜索（默认 AND，`--any` 切 OR）；默认输出命中行 ±2 行上下文 |
| `find <query> [-nK] [-l]` | 语义搜索，按段落匹配 Top-K |
| `reindex [--all]` | 补算 pending 向量；`--all` 全量重建 |
| `update [-f/--force]` | 从 GitHub Release 自更新（`-f` 强制重装） |
| `config [get/set/unset]` | 交互式配置 |
| `stop` | 停止后台 llama-server |

无参数进入 REPL。

## 编译

Windows（MinGW-w64，静态链接，无运行库依赖）：

```bash
g++ -static -static-libgcc -static-libstdc++ -Wall -Wextra -O2 -std=c++17 simple-memo.cpp -o simple-memo.exe
```

Linux / macOS：

```bash
g++ -Wall -Wextra -O2 -std=c++17 simple-memo.cpp -o simple-memo
```

运行依赖 `curl`（网络请求）、`tar` 或 `unzip`（首次解压 llama.cpp）。

或直接从 [Releases](../../releases) 下载。

## 语义搜索

首次 embedding 自动下载 llama.cpp 与 embeddinggemma-300m（约 330 MB，默认 hf-mirror），之后复用本地 server。离线时除 `find` 外均可使用，pending 向量用 `reindex` 补算。

> 下载需访问 github.com / huggingface.co，国内建议开 WARP/代理。

## 配置

REPL 内 `config`，或用环境变量。优先级：env > config.json（`~/.simple_memo/config.json`），冲突启动时处理。

| Key / 环境变量 | 默认值 | 说明 |
|---|---|---|
| `hf_endpoint` / `MEMO_HF_ENDPOINT` | `https://hf-mirror.com` | HF 镜像，置空用官方 |
| `embed_hf_repo` / `MEMO_EMBED_HF_REPO` | `ggml-org/embeddinggemma-300M-GGUF` | 嵌入模型仓库 |
| `embed_hf_file` / `MEMO_EMBED_HF_FILE` | `embeddinggemma-300M-Q8_0.gguf` | 模型文件 |
| `server_port` / `MEMO_SERVER_PORT` | `8732` | embedding 端口 |
| `llama_server_exe` / `MEMO_LLAMA_SERVER_EXE` | 自动下载 | llama-server 路径 |
| `editor` / `EDITOR` | 自动探测 | 外部编辑器 |
| `language` / `MEMO_LANG` | `zh` | 界面语言 zh/en |
| — / `MEMO_DATA_DIR` | `~/.simple_memo` | 数据目录 |

## 存储

`~/.simple_memo/`：`memos.jsonl`（每行一条 JSON，方便 grep）、`vectors.bin`（按 id 的段落向量）、`llama/`（下载的 llama.cpp）。

## 许可证

- 本程序：[MIT](LICENSE)
- llama.cpp（自动下载）：MIT
- embeddinggemma-300m（自动下载）：[Google Gemma Terms of Use](https://ai.google.dev/gemma/terms)
