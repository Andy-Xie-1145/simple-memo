# Simple Memo / 简单备忘录

CLI memo tool in a single C++ file: **title + body**, keyword search, and **local vector semantic search** (llama.cpp `llama-server --embedding` + embeddinggemma-300m, auto-downloaded on first use).

单文件 C++ 命令行备忘录：**标题 + 正文**，关键词搜索与**本地向量语义搜索**（llama.cpp + embeddinggemma-300m，首次自动下载）。

## Commands / 命令

| Command / 命令 | 说明 / Description |
|---|---|
| `add <title> [body...]` | 新增备忘录；正文空行分段，每段独立向量化 / Add a memo; blank lines split paragraphs, each gets its own vector |
| `edit <id>` | 外部编辑器编辑（`$EDITOR > $VISUAL > edit→notepad/nano→vi`），保存后重算向量 / Edit in external editor, vectors recomputed on save |
| `delete <id>` | 删除（`del` / `rm`）/ Delete |
| `list` / `ls` | 列出全部 / List all |
| `show <id> [opts]` | 查看内容：`-f` 上一个活跃、`-m` 渲染 Markdown、`--limit=nL/nP`+`--from=nL/nP` 分页、`--all` 全部（过大自动截断并提示）/ Show: `-f` last active, `-m` markdown, `--limit/--from` paging, `--all` |
| `search <kw...> [--any] [-l]` | 子串搜索（默认 AND，`--any` 切 OR）；默认输出命中行 ±2 行上下文 / Substring search, hit lines with ±2 context |
| `find <query> [-nK] [-l]` | 语义搜索，按段落匹配 Top-K / Semantic search per paragraph |
| `reindex [--all]` | 补算 pending 向量；`--all` 全量重建 / Recompute pending vectors; `--all` rebuilds |
| `update [-f/--force]` | 从 GitHub Release 自更新 / Self-update from GitHub Releases (`-f` reinstall) |
| `config [get/set/unset]` | 交互式配置 / Interactive config |
| `stop` | 停止后台 llama-server / Stop background llama-server |

REPL: run without arguments. 无参数进入 REPL。

## Build / 编译

Windows (MinGW-w64, static, no runtime DLLs / 静态链接，无运行库依赖):

```bash
g++ -static -static-libgcc -static-libstdc++ -Wall -Wextra -O2 -std=c++17 "简单备忘录.cpp" -o simple-memo.exe
```

Linux / macOS:

```bash
g++ -Wall -Wextra -O2 -std=c++17 简单备忘录.cpp -o simple-memo
```

Runtime deps: `curl` (server/download), `tar` or `unzip` (first-run llama.cpp extract). 运行依赖 `curl`、`tar`/`unzip`（首次解压 llama.cpp）。

Or download a prebuilt binary from [Releases](../../releases). 或直接从 [Releases](../../releases) 下载。

## Semantic search / 语义搜索

First `add`/`find` auto-downloads llama.cpp and embeddinggemma-300m (~330 MB via hf-mirror), then keeps a local `llama-server`. Offline, everything except `find` still works; pending vectors are backfilled by `reindex`.

首次 embedding 自动下载 llama.cpp 与 embeddinggemma-300m（约 330 MB，默认 hf-mirror），之后复用本地 server。离线时除 `find` 外均可使用，pending 向量用 `reindex` 补算。

> Downloads need github.com / huggingface.co access (use WARP/proxy in CN). 下载需访问 github.com / huggingface.co，国内建议开 WARP/代理。

## Configuration / 配置

`config` in REPL, or env vars. Priority: env > config.json (`~/.simple_memo/config.json`), conflicts resolved at startup. REPL 内 `config` 或环境变量；优先级 env > config，冲突启动时处理。

| Key / Env | Default | Description |
|---|---|---|
| `hf_endpoint` / `MEMO_HF_ENDPOINT` | `https://hf-mirror.com` | HF 镜像，置空用官方 / HF mirror, empty = official |
| `embed_hf_repo` / `MEMO_EMBED_HF_REPO` | `ggml-org/embeddinggemma-300M-GGUF` | 嵌入模型仓库 / Embedding model repo |
| `embed_hf_file` / `MEMO_EMBED_HF_FILE` | `embeddinggemma-300M-Q8_0.gguf` | 模型文件 / Model file |
| `server_port` / `MEMO_SERVER_PORT` | `8732` | embedding 端口 / Port |
| `llama_server_exe` / `MEMO_LLAMA_SERVER_EXE` | auto-download | llama-server 路径 / Path to llama-server |
| `editor` / `EDITOR` | auto-detect | 外部编辑器 / External editor |
| `language` / `MEMO_LANG` | `zh` | 界面语言 zh/en / UI language |
| — / `MEMO_DATA_DIR` | `~/.simple_memo` | 数据目录 / Data dir |

## Storage / 存储

`~/.simple_memo/`: `memos.jsonl` (one JSON per line, grep-friendly), `vectors.bin` (per-id paragraph vectors), `llama/` (downloaded llama.cpp).

## License / 许可证

- This program / 本程序: [MIT](LICENSE)
- llama.cpp (auto-downloaded): MIT
- embeddinggemma-300m (auto-downloaded): [Google Gemma Terms of Use](https://ai.google.dev/gemma/terms)
