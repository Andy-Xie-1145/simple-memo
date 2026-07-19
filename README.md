# 简单备忘录 (Simple Memo)

C++ 命令行备忘录：存储 **标题 + 文本**，支持**多关键词搜索**与**向量语义搜索**（经本地 [LM Studio](https://lmstudio.ai/) 的 embedding 接口，底层 llama.cpp）。

## 功能

- **写入/读取**：标题 + 正文，子命令或 REPL 操作
- **多关键词搜索**：默认 AND，`--any` 切 OR；子串匹配、不区分大小写、搜标题+正文（中文友好）
- **语义搜索**：LM Studio 文本嵌入 + 余弦相似度，返回 Top-K 并带相似度百分比
- **lazy embedding**：写入时若 LM Studio 不可用则标记 `pending`，`reindex` 后补算，**写入永不被服务可用性阻塞**

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
请输入正文（独占一行输入 :end 结束）：
...
memo> quit
```

## 接入语义搜索（LM Studio）

1. LM Studio → Developer / Local Server，启动 server（默认端口 `1234`）
2. 下载并加载一个嵌入模型（推荐 `bge-m3`，中文友好；或 `nomic-embed-text-v1.5`）
3. 设置模型名环境变量（与 LM Studio 中显示一致）：

   ```powershell
   $env:MEMO_EMBED_MODEL = "bge-m3"
   ```
4. 运行 `reindex` 给已有备忘录补算向量，之后 `find` 即可返回语义匹配

无 LM Studio 时，`add` / `search` / `list` 等仍正常工作，仅 `find`（语义搜索）不可用。

## 环境变量

| 变量 | 默认值 | 说明 |
|---|---|---|
| `MEMO_EMBED_URL` | `http://localhost:1234/v1/embeddings` | embedding API 地址 |
| `MEMO_EMBED_MODEL` | `text-embedding-nomic-embed-text-v1.5` | 嵌入模型名 |
| `MEMO_DATA_DIR` | `~/.simple_memo` | 数据目录（`memos.jsonl` + `vectors.bin`） |

## 存储

- `memos.jsonl`：一行一条，字段 `id` / `title` / `body` / `ctime` / `vec_ok`，人可读、可 grep
- `vectors.bin`：向量按 id 索引，紧凑二进制
