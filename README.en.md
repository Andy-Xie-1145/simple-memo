# Simple Memo (简单备忘录)

English | [中文](README.md)

CLI memo tool in a single C++ file: **title + body**, keyword search, and **local vector semantic search** (llama.cpp + embeddinggemma-300m, auto-downloaded on first use).

## Commands

| Command | Description |
|---|---|
| `add <title...> [-- body...]` | Add a memo; title may contain spaces (separate body with `--` or quote the title); blank lines split paragraphs, each gets its own vector |
| `edit <id|title>` | Edit in external editor (`$EDITOR > $VISUAL > edit→notepad / nano→vi`), vectors recomputed on save |
| `delete <id|title> [-y]` | Delete with confirmation (`-y` skips; `del` / `rm`) |
| `list` / `ls` | List all |
| `show <id|title> [opts]` | Show: `-f` last active, `-m` markdown rendering, `--limit=nL/nP` + `--from=nL/nP` paging, `--all` everything (oversized memos auto-truncated with notice) |
| `search <kw...> [--any] [-l]` | Substring search (AND by default, `--any` for OR); prints hit lines with ±2 context |
| `find <query> [-nK] [-l]` | Semantic search, per-paragraph Top-K |
| `reindex [--all]` | Backfill pending vectors; `--all` rebuilds from scratch |
| `update [-f/--force]` | Self-update from GitHub Releases (`-f` reinstall) |
| `config [get/set/unset]` | Interactive configuration |
| `stop` | Stop the background llama-server |

Run without arguments to enter the REPL.

## Build

Windows (MinGW-w64, static linking, no runtime DLLs):

```bash
g++ -static -static-libgcc -static-libstdc++ -Wall -Wextra -O2 -std=c++17 simple-memo.cpp -o simple-memo.exe
```

Linux / macOS:

```bash
g++ -Wall -Wextra -O2 -std=c++17 simple-memo.cpp -o simple-memo
```

Runtime deps: `curl` (network), `tar` or `unzip` (first-run llama.cpp extract).

Or download a prebuilt binary from [Releases](../../releases).

## Semantic search

The first `add`/`find` auto-downloads llama.cpp and embeddinggemma-300m (~330 MB via hf-mirror by default), then keeps a local `llama-server`. Offline, everything except `find` still works; pending vectors are backfilled by `reindex`.

> Downloads need github.com / huggingface.co access (use WARP/proxy in CN).

## Configuration

`config` in the REPL, or env vars. Priority: env > config.json (`~/.simple_memo/config.json`); conflicts are resolved at startup.

| Key / Env | Default | Description |
|---|---|---|
| `hf_endpoint` / `MEMO_HF_ENDPOINT` | `https://hf-mirror.com` | HF mirror, empty = official |
| `embed_hf_repo` / `MEMO_EMBED_HF_REPO` | `ggml-org/embeddinggemma-300M-GGUF` | Embedding model repo |
| `embed_hf_file` / `MEMO_EMBED_HF_FILE` | `embeddinggemma-300M-Q8_0.gguf` | Model file |
| `server_port` / `MEMO_SERVER_PORT` | `8732` | Embedding server port |
| `llama_server_exe` / `MEMO_LLAMA_SERVER_EXE` | auto-download | Path to llama-server |
| `editor` / `EDITOR` | auto-detect | External editor |
| `language` / `MEMO_LANG` | `zh` | UI language zh/en |
| — / `MEMO_DATA_DIR` | `~/.simple_memo` | Data dir |

## Storage

`~/.simple_memo/`: `memos.jsonl` (one JSON per line, grep-friendly), `vectors.bin` (per-id paragraph vectors), `llama/` (downloaded llama.cpp).

## License

- This program: [MIT](LICENSE)
- llama.cpp (auto-downloaded): MIT
- embeddinggemma-300m (auto-downloaded): [Google Gemma Terms of Use](https://ai.google.dev/gemma/terms)
