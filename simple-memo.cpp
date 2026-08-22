// 简单备忘录：标题+文本，多关键词搜索 + 向量语义搜索
// 嵌入由本地 llama.cpp（llama-embedding 子进程 + embeddinggemma-300m）提供，首次自动下载
// 编译：g++ -static -static-libgcc -static-libstdc++ -Wall -Wextra -O2 -std=c++17 simple-memo.cpp -o simple-memo.exe

// 显式标准头（不依赖 GCC 专属的 bits/stdc++.h，clang/libc++ 也可编译）
#include <algorithm>
#include <array>
#include <cctype>
#include <climits>
#include <cmath>
#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <filesystem>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <clocale>
#include <unistd.h>   // getpid / usleep / readlink
#include <sys/types.h>
#include <signal.h>   // kill（PID 存活检测）
#include <cerrno>     // errno（EPERM）
#ifdef __APPLE__
#include <mach-o/dyld.h> // _NSGetExecutablePath
#endif
#endif

using namespace std;
using ll = long long;

// ===================== 配置 =====================
static string get_env(const char* name, const string& def)
{
	const char* v = getenv(name);
	return (v && *v) ? string(v) : def;
}

// 数据目录：环境变量 > ~/.simple_memo
static string data_dir()
{
	string d = get_env("MEMO_DATA_DIR", "");
	if (!d.empty()) return d;
	const char* up = getenv("USERPROFILE");
	if (!up || !*up) up = getenv("HOME");
	if (!up || !*up) up = ".";
	return string(up) + "/.simple_memo";
}

static string memos_path() { return data_dir() + "/memos.jsonl"; }
static string vectors_path() { return data_dir() + "/vectors.bin"; }
static string llama_dir() { return data_dir() + "/llama"; }
// 平台空设备（shell 重定向用）
#ifdef _WIN32
static const char* NULL_DEV = "nul";
#else
static const char* NULL_DEV = "/dev/null";
#endif
// llama-server 二进制名：Win 带 .exe，Unix 无后缀
static string llama_server_bin_name()
{
#ifdef _WIN32
	return "llama-server.exe";
#else
	return "llama-server";
#endif
}
static string llama_server_default() { return llama_dir() + "/" + llama_server_bin_name(); }

// 配置持久化（config.json）：前置声明，实现在下方（依赖 trim / ensure_data_dir）
static string config_file();
static map<string, string> load_config_map();
static string get_effective(const string& key, const string& env_name, const string& def);

// llama-server.exe 路径：环境变量 / config > 默认下载位置
static string llama_server_path()
{
	return get_effective("llama_server_exe", "MEMO_LLAMA_SERVER_EXE", llama_server_default());
}
// 本地 embedding server 端口
static int server_port() { return atoi(get_effective("server_port", "MEMO_SERVER_PORT", "8732").c_str()); }
// 嵌入模型（embeddinggemma-300m Q8_0，官方 GGUF）
static string embed_hf_repo() { return get_effective("embed_hf_repo", "MEMO_EMBED_HF_REPO", "ggml-org/embeddinggemma-300M-GGUF"); }
static string embed_hf_file() { return get_effective("embed_hf_file", "MEMO_EMBED_HF_FILE", "embeddinggemma-300M-Q8_0.gguf"); }
// HF 镜像（国内加速；置空用官方）
static string hf_endpoint() { return get_effective("hf_endpoint", "MEMO_HF_ENDPOINT", "https://hf-mirror.com"); }

static bool ensure_data_dir()
{
	error_code ec;
	filesystem::create_directories(data_dir(), ec);
	return !ec;
}

// ===================== 配置持久化（config.json） =====================
static string trim(const string& s)
{
	size_t a = s.find_first_not_of(" \t\r\n");
	if (a == string::npos) return "";
	size_t b = s.find_last_not_of(" \t\r\n");
	return s.substr(a, b - a + 1);
}

// 大小写不敏感：仅对 ASCII 字母生效，UTF-8 中文字节(>=0x80)在 "C" locale 下保持不变
static string lower(const string& s)
{
	string r;
	r.reserve(s.size());
	for (unsigned char c : s) r += (char)tolower(c);
	return r;
}

// 原子写：先写 <path>.tmp 再 rename 替换 → 进程中途被杀不会留下半截文件
// rename 同目录下原子（POSIX 保证；Win 的 MoveFileEx 也原子）
static bool write_file_atomic(const string& path, const string& content)
{
	string tmp = path + ".tmp";
	{
		ofstream f(tmp, ios::binary | ios::trunc);
		if (!f) return false;
		f.write(content.data(), (streamsize)content.size());
		if (!f.good()) { f.close(); error_code ec; filesystem::remove(tmp, ec); return false; }
	}
	error_code ec;
	// Windows 上 rename 到已存在文件会失败 → 先删目标（此刻起短暂窗口，可接受：tmp 已完整）
	filesystem::remove(path, ec);
	filesystem::rename(tmp, path, ec);
	if (ec) { filesystem::remove(tmp, ec); return false; }
	return true;
}

// shell 参数转义：包引号，内部 " → \"，\ → \\（防路径含引号/特殊字符炸 system 命令行）
static string shell_quote(const string& s)
{
	string r = "\"";
	for (char c : s)
	{
		if (c == '"' || c == '\\') r += '\\';
		r += c;
	}
	return r + "\"";
}

static string config_file() { return data_dir() + "/config.json"; }
static const char* IGNORE_ENV_KEY = "@ignore_env";
static int g_lang_cache = -1; // i18n 语言缓存（定义见 i18n 节；save 后失效）

// 读 config.json：每行 key=value（value 保留 = 之后的全部，含空格）；# 开头为注释
static map<string, string> load_config_map()
{
	map<string, string> m;
	ifstream f(config_file());
	if (!f) return m;
	string line;
	while (getline(f, line))
	{
		while (!line.empty() && line.back() == '\r') line.pop_back();
		if (line.empty() || line[0] == '#') continue;
		size_t eq = line.find('=');
		if (eq == string::npos) continue;
		string k = trim(line.substr(0, eq));
		if (!k.empty()) m[k] = trim(line.substr(eq + 1));
	}
	return m;
}

static void save_config_map(const map<string, string>& m)
{
	ensure_data_dir();
	ofstream f(config_file(), ios::trunc);
	for (auto& kv : m) f << kv.first << "=" << kv.second << "\n";
	g_lang_cache = -1; // 语言等配置变更 → 缓存失效，立即生效
}

// key 是否出现在 @ignore_env 列表（逗号分隔）：表示该项 config 优先于环境变量
static bool cfg_ignores_env(const map<string, string>& m, const string& key)
{
	auto it = m.find(IGNORE_ENV_KEY);
	if (it == m.end() || it->second.empty()) return false;
	string s = it->second + ",";
	size_t pos;
	while ((pos = s.find(',')) != string::npos)
	{
		if (trim(s.substr(0, pos)) == key) return true;
		s.erase(0, pos + 1);
	}
	return false;
}

// 生效值：默认环境变量优先；若标记 @ignore_env 则该 key 的 config 优先
static string get_effective(const string& key, const string& env_name, const string& def)
{
	auto m = load_config_map();
	auto it = m.find(key);
	string cfg = (it != m.end()) ? it->second : "";
	bool ignored = cfg_ignores_env(m, key);
	const char* e = env_name.empty() ? nullptr : getenv(env_name.c_str());
	string env = (e && *e) ? e : "";
	if (ignored && !cfg.empty()) return cfg;
	if (!env.empty()) return env;
	if (!cfg.empty()) return cfg;
	return def;
}

// ===================== i18n / 版本 =====================
// 本程序版本（与 GitHub Release tag 对应，update 子命令据此比较）
static const char* MEMO_VERSION = "v0.2.1";
static const char* MEMO_REPO = "Andy-Xie-1145/simple-memo";

// 界面语言：config language / 环境变量 MEMO_LANG（zh|en，默认 zh）。
// 结果缓存（g_lang_cache 定义于上方 config 节）；save_config_map 时失效 → REPL 内改语言立即生效。
static bool lang_is_en()
{
	if (g_lang_cache < 0)
	{
		string l = lower(get_effective("language", "MEMO_LANG", "zh"));
		g_lang_cache = (l == "en" || l == "english") ? 1 : 0;
	}
	return g_lang_cache == 1;
}
// 双语文案：tr("中文", "English")
static const char* tr(const char* zh, const char* en) { return lang_is_en() ? en : zh; }

// 配置项元数据（供 config 引导式交互 / get 显示）
struct CfgItem { const char* key; const char* env; const char* desc; const char* desc_en; string(*def)(); };
static string cfg_def_hf_endpoint() { return "https://hf-mirror.com"; }
static string cfg_def_embed_repo() { return "ggml-org/embeddinggemma-300M-GGUF"; }
static string cfg_def_embed_file() { return "embeddinggemma-300M-Q8_0.gguf"; }
static string cfg_def_port() { return "8732"; }
static string cfg_def_llama() { return llama_server_default(); }
static string cfg_def_editor() { return ""; }
static string cfg_def_lang() { return "zh"; }
static const CfgItem g_cfg_items[] = {
	{ "hf_endpoint", "MEMO_HF_ENDPOINT", "HF 镜像地址（国内加速，置空用官方）", "HF mirror endpoint (empty = official)", cfg_def_hf_endpoint },
	{ "embed_hf_repo", "MEMO_EMBED_HF_REPO", "嵌入模型 HF 仓库", "Embedding model HF repo", cfg_def_embed_repo },
	{ "embed_hf_file", "MEMO_EMBED_HF_FILE", "嵌入模型 GGUF 文件名", "Embedding model GGUF file name", cfg_def_embed_file },
	{ "server_port", "MEMO_SERVER_PORT", "本地 embedding 服务端口", "Local embedding server port", cfg_def_port },
	{ "llama_server_exe", "MEMO_LLAMA_SERVER_EXE", "llama-server.exe 路径", "Path to llama-server.exe", cfg_def_llama },
	{ "editor", "EDITOR", "外部编辑器命令（可带参数，空=自动探测：Win edit→notepad / Unix nano→vi）", "External editor command (args allowed; empty = auto-detect: Win edit→notepad / Unix nano→vi)", cfg_def_editor },
	{ "language", "MEMO_LANG", "界面语言（zh 中文 / en English）", "UI language (zh Chinese / en English)", cfg_def_lang },
};

static const CfgItem* cfg_find(const string& key)
{
	for (auto& it : g_cfg_items) if (key == it.key) return &it;
	return nullptr;
}

// 增删 @ignore_env 列表中的 key（on=true 加入，on=false 移除）
static void cfg_set_ignore_env(map<string, string>& m, const string& key, bool on)
{
	string cur = (m.find(IGNORE_ENV_KEY) != m.end()) ? m[IGNORE_ENV_KEY] : "";
	vector<string> v;
	string s = cur + ",";
	size_t pos;
	while ((pos = s.find(',')) != string::npos)
	{
		string tok = trim(s.substr(0, pos));
		s.erase(0, pos + 1);
		if (!tok.empty()) v.push_back(tok);
	}
	vector<string> nv;
	for (auto& t : v) if (t != key) nv.push_back(t);
	if (on) nv.push_back(key);
	string joined;
	for (size_t i = 0; i < nv.size(); i++) { if (i) joined += ","; joined += nv[i]; }
	if (joined.empty()) m.erase(IGNORE_ENV_KEY);
	else m[IGNORE_ENV_KEY] = joined;
}

// 启动时扫描：检测 env 与 config 同时设置且未标记 @ignore_env 的冲突。
// - 值相同 → 自动删除冗余的 config 项（环境变量生效即可）
// - 值不同 → interactive=true 时交互式二选一；否则保持现状（环境变量默认生效）
// 返回 true 表示 config 被修改过。
static bool scan_config_conflicts(bool interactive)
{
	auto m = load_config_map();
	bool changed = false;

	auto env_of = [](const CfgItem& it) -> string {
		const char* e = getenv(it.env);
		return (e && *e) ? string(e) : string("");
	};

	for (auto& it : g_cfg_items)
	{
		string env = env_of(it);
		if (env.empty()) continue;
		auto mit = m.find(it.key);
		if (mit == m.end() || mit->second.empty()) continue; // config 未设置/为空，无冲突
		if (cfg_ignores_env(m, it.key)) continue;            // 已显式让 config 优先，不算冲突
		string cfg = mit->second;

		// 二者值相同 → 自动删去冗余 config 项
		if (env == cfg)
		{
			m.erase(it.key);
			cfg_set_ignore_env(m, it.key, false);
			changed = true;
			if (interactive)
				printf(tr("启动扫描：%s 的 config 与环境变量值相同，已自动删除冗余 config 项。\n",
				          "Startup scan: config %s equals its env var; removed the redundant config entry.\n"), it.key);
			continue;
		}

		// 值不同 → 交互式二选一
		if (!interactive) continue;
		printf(tr("⚠ 启动扫描发现冲突：%s（%s）\n", "⚠ Startup scan conflict: %s (%s)\n"), it.key, tr(it.desc, it.desc_en));
		printf(tr("    环境变量 %s = [%s]\n", "    env var  %s = [%s]\n"), it.env, env.c_str());
		printf(tr("    config  %s = [%s]\n", "    config  %s = [%s]\n"), it.key, cfg.c_str());
		printf(tr("    [1] 生效 config（标记其优先于环境变量）\n", "    [1] keep config (mark it higher priority than env)\n"));
		printf(tr("    [2] 生效环境变量（删除 config 项）\n", "    [2] use env var (delete the config entry)\n"));
		printf(tr("  请选择 [1/2]（默认 1）：", "  Choose [1/2] (default 1): "));
		fflush(stdout);
		string choice;
		getline(cin, choice);
		choice = trim(choice);
		if (choice == "2")
		{
			m.erase(it.key);
			cfg_set_ignore_env(m, it.key, false);
			printf(tr("    已删除 config.%s，环境变量生效。\n", "    Deleted config.%s; the env var takes effect.\n"), it.key);
		}
		else
		{
			cfg_set_ignore_env(m, it.key, true);
			printf(tr("    已标记 config.%s 优先。\n", "    Marked config.%s as priority.\n"), it.key);
		}
		changed = true;
		printf("\n");
	}

	if (changed) save_config_map(m);
	return changed;
}

// ===================== 小工具 =====================
static string now_iso()
{
	time_t t = time(nullptr);
	tm* lt = localtime(&t);
	char buf[32];
	strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", lt);
	return buf;
}

// 简单分词：空格分隔，支持双引号包裹含空格的参数
static vector<string> tokenize(const string& line)
{
	vector<string> toks;
	string cur;
	bool inq = false;
	for (char c : line)
	{
		if (c == '"') { inq = !inq; continue; }
		if (!inq && isspace((unsigned char)c))
		{
			if (!cur.empty()) { toks.push_back(cur); cur.clear(); }
		}
		else cur += c;
	}
	if (!cur.empty()) toks.push_back(cur);
	return toks;
}

// ===================== 极简 JSON =====================
struct Json
{
	enum T { Null, Bool, Num, Str, Arr, Obj } t = Null;
	bool b = false;
	double num = 0;
	string s;
	vector<Json> arr;
	vector<pair<string, Json>> obj;

	Json* find(const string& key)
	{
		if (t != Obj) return nullptr;
		for (auto& kv : obj) if (kv.first == key) return &kv.second;
		return nullptr;
	}
};

struct JsonParser
{
	const string& src;
	size_t i = 0;
	JsonParser(const string& s) : src(s) {}
	void ws() { while (i < src.size() && isspace((unsigned char)src[i])) i++; }

	Json parse()
	{
		ws();
		if (i >= src.size()) return Json{};
		char c = src[i];
		if (c == '{') return parseObj();
		if (c == '[') return parseArr();
		if (c == '"') return parseStr();
		if (c == 't' || c == 'f') return parseBool();
		if (c == 'n') return parseNull();
		return parseNum();
	}
	Json parseObj()
	{
		Json j; j.t = Json::Obj; i++; ws();
		if (i < src.size() && src[i] == '}') { i++; return j; }
		while (i < src.size())
		{
			ws();
			Json k = parseStr();
			ws();
			if (i < src.size() && src[i] == ':') i++;
			Json v = parse();
			j.obj.emplace_back(k.s, move(v));
			ws();
			if (i < src.size() && src[i] == ',') { i++; continue; }
			if (i < src.size() && src[i] == '}') { i++; }
			break;
		}
		return j;
	}
	Json parseArr()
	{
		Json j; j.t = Json::Arr; i++; ws();
		if (i < src.size() && src[i] == ']') { i++; return j; }
		while (i < src.size())
		{
			j.arr.push_back(parse());
			ws();
			if (i < src.size() && src[i] == ',') { i++; continue; }
			if (i < src.size() && src[i] == ']') { i++; }
			break;
		}
		return j;
	}
	Json parseStr()
	{
		Json j; j.t = Json::Str; i++; // 跳过开引号
		string r;
		while (i < src.size())
		{
			char c = src[i++];
			if (c == '"') break;
			if (c == '\\')
			{
				if (i >= src.size()) break;
				char e = src[i++];
				switch (e)
				{
					case '"': r += '"'; break;
					case '\\': r += '\\'; break;
					case '/': r += '/'; break;
					case 'b': r += '\b'; break;
					case 'f': r += '\f'; break;
					case 'n': r += '\n'; break;
					case 'r': r += '\r'; break;
					case 't': r += '\t'; break;
					case 'u':
						if (i + 4 <= src.size())
						{
							int cp = (int)strtol(src.substr(i, 4).c_str(), nullptr, 16);
							i += 4;
							if (cp < 0x80) r += (char)cp;
							else if (cp < 0x800)
							{
								r += (char)(0xC0 | (cp >> 6));
								r += (char)(0x80 | (cp & 0x3F));
							}
							else
							{
								r += (char)(0xE0 | (cp >> 12));
								r += (char)(0x80 | ((cp >> 6) & 0x3F));
								r += (char)(0x80 | (cp & 0x3F));
							}
						}
						break;
					default: r += e;
				}
			}
			else r += c;
		}
		j.s = r;
		return j;
	}
	Json parseBool()
	{
		Json j; j.t = Json::Bool;
		if (src.compare(i, 4, "true") == 0) { j.b = true; i += 4; }
		else if (src.compare(i, 5, "false") == 0) { j.b = false; i += 5; }
		return j;
	}
	Json parseNull()
	{
		Json j;
		if (src.compare(i, 4, "null") == 0) i += 4;
		return j;
	}
	Json parseNum()
	{
		Json j; j.t = Json::Num;
		size_t start = i;
		while (i < src.size())
		{
			char c = src[i];
			if ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E') i++;
			else break;
		}
		j.num = strtod(src.c_str() + start, nullptr);
		return j;
	}
};

static Json json_parse(const string& s)
{
	JsonParser p(s);
	return p.parse();
}

static string json_escape(const string& s)
{
	string r = "\"";
	for (char c : s)
	{
		switch (c)
		{
			case '"': r += "\\\""; break;
			case '\\': r += "\\\\"; break;
			case '\b': r += "\\b"; break;
			case '\f': r += "\\f"; break;
			case '\n': r += "\\n"; break;
			case '\r': r += "\\r"; break;
			case '\t': r += "\\t"; break;
			default:
				if ((unsigned char)c < 0x20)
				{
					char b[8];
					sprintf(b, "\\u%04x", (unsigned char)c);
					r += b;
				}
				else r += c;
		}
	}
	r += "\"";
	return r;
}

// ===================== 网络（curl 子进程） =====================
// 统一 curl 执行：参数一律 shell_quote 转义，输出重定向到临时文件再读
// （用 system 重定向避免 MinGW _popen 管道读取问题）
static string curl_run(const string& args, const string& out)
{
	string cmd = "curl " + args + " > " + shell_quote(out) + " 2>" + NULL_DEV;
	system(cmd.c_str());
	string resp;
	ifstream f(out, ios::binary);
	if (f) resp = string((istreambuf_iterator<char>(f)), istreambuf_iterator<char>());
	return resp;
}

// GET 取文本响应（用于查 GitHub release API）
static string http_get(const string& url)
{
	ensure_data_dir();
	return curl_run("-s -L -m 60 -A simple-memo " + shell_quote(url), data_dir() + "/.get.txt");
}

// GET 下载到文件（用于下载 llama.cpp release zip / 更新自身）
static bool curl_download(const string& url, const string& dest)
{
	string cmd = "curl -s -L -m 600 --retry 2 -A simple-memo -o " + shell_quote(dest) + " " + shell_quote(url);
	int rc = system(cmd.c_str());
	if (rc != 0) return false;
	ifstream f(dest, ios::binary | ios::ate);
	return f && f.tellg() > 0;
}

// ===================== Embedding（llama.cpp 子进程） =====================
static string g_last_err;

// 查 llama.cpp 最新 release 中本平台的 zip 下载地址
// Win: bin-win-cpu-x64（新版）/ bin-win-avx2-x64（旧版）；Linux: bin-ubuntu-x64；macOS: bin-macos-arm64 / bin-macos-x64
static string find_llama_release_url()
{
	string resp = http_get("https://api.github.com/repos/ggml-org/llama.cpp/releases/latest");
	if (resp.empty()) return "";
	Json j = json_parse(resp);
	Json* assets = j.find("assets");
	if (!assets || assets->t != Json::Arr) return "";
#ifdef _WIN32
	const string want = "bin-win-cpu-x64", legacy = "bin-win-avx2-x64", loose = "bin-win-";
#elif defined(__APPLE__)
#if defined(__aarch64__)
	const string want = "bin-macos-arm64", legacy = "bin-macos-avx", loose = "bin-macos-";
#else
	const string want = "bin-macos-x64", legacy = "bin-macos-avx", loose = "bin-macos-";
#endif
#else
	const string want = "bin-ubuntu-x64", legacy = "bin-ubuntu", loose = "bin-ubuntu";
#endif
	string best, legacy_hit, fallback;
	for (auto& a : assets->arr)
	{
		Json* name = a.find("name");
		Json* url = a.find("browser_download_url");
		if (!name || name->t != Json::Str || !url || url->t != Json::Str) continue;
		const string& n = name->s;
		if (n.size() < 4 || n.substr(n.size() - 4) != ".zip") continue; // 只认 zip 资产
		if (n.find(want) != string::npos) best = url->s;
		else if (!legacy.empty() && n.find(legacy) != string::npos && legacy_hit.empty()) legacy_hit = url->s;
		else if (n.find(loose) != string::npos && fallback.empty()) fallback = url->s;
	}
	return !best.empty() ? best : (!legacy_hit.empty() ? legacy_hit : fallback);
}

// 验证 llama-server 可正常执行（--version 打印版本后退出，不启动 server）
static bool llama_server_works()
{
	string cmd = "\"" + llama_server_path() + "\" --version > " + NULL_DEV + " 2>" + NULL_DEV;
	return system(cmd.c_str()) == 0;
}

// 解压 zip 到目录：Win 用系统自带 tar（bsdtar，Win10+）；Unix 依次尝试 unzip → python3 → tar
static bool extract_archive(const string& arc, const string& dir)
{
#ifdef _WIN32
	return system(("tar -xf \"" + arc + "\" -C \"" + dir + "\"").c_str()) == 0;
#else
	if (system(("unzip -oq \"" + arc + "\" -d \"" + dir + "\"").c_str()) == 0) return true;
	if (system(("python3 -m zipfile -e \"" + arc + "\" \"" + dir + "/\"").c_str()) == 0) return true;
	return system(("tar -xf \"" + arc + "\" -C \"" + dir + "\"").c_str()) == 0;
#endif
}

// 确保 llama-server 可用；已存在但损坏则清理重下（自愈）。失败设 g_last_err
static bool ensure_llama_binary()
{
	// 已存在且可执行 → 跳过
	if (llama_server_works()) return true;

	// 用户用环境变量 / config 显式指定却不可用 → 不自动重下，直接报错
	string explicit_exe = get_effective("llama_server_exe", "MEMO_LLAMA_SERVER_EXE", "");
	bool user_set = !explicit_exe.empty() && explicit_exe != llama_server_default();
	if (user_set)
	{
		g_last_err = string(tr("llama-server 指定的文件不可用: ", "configured llama-server is not usable: ")) + llama_server_path();
		return false;
	}

	// 默认位置存在但损坏/不完整 → 清理后重下（自愈）
	error_code ec;
	if (filesystem::exists(llama_server_default(), ec))
	{
		printf(tr("  llama-server 不可用（损坏/不完整？），清理后重新下载...\n",
		          "  llama-server not usable (corrupt/incomplete?); cleaning and re-downloading...\n"));
		filesystem::remove_all(llama_dir(), ec);
	}

	ensure_data_dir();
	printf(tr("首次使用：正在获取 llama.cpp（下载 + 解压，请耐心等待）...\n",
	          "First use: fetching llama.cpp (download + extract, please wait)...\n"));

	string url = find_llama_release_url();
	if (url.empty()) { g_last_err = tr("无法获取 llama.cpp 最新 release 地址（网络？请开 WARP/代理）", "cannot find llama.cpp latest release URL (network? try WARP/proxy)"); return false; }

	string zip = data_dir() + "/llama-release.zip";
	printf(tr("  下载 %s\n", "  Downloading %s\n"), url.c_str());
	if (!curl_download(url, zip))
	{
		g_last_err = tr("下载 llama.cpp release 失败（github.com 可能被墙，请开 WARP/代理后重试）",
		                "failed to download llama.cpp release (github.com may be blocked; try WARP/proxy)");
		return false;
	}

	filesystem::create_directories(llama_dir(), ec);
	if (!extract_archive(zip, llama_dir()))
	{
		filesystem::remove(zip, ec);
		g_last_err = tr("解压 llama.cpp release 失败", "failed to extract llama.cpp release");
		return false;
	}
	filesystem::remove(zip, ec);
#ifndef _WIN32
	// zip 不保留执行位 → 解压后补 chmod +x（递归整个目录，llama-server 及附带工具都需要）
	(void)system(("chmod +x \"" + llama_dir() + "\"/* 2>/dev/null").c_str());
#endif

	if (!llama_server_works()) { g_last_err = tr("下载后 llama-server 仍不可用（zip 可能损坏，请重试）", "llama-server still unusable after download (zip may be corrupt; retry)"); return false; }
	printf(tr("  llama.cpp 就绪。\n", "  llama.cpp ready.\n"));
	return true;
}

// POST JSON 到本地 llama-server
static string http_post_json(const string& url, const string& body)
{
	ensure_data_dir();
	string bf = data_dir() + "/.body.json";
	{ ofstream f(bf, ios::binary); f << body; }
	return curl_run("-s -m 60 -X POST -H Content-Type:application/json --data-binary @" + shell_quote(bf) + " " + shell_quote(url),
	                data_dir() + "/.post.txt");
}

// llama-server 是否就绪（/health 返回 HTTP 200；模型加载中是 503）
static bool server_health()
{
	ensure_data_dir();
	string codef = data_dir() + "/.code.txt";
	string args = "-s -m 10 -w %{http_code} -o " + string(NULL_DEV) + " " + shell_quote("http://127.0.0.1:" + to_string(server_port()) + "/health");
	string cmd = "curl " + args + " > " + shell_quote(codef) + " 2>" + NULL_DEV;
	system(cmd.c_str());
	string code;
	ifstream f(codef);
	if (f) getline(f, code);
	return code.find("200") != string::npos;
}

// 启动并等待 llama-server 就绪；已运行则复用
static bool ensure_server()
{
	if (server_health()) return true;
	if (!ensure_llama_binary()) return false;

	// 注入 HF_ENDPOINT 给 server 子进程（国内加速模型下载）
	string hfEp = hf_endpoint();
#ifdef _WIN32
	if (!hfEp.empty()) _putenv(("HF_ENDPOINT=" + hfEp).c_str());
#else
	if (!hfEp.empty()) setenv("HF_ENDPOINT", hfEp.c_str(), 1);
#endif

	printf(tr("  启动 llama-server（首次下载+加载模型，可能需几分钟）...\n",
	          "  Starting llama-server (first run downloads + loads the model; may take minutes)...\n"));
	ensure_data_dir();
	string logf = data_dir() + "/server.log";
	string common = string(" --embedding")
		+ " --host 127.0.0.1 --port " + to_string(server_port())
		+ " -hf " + shell_quote(embed_hf_repo())
		+ " --hf-file " + shell_quote(embed_hf_file());
#ifdef _WIN32
	string cmd = "start \"memo-embd\" /B " + shell_quote(llama_server_path()) + common
		+ " > nul 2> " + shell_quote(logf);
#else
	string cmd = "(" + shell_quote(llama_server_path()) + common
		+ " > /dev/null 2> " + shell_quote(logf) + " &) ";
#endif
	system(cmd.c_str());

	for (int i = 0; i < 600; i++) // 最多等 300 秒（首次模型下载 + 加载）
	{
#ifdef _WIN32
		Sleep(500);
#else
		usleep(500 * 1000);
#endif
		if (server_health()) { printf(tr("  llama-server 就绪。\n", "  llama-server ready.\n")); return true; }
	}
	// 超时：dump server.log 末尾帮助诊断
	g_last_err = tr("llama-server 启动超时（首次下载模型较久，请重试）。server.log 末尾：\n",
	                 "llama-server startup timed out (first model download is slow; retry). server.log tail:\n");
	ifstream lf(logf, ios::binary);
	if (lf)
	{
		string log((istreambuf_iterator<char>(lf)), istreambuf_iterator<char>());
		if (log.size() > 800) log = "..." + log.substr(log.size() - 800);
		g_last_err += log;
	}
	return false;
}

// 程序退出时清理 server
static void kill_server()
{
#ifdef _WIN32
	system("taskkill /IM llama-server.exe /F >nul 2>nul");
#else
	system("pkill -x llama-server >/dev/null 2>&1");
#endif
}

// embeddinggemma 是非对称检索模型，文档/查询需各自前缀（model card 要求）
static string doc_prompt(const string& title, const string& body)
{
	return "title: " + (title.empty() ? string("none") : title) + " | text: " + body;
}
static string query_prompt(const string& q)
{
	return "task: search result | query: " + q;
}

// 成功返回向量；失败返回空并设置 g_last_err
static vector<float> embed_text(const string& text)
{
	g_last_err.clear();
	if (!ensure_server())
	{
		if (g_last_err.empty()) g_last_err = tr("llama-server 不可用", "llama-server unavailable");
		return {};
	}

	string body = "{\"input\":" + json_escape(text) + "}";
	string resp = http_post_json("http://127.0.0.1:" + to_string(server_port()) + "/v1/embeddings", body);
	if (resp.empty()) { g_last_err = tr("embedding 请求无响应", "embedding request got no response"); return {}; }

	// 解析 OpenAI 兼容 JSON（兼容 data[0].embedding 和顶层 embedding）
	Json j = json_parse(resp);
	Json* emb = nullptr;
	Json* data = j.find("data");
	if (data && data->t == Json::Arr && !data->arr.empty())
		emb = data->arr[0].find("embedding");
	if (!emb) emb = j.find("embedding");
	if (!emb || emb->t != Json::Arr)
	{
		g_last_err = string(tr("响应缺少 embedding 数组（原始: ", "response missing embedding array (raw: ")) + resp.substr(0, 200) + ")";
		return {};
	}
	vector<float> v;
	v.reserve(emb->arr.size());
	for (auto& x : emb->arr) if (x.t == Json::Num) v.push_back((float)x.num);
	if (v.empty()) g_last_err = tr("embedding 数组为空", "embedding array is empty");
	return v;
}

static double cosine(const vector<float>& a, const vector<float>& b)
{
	if (a.size() != b.size() || a.empty()) return -1.0;
	double dot = 0, na = 0, nb = 0;
	for (size_t i = 0; i < a.size(); i++)
	{
		dot += (double)a[i] * b[i];
		na += (double)a[i] * a[i];
		nb += (double)b[i] * b[i];
	}
	if (na == 0 || nb == 0) return 0;
	return dot / (sqrt(na) * sqrt(nb));
}

// ===================== 段落切分与文本处理 =====================
// 段落切分：连续 >=2 个换行（空行）为段落边界；单换行保留在段内（软换行）；多余空行压缩。
// \r\n 归一化为 \n（忽略 \r）。正文为空时返回一个空段落（保证标题仍可嵌入）。
static vector<string> split_paragraphs(const string& body)
{
	vector<string> paras;
	string cur;
	size_t i = 0, n = body.size();
	auto flush = [&]()
	{
		if (!cur.empty()) { paras.push_back(cur); cur.clear(); }
	};
	while (i < n)
	{
		char c = body[i];
		if (c == '\r') { i++; continue; }
		if (c == '\n')
		{
			size_t j = i;
			int nl = 0;
			while (j < n && (body[j] == '\n' || body[j] == '\r'))
			{
				if (body[j] == '\n') nl++;
				j++;
			}
			if (nl >= 2) flush();   // 空行 → 段落边界
			else cur += '\n';       // 单换行 → 软换行保留
			i = j;
		}
		else { cur += c; i++; }
	}
	flush();
	if (paras.empty()) paras.push_back(""); // 仅标题/空正文 → 一个空段落
	return paras;
}

// 按行切分（去掉 \r，单 \n 即换行），用于 search 上下文定位；空正文返回空
static vector<string> split_lines(const string& body)
{
	vector<string> lines;
	string cur;
	for (char c : body)
	{
		if (c == '\r') continue;
		if (c == '\n') { lines.push_back(cur); cur.clear(); }
		else cur += c;
	}
	if (body.empty()) return lines;
	if (!cur.empty()) lines.push_back(cur);
	else if (!lines.empty()) lines.pop_back(); // 正文以换行结尾 → 去掉尾空行
	return lines;
}

// 按 UTF-8 字符截断到 maxn 个字符，超出加省略号（不切断多字节字符）
static string truncate_utf8_chars(const string& s, size_t maxn)
{
	size_t chars = 0, i = 0;
	while (i < s.size() && chars < maxn)
	{
		unsigned char c = (unsigned char)s[i];
		size_t len = 1;
		if ((c & 0xE0) == 0xC0) len = 2;
		else if ((c & 0xF0) == 0xE0) len = 3;
		else if ((c & 0xF8) == 0xF0) len = 4;
		if (i + len > s.size()) break;
		i += len;
		chars++;
	}
	if (i >= s.size()) return s;
	return s.substr(0, i) + "…";
}

// ===================== 数据模型与持久化 =====================
struct Memo
{
	ll id = 0;
	string title;
	string body;
	string ctime;
	bool vec_ok = false;
};

static vector<Memo> load_memos()
{
	vector<Memo> ms;
	ifstream f(memos_path());
	if (!f) return ms;
	string line;
	while (getline(f, line))
	{
		if (line.empty()) continue;
		Json j = json_parse(line);
		if (j.t != Json::Obj) continue;
		Memo m;
		Json* p;
		if ((p = j.find("id")) && p->t == Json::Num) m.id = (ll)p->num;
		if ((p = j.find("title")) && p->t == Json::Str) m.title = p->s;
		if ((p = j.find("body")) && p->t == Json::Str) m.body = p->s;
		if ((p = j.find("ctime")) && p->t == Json::Str) m.ctime = p->s;
		if ((p = j.find("vec_ok")) && p->t == Json::Bool) m.vec_ok = p->b;
		ms.push_back(move(m));
	}
	return ms;
}

static void save_memos(const vector<Memo>& ms)
{
	ensure_data_dir();
	string buf;
	buf.reserve(ms.size() * 128);
	for (auto& m : ms)
	{
		buf += "{\"id\":" + to_string(m.id)
			+ ",\"title\":" + json_escape(m.title)
			+ ",\"body\":" + json_escape(m.body)
			+ ",\"ctime\":" + json_escape(m.ctime)
			+ ",\"vec_ok\":" + (m.vec_ok ? "true" : "false")
			+ "}\n";
	}
	write_file_atomic(memos_path(), buf);
}

static ll next_id(const vector<Memo>& ms)
{
	ll mx = 0;
	for (auto& m : ms) mx = max(mx, m.id);
	return mx + 1;
}

// ===================== 向量存储（v2，多段落） =====================
static const uint32_t VEC_SCHEMA_VERSION = 2;
static const char VEC_MAGIC[4] = { 'S', 'M', 'V', '2' };

static string vec_magic_string() { return string(VEC_MAGIC, 4); }

// 每 id 一组段落向量（按段落顺序，段落 0/1/2... 对应 split_paragraphs 结果）
using VecSet = vector<vector<float>>;

// vectors.bin v2 格式：
//   magic(4B "SMV2") + version(uint32)
//   记录: (id uint64)(nparas uint32) + nparas × (dim uint32)(float[dim])
// 旧 v1（无 magic，单向量）→ 不识别，返回空（需 reindex 重算）
static map<ll, VecSet> load_vectors()
{
	map<ll, VecSet> vs;
	ifstream f(vectors_path(), ios::binary);
	if (!f) return vs;

	char magic[4];
	f.read(magic, 4);
	if (f.gcount() != 4 || string(magic, 4) != vec_magic_string()) return vs; // 旧格式/损坏

	uint32_t ver = 0;
	f.read((char*)&ver, sizeof(ver));
	if (f.gcount() != (streamsize)sizeof(ver) || ver != VEC_SCHEMA_VERSION) return vs;

	while (f)
	{
		uint64_t id = 0;
		uint32_t np = 0;
		f.read((char*)&id, sizeof(id));
		if (!f || f.gcount() != (streamsize)sizeof(id)) break;
		f.read((char*)&np, sizeof(np));
		if (!f || f.gcount() != (streamsize)sizeof(np)) break;
		VecSet set;
		bool ok = true;
		for (uint32_t p = 0; p < np; p++)
		{
			uint32_t dim = 0;
			f.read((char*)&dim, sizeof(dim));
			if (!f || f.gcount() != (streamsize)sizeof(dim)) { ok = false; break; }
			vector<float> v(dim);
			if (dim)
			{
				f.read((char*)v.data(), (streamsize)dim * sizeof(float));
				if ((size_t)f.gcount() != (size_t)dim * sizeof(float)) { ok = false; break; }
			}
			set.push_back(move(v));
		}
		if (!ok) break;
		vs[(ll)id] = move(set);
	}
	return vs;
}

static void save_vectors(const map<ll, VecSet>& vs)
{
	ensure_data_dir();
	string buf;
	buf += string(VEC_MAGIC, 4);
	buf.append((const char*)&VEC_SCHEMA_VERSION, sizeof(VEC_SCHEMA_VERSION));
	for (auto& kv : vs)
	{
		uint64_t id = (uint64_t)kv.first;
		uint32_t np = (uint32_t)kv.second.size();
		buf.append((const char*)&id, sizeof(id));
		buf.append((const char*)&np, sizeof(np));
		for (auto& v : kv.second)
		{
			uint32_t dim = (uint32_t)v.size();
			buf.append((const char*)&dim, sizeof(dim));
			if (dim) buf.append((const char*)v.data(), (size_t)dim * sizeof(float));
		}
	}
	write_file_atomic(vectors_path(), buf);
}

// 磁盘上向量文件的 schema 版本：0=不存在，1=旧 v1（无 magic），2=当前
static int vec_schema_version_on_disk()
{
	ifstream f(vectors_path(), ios::binary);
	if (!f) return 0;
	char magic[4];
	f.read(magic, 4);
	if (f.gcount() != 4 || string(magic, 4) != vec_magic_string()) return 1;
	uint32_t ver = 0;
	f.read((char*)&ver, sizeof(ver));
	if (f.gcount() != (streamsize)sizeof(ver)) return 1;
	return (int)ver;
}

// ===================== 命令 =====================
static bool g_repl = false; // REPL 模式下 add 无正文时读 stdin
// ---- 活跃备忘录（-f/--follow 的状态源） ----
// 规则集中于此：add/edit/show/search 首命中/find 首命中 → touch；REPL 退出 → clear
static ll g_last_active_id = 0;
static void memo_touch(ll id) { g_last_active_id = id; }
static ll memo_current() { return g_last_active_id; }
static void memo_clear() { g_last_active_id = 0; }

static int cmd_add(vector<string>& args)
{
	if (args.empty()) { printf(tr("用法: add <标题> [正文...]\n", "Usage: add <title> [body...]\n")); return 1; }
	string title = args[0];
	string body;
	for (size_t i = 1; i < args.size(); i++)
	{
		if (i > 1) body += " ";
		body += args[i];
	}
	if (body.empty() && g_repl)
	{
		printf(tr("请输入正文（单独一行输入 :end 或 end 结束；或按 Ctrl+Z 回车结束）：\n",
		          "Enter body (finish with a lone :end or end line; or Ctrl+Z then Enter):\n"));
		string line;
		while (getline(cin, line))
		{
			// 去掉行尾 \r/空白（兼容 Windows CRLF、误输空格等）
			while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ' || line.back() == '\t'))
				line.pop_back();
			string tl = lower(line);
			if (tl == ":end" || tl == "end" || tl == "：end" || tl == ".end") break;
			if (!body.empty()) body += "\n";
			body += line;
		}
		cin.clear();
	}

	auto ms = load_memos();
	ll id = next_id(ms);
	memo_touch(id); // 新增即为上一个活跃备忘录
	Memo m{ id, title, body, now_iso(), false };

	// 按段落分别计算向量（空行分段，每段带标题前缀）
	auto paras = split_paragraphs(body);
	auto vs = load_vectors();
	VecSet set;
	int ok_cnt = 0, fail_cnt = 0;
	for (auto& p : paras)
	{
		auto v = embed_text(doc_prompt(title, p));
		if (!v.empty()) { set.push_back(v); ok_cnt++; }
		else fail_cnt++;
	}

	if (fail_cnt == 0 && !set.empty())
	{
		m.vec_ok = true;
		vs[id] = set;
		printf(tr("已新增 #%-4lld（向量已生成，%d 段，dim=%zu）\n",
		          "Added #%-4lld (vectors ok, %d paras, dim=%zu)\n"), id, ok_cnt, set[0].size());
	}
	else
	{
		m.vec_ok = false;
		printf(tr("已新增 #%-4lld（警告：embedding 失败 %d 段，标记 pending）\n",
		          "Added #%-4lld (warning: embedding failed for %d paras, marked pending)\n"), id, fail_cnt);
		if (!g_last_err.empty()) printf(tr("  原因：%s\n", "  reason: %s\n"), g_last_err.c_str());
		printf(tr("  解决后运行 reindex 补算。\n", "  Run reindex once fixed.\n"));
	}
	ms.push_back(m);
	save_memos(ms);
	save_vectors(vs);
	return 0;
}

static int cmd_list(vector<string>&)
{
	auto ms = load_memos();
	if (ms.empty()) { printf(tr("（空，还没有备忘录）\n", "(empty, no memos yet)\n")); return 0; }
	printf(tr("共 %zu 条：\n", "%zu memo(s):\n"), ms.size());
	for (auto& m : ms)
	{
		printf("#%-4lld [%s] %s", m.id, m.ctime.c_str(), m.title.c_str());
		if (!m.vec_ok) printf(tr("  (无向量)", "  (no vectors)"));
		printf("\n");
	}
	return 0;
}

// ===================== show：Markdown 渲染 + 分页 =====================
// 终端是否支持 ANSI 转义（Windows 需在 main 里开启 VT；重定向/老终端则关闭渲染色）
static bool g_ansi_ok =
#ifdef _WIN32
	false;
#else
	true;
#endif

static const char* A(const char* code) { return g_ansi_ok ? code : ""; }
static const char* A_RESET() { return A("\x1b[0m"); }

// 行内标记：`code`、**粗体**、*斜体*、_斜体_、__粗体__（未闭合的标记原样输出）
static string md_inline(const string& s)
{
	string out;
	size_t i = 0;
	while (i < s.size())
	{
		char c = s[i];
		if (c == '`')
		{
			size_t j = s.find('`', i + 1);
			if (j != string::npos)
			{
				out += A("\x1b[7m") + s.substr(i + 1, j - i - 1) + A_RESET();
				i = j + 1;
				continue;
			}
		}
		else if (c == '*' || c == '_')
		{
			bool dbl = (i + 1 < s.size() && s[i + 1] == c);
			string seq = dbl ? string(2, c) : string(1, c);
			size_t j = s.find(seq, i + seq.size());
			if (j != string::npos)
			{
				// **粗**；单符号但内容两侧紧贴空格的（如 a * b）按字面处理
				out += A(dbl ? "\x1b[1m" : "\x1b[3m") + s.substr(i + seq.size(), j - i - seq.size()) + A_RESET();
				i = j + seq.size();
				continue;
			}
		}
		out += c;
		i++;
	}
	return out;
}

// 渲染一段多行文本（保留 fence 状态以跨段渲染 ``` 代码块）
static void md_print_text(const string& text, bool& in_fence)
{
	for (auto& ln : split_lines(text))
	{
		if (!in_fence && (ln.rfind("```", 0) == 0 || ln.rfind("~~~", 0) == 0))
		{
			in_fence = true;
			printf("%s%s%s\n", A("\x1b[36m"), ln.c_str(), A_RESET());
			continue;
		}
		if (in_fence)
		{
			if (ln.rfind("```", 0) == 0 || ln.rfind("~~~", 0) == 0)
			{
				in_fence = false;
				printf("%s%s%s\n", A("\x1b[36m"), ln.c_str(), A_RESET());
			}
			else printf("%s%s%s\n", A("\x1b[36m"), ln.c_str(), A_RESET());
			continue;
		}
		// 标题 # ~ ######
		if (ln.size() > 1 && ln[0] == '#')
		{
			size_t h = 0;
			while (h < ln.size() && ln[h] == '#') h++;
			if (h <= 6 && h < ln.size() && (ln[h] == ' ' || ln[h] == '\t'))
			{
				const char* col = h == 1 ? "\x1b[1;96m" : h == 2 ? "\x1b[1;95m" : "\x1b[1;94m";
				printf("%s%s%s\n", A(col), md_inline(ln).c_str(), A_RESET());
				continue;
			}
		}
		// 分隔线
		if (ln.size() >= 3 && (ln.find_first_not_of('-') == string::npos || ln.find_first_not_of('*') == string::npos || ln.find_first_not_of('_') == string::npos))
		{
			printf("%s%s%s\n", A("\x1b[2m"), ln.c_str(), A_RESET());
			continue;
		}
		// 引用 >
		if (ln.size() >= 1 && ln[0] == '>')
		{
			printf("%s%s%s%s\n", A("\x1b[2;3m"), "> ", A_RESET(), md_inline(ln.size() > 1 && ln[1] == ' ' ? ln.substr(2) : ln.substr(1)).c_str());
			continue;
		}
		// 列表（- * + 与 1. 数字）
		{
			size_t k = 0;
			while (k < ln.size() && (ln[k] == ' ' || ln[k] == '\t')) k++;
			bool bullet = k < ln.size() && (ln[k] == '-' || ln[k] == '*' || ln[k] == '+') && k + 1 < ln.size() && ln[k + 1] == ' ';
			bool number = false;
			size_t d = k;
			while (d < ln.size() && ln[d] >= '0' && ln[d] <= '9') d++;
			if (!bullet && d > k && d + 1 < ln.size() && ln[d] == '.' && ln[d + 1] == ' ') number = true;
			if (bullet || number)
			{
				size_t mark_end = bullet ? k + 2 : d + 2;
				printf("%.*s%s%.*s%s%s\n", (int)k, ln.c_str(),
				       A("\x1b[1;33m"), (int)(mark_end - k), ln.c_str() + k, A_RESET(),
				       md_inline(ln.substr(mark_end)).c_str());
				continue;
			}
		}
		printf("%s\n", md_inline(ln).c_str());
	}
}

// 解析 --limit / --from 的值：<n>L 行 / <n>P 段（后缀可省略，默认 L）
static bool parse_unit_val(const string& v, ll& n, char& unit)
{
	if (v.empty()) return false;
	unit = 'L';
	string num = v;
	char back = v[v.size() - 1];
	if (back == 'L' || back == 'l' || back == 'P' || back == 'p')
	{
		unit = (back == 'P' || back == 'p') ? 'P' : 'L';
		num = v.substr(0, v.size() - 1);
	}
	if (num.empty() || num.find_first_not_of("0123456789") != string::npos) return false;
	n = atoll(num.c_str());
	return true;
}

// id/标题定位（定义于下方 cmd_delete 前；show/edit 共用）
static bool resolve_ref(vector<Memo>& ms, const string& ref, ll& id);

static int cmd_show(vector<string>& args)
{
	bool follow = false, md = false, all = false, has_id = false;
	ll id = 0;
	string ref;
	ll limit_n = -1, from_n = 1;
	char limit_unit = 'L', from_unit = 'L';
	for (size_t i = 0; i < args.size(); i++)
	{
		const string& a = args[i];
		if (a == "-f" || a == "--follow") follow = true;
		else if (a == "-m" || a == "--markdown") md = true;
		else if (a == "--all") all = true;
		else if (a.rfind("--limit", 0) == 0)
		{
			string v = a.size() > 7 ? a.substr(7) : "";
			if (!v.empty() && v[0] == '=') v = v.substr(1);
			else if (v.empty() && i + 1 < args.size()) v = args[++i];
			if (!parse_unit_val(v, limit_n, limit_unit))
			{
				printf(tr("用法: --limit=<n>L 或 <n>P（如 --limit=20L、--limit=3P）\n", "Usage: --limit=<n>L or <n>P (e.g. --limit=20L, --limit=3P)\n"));
				return 1;
			}
		}
		else if (a.rfind("--from", 0) == 0)
		{
			string v = a.size() > 6 ? a.substr(6) : "";
			if (!v.empty() && v[0] == '=') v = v.substr(1);
			else if (v.empty() && i + 1 < args.size()) v = args[++i];
			if (!parse_unit_val(v, from_n, from_unit))
			{
				printf(tr("用法: --from=<n>L 或 <n>P（如 --from=5L，从第 5 行开始）\n", "Usage: --from=<n>L or <n>P (e.g. --from=5L, start at line 5)\n"));
				return 1;
			}
		}
		else { ref = a; has_id = true; }
	}
	if (!has_id && !follow)
	{
		printf(tr("用法: show <id|标题片段> [-f] [-m] [--limit=nL|nP] [--from=nL|nP] [--all]\n",
		          "Usage: show <id|title fragment> [-f] [-m] [--limit=nL|nP] [--from=nL|nP] [--all]\n"));
		return 1;
	}
	if (follow && !has_id)
	{
		if (memo_current() == 0) { printf(tr("（还没有活跃备忘录：先 add / edit / search / find）\n", "(no active memo yet: try add / edit / search / find first)\n")); return 0; }
		id = memo_current();
	}

	auto ms = load_memos();
	if (has_id && !resolve_ref(ms, ref, id)) return 1;
	for (auto& m : ms) if (m.id == id)
	{
		memo_touch(id);
		printf("#%lld  %s\n", m.id, m.title.c_str());
		printf(tr("  时间: %s   向量: %s\n", "  time: %s   vectors: %s\n"), m.ctime.c_str(), m.vec_ok ? tr("已生成", "ok") : tr("缺失", "missing"));

		// ---- 分页：单位（行 L / 段 P），显式 --limit/--from 优先于自动截断 ----
		char unit = (limit_n >= 0) ? limit_unit : from_unit;
		bool explicit_range = (limit_n >= 0 || from_n > 1);
		vector<string> units;
		if (unit == 'P') units = split_paragraphs(m.body);
		else units = split_lines(m.body);
		ll total = (ll)units.size();

		ll begin = (from_n > 1) ? from_n : 1;
		ll end = total;
		if (limit_n >= 0) end = begin + limit_n - 1;

		// 未显式指定范围：默认最多显示一部分；--all 显示全部（但仍设硬上限，尽可能多显示）
		const ll DEF_MAX_UNITS = 50, DEF_MAX_BYTES = 4000;
		const ll ALL_MAX_UNITS = 500, ALL_MAX_BYTES = 50000;
		if (!explicit_range)
		{
			ll maxU = all ? ALL_MAX_UNITS : DEF_MAX_UNITS;
			ll maxB = all ? ALL_MAX_BYTES : DEF_MAX_BYTES;
			ll bytes = 0;
			end = begin - 1;
			for (ll k = begin; k <= total; k++)
			{
				bytes += (ll)units[k - 1].size() + 1;
				end = k;
				if (end - begin + 1 >= maxU || bytes >= maxB) break;
			}
		}
		if (end > total) end = total;

		const char* un = (unit == 'P') ? tr("段", "para(s)") : tr("行", "line(s)");
		ll shown = end - begin + 1;
		printf(tr("  正文（共 %lld %s）:\n", "  body (%lld %s total):\n"), total, un);
		if (shown <= 0)
		{
			printf(tr("  （第 %lld %s超出范围，没有可显示的内容）\n", "  (unit %lld is out of range; nothing to show)\n"), begin, un);
			return 0;
		}
		if (begin > 1)
			printf("%s%s%s\n", A("\x1b[2m"),
			       (string(tr("  ……（前面还有 ", "  ... (") + to_string(begin - 1) + " " + un + tr("未显示）", " before, omitted)"))).c_str(), A_RESET());

		bool in_fence = false;
		for (ll k = begin; k <= end; k++)
		{
			if (k > begin) printf(unit == 'P' ? "\n" : "");
			if (md) md_print_text(units[k - 1], in_fence);
			else printf("%s\n", units[k - 1].c_str());
		}
		if (end < total)
		{
			string hint = explicit_range ? "" : tr("；--all 显示全部", "; use --all to show everything");
			printf("%s%s%s\n", A("\x1b[2m"),
			       (string(tr("  ……（后面还有 ", "  ... (") + to_string(total - end) + " " + un + tr("未显示", " after, omitted")) + hint + tr("）", ")")).c_str(), A_RESET());
		}
		return 0;
	}
	printf(tr("找不到 #%lld\n", "memo #%lld not found\n"), id);
	return 1;
}

static int cmd_search(vector<string>& args)
{
	bool any = false;
	bool list_only = false;
	vector<string> kws;
	for (auto& a : args)
	{
		if (a == "--any") any = true;
		else if (a == "-l" || a == "--list") list_only = true;
		else kws.push_back(a);
	}
	if (kws.empty()) { printf(tr("用法: search <关键词...> [--any] [-l|--list]\n", "Usage: search <keywords...> [--any] [-l|--list]\n")); return 1; }

	vector<string> kws_l;
	for (auto& k : kws) kws_l.push_back(lower(k));

	// 某行文本（已 lower）是否命中，与整体 AND/OR 语义一致
	auto line_hit = [&](const string& line_lower) -> bool {
		size_t matched = 0;
		for (auto& k : kws_l) if (line_lower.find(k) != string::npos) matched++;
		return any ? (matched >= 1) : (matched == kws_l.size());
	};

	auto ms = load_memos();
	vector<Memo*> hits;
	for (auto& m : ms)
	{
		string hay = lower(m.title + " " + m.body);
		size_t matched = 0;
		for (auto& k : kws_l) if (hay.find(k) != string::npos) matched++;
		bool ok = any ? (matched >= 1) : (matched == kws_l.size());
		if (ok) hits.push_back(&m);
	}
	if (hits.empty()) { printf(tr("未匹配任何备忘录。\n", "No memos matched.\n")); return 0; }
	g_last_active_id = hits[0]->id; // search 首个匹配即为上一个活跃备忘录

	printf(tr("命中 %zu 条（%s）：\n", "%zu hit(s) (%s):\n"), hits.size(), any ? "OR" : "AND");
	for (auto* m : hits)
	{
		if (list_only)
		{
			printf("#%-4lld [%s] %s\n", m->id, m->ctime.c_str(), m->title.c_str());
			continue;
		}

		bool t_hit = line_hit(lower(m->title));
		printf("#%-4lld [%s] %s%s\n", m->id, m->ctime.c_str(), m->title.c_str(), t_hit ? "  <-" : "");

		auto lines = split_lines(m->body);
		vector<string> lines_lower;
		lines_lower.reserve(lines.size());
		for (auto& L : lines) lines_lower.push_back(lower(L));

		// 收集命中行
		vector<int> hit_idx;
		for (int i = 0; i < (int)lines.size(); i++)
			if (line_hit(lines_lower[i])) hit_idx.push_back(i);

		if (!hit_idx.empty())
		{
			const int CTX = 2; // 上下文行数（前后各 2 行）
			vector<pair<int, int>> ranges;
			for (int idx : hit_idx)
			{
				int lo = max(0, idx - CTX);
				int hi = min((int)lines.size() - 1, idx + CTX);
				if (ranges.empty() || lo > ranges.back().second + 1)
					ranges.emplace_back(lo, hi);
				else
					ranges.back().second = max(ranges.back().second, hi);
			}
			for (auto& r : ranges)
			{
				if (r.first > 0) printf("    ...\n");
				for (int i = r.first; i <= r.second; i++)
					printf("    %4d | %s%s\n", i + 1, lines[i].c_str(),
					       line_hit(lines_lower[i]) ? "  <-" : "");
				if (r.second != (int)lines.size() - 1) printf("    ...\n");
			}
		}
	}
	return 0;
}

static int cmd_find(vector<string>& args)
{
	int n = 5;
	bool list_only = false;
	vector<string> parts;
	for (auto& a : args)
	{
		if (a == "-l" || a == "--list") list_only = true;
		else if (a.rfind("-n", 0) == 0 && a.size() > 2) n = atoi(a.c_str() + 2);
		else if (a != "-n") parts.push_back(a);
	}
	if (parts.empty()) { printf(tr("用法: find <语义查询> [-nK] [-l|--list]\n", "Usage: find <semantic query> [-nK] [-l|--list]\n")); return 1; }

	string q;
	for (size_t i = 0; i < parts.size(); i++) { if (i) q += " "; q += parts[i]; }

	auto qv = embed_text(query_prompt(q));
	if (qv.empty())
	{
		printf(tr("无法向量化查询，语义搜索中止。\n  原因：%s\n", "Failed to embed the query; semantic search aborted.\n  reason: %s\n"), g_last_err.c_str());
		return 1;
	}

	auto ms = load_memos();
	auto vs = load_vectors();
	struct Hit { double score; Memo* m; int para; };
	vector<Hit> ranked;
	for (auto& m : ms)
	{
		if (!m.vec_ok) continue;
		auto it = vs.find(m.id);
		if (it == vs.end()) continue;
		for (size_t p = 0; p < it->second.size(); p++)
			ranked.push_back({ cosine(qv, it->second[p]), &m, (int)p });
	}
	if (ranked.empty())
	{
		printf(tr("没有可语义搜索的备忘录（可能都 pending）。运行 reindex 补算。\n",
		          "No memos with vectors (all pending?). Run reindex to compute them.\n"));
		return 0;
	}
	sort(ranked.begin(), ranked.end(), [](auto& a, auto& b) { return a.score > b.score; });
	g_last_active_id = ranked[0].m->id; // find 第一匹配即为上一个活跃备忘录

	int shown = min((int)ranked.size(), n > 0 ? n : 5);
	printf(tr("Top-%d 语义匹配：\n", "Top-%d semantic matches:\n"), shown);
	if (list_only)
	{
		// 只列标题（去重：每条备忘录按最高段落分只出现一次）
		unordered_set<ll> seen;
		int printed = 0;
		for (auto& h : ranked)
		{
			if (printed >= shown) break;
			if (!seen.insert(h.m->id).second) continue;
			printf("  %5.1f%%  #%-4lld %s\n", h.score * 100.0, h.m->id, h.m->title.c_str());
			printed++;
		}
	}
	else
	{
		for (int i = 0; i < shown; i++)
		{
			auto& h = ranked[i];
			auto paras = split_paragraphs(h.m->body);
			string ptext = (h.para >= 0 && h.para < (int)paras.size()) ? paras[h.para] : "";
			string preview = truncate_utf8_chars(ptext, 18);
			if (preview.empty()) preview = tr("(仅标题)", "(title only)");
			printf("  %5.1f%%  #%-4lld %s  ── %s\n", h.score * 100.0, h.m->id, h.m->title.c_str(), preview.c_str());
		}
	}
	return 0;
}

// ===================== id / 标题定位 =====================
// 纯数字按 id；否则按标题片段（不区分大小写子串）唯一匹配。
// 成功 true 并设 id；失败已打印原因（0 命中 / 多命中列候选）。
static bool resolve_ref(vector<Memo>& ms, const string& ref, ll& id)
{
	bool numeric = !ref.empty() && ref.find_first_not_of("0123456789") == string::npos;
	if (numeric)
	{
		id = atoll(ref.c_str());
		for (auto& m : ms) if (m.id == id) return true;
		printf(tr("找不到 #%lld\n", "memo #%lld not found\n"), id);
		return false;
	}
	string low = lower(ref);
	vector<pair<ll, string>> hits;
	for (auto& m : ms) if (lower(m.title).find(low) != string::npos) hits.push_back({ m.id, m.title });
	if (hits.size() == 1)
	{
		id = hits[0].first;
		return true;
	}
	if (hits.empty())
	{
		printf(tr("没有标题含「%s」的备忘录（也可直接用 id）。\n", "No memo title contains \"%s\" (or use an id).\n"), ref.c_str());
		return false;
	}
	printf(tr("「%s」匹配到 %d 条，请用 id 或更精确的片段：\n", "\"%s\" matches %d memos; use an id or a more specific fragment:\n"), ref.c_str(), (int)hits.size());
	for (auto& h : hits) printf("  #%lld  %s\n", h.first, h.second.c_str());
	return false;
}

static int cmd_delete(vector<string>& args)
{
	string ref;
	bool yes = false;
	for (auto& a : args)
	{
		if (a == "-y" || a == "--yes") yes = true;
		else if (ref.empty()) ref = a;
	}
	if (ref.empty()) { printf(tr("用法: delete <id|标题片段> [-y]\n", "Usage: delete <id|title fragment> [-y]\n")); return 1; }

	auto ms = load_memos();
	ll id = 0;
	if (!resolve_ref(ms, ref, id)) return 1;

	string title;
	for (auto& m : ms) if (m.id == id) title = m.title;

	// 删除不可恢复 → 二次确认（-y 跳过；非交互管道下默认拒绝）
	if (!yes)
	{
		printf(tr("将删除 #%lld「%s」（不可恢复）。确认？(y/N) ", "Delete #%lld \"%s\" (irreversible)? (y/N) "), id, title.c_str());
		fflush(stdout);
		string ans;
		if (!getline(cin, ans))
		{
			printf(tr("\n未确认（非交互环境请加 -y），已取消。\n", "\nNot confirmed (add -y in non-interactive use); cancelled.\n"));
			return 1;
		}
		ans = lower(trim(ans));
		if (ans != "y" && ans != "yes")
		{
			printf(tr("已取消。\n", "Cancelled.\n"));
			return 0;
		}
	}

	vector<Memo> ns;
	ns.reserve(ms.size());
	for (auto& m : ms) if (m.id != id) ns.push_back(m);
	save_memos(ns);

	auto vs = load_vectors();
	vs.erase(id);
	save_vectors(vs);
	printf(tr("已删除 #%lld\n", "Deleted #%lld\n"), id);
	return 0;
}

// 编辑器命令：config.editor / $EDITOR > $VISUAL > 平台默认（Win: edit→notepad；Unix: nano→vi）
// 在 PATH 中查找可执行文件
static bool exe_on_path(const string& name)
{
#ifdef _WIN32
	const char sep = ';';
	const string exe = name + ".exe";
#else
	const char sep = ':';
	const string exe = name;
#endif
	const char* pe = getenv("PATH");
	if (!pe) return false;
	string dirs(pe);
	size_t pos = 0;
	while (pos <= dirs.size())
	{
		size_t end = dirs.find(sep, pos);
		string dir = dirs.substr(pos, (end == string::npos ? dirs.size() : end) - pos);
		error_code ec;
		if (!dir.empty() && filesystem::exists(dir + "/" + exe, ec)) return true;
		if (end == string::npos) break;
		pos = end + 1;
	}
	return false;
}

static string default_editor()
{
#ifdef _WIN32
	// 新版 edit.exe（2025+ Win11 随更新推送）是阻塞式控制台编辑器，体验更好；老系统无则退回 notepad
	if (exe_on_path("edit")) return "edit";
	return "notepad";
#else
	if (exe_on_path("nano")) return "nano";
	return "vi";
#endif
}

static string resolve_editor()
{
	string v = get_effective("editor", "EDITOR", "");
	if (!v.empty()) return v;
	v = get_env("VISUAL", "");
	if (!v.empty()) return v;
	return default_editor();
}

static void write_edit_file(const string& path, const string& title, const string& body)
{
	ofstream f(path, ios::binary | ios::trunc);
	const unsigned char bom[3] = { 0xEF, 0xBB, 0xBF };
	f.write((const char*)bom, 3);
	f << title << "\n====\n" << body;
}

// 返回 false：找不到 ==== 分隔行；否则 title（已 trim，可能为空）/ body 已解析
static bool parse_edit_file(const string& path, string& title, string& body)
{
	ifstream f(path, ios::binary);
	if (!f) return false;
	string content((istreambuf_iterator<char>(f)), istreambuf_iterator<char>());
	if (content.size() >= 3 && (unsigned char)content[0] == 0xEF
		&& (unsigned char)content[1] == 0xBB && (unsigned char)content[2] == 0xBF)
		content = content.substr(3);
	auto lines = split_lines(content);
	size_t sep = string::npos;
	for (size_t i = 0; i < lines.size(); i++)
		if (trim(lines[i]) == "====") { sep = i; break; }
	if (sep == string::npos) return false;
	title = sep > 0 ? trim(lines[0]) : "";
	body.clear();
	for (size_t i = sep + 1; i < lines.size(); i++)
	{
		if (i > sep + 1) body += "\n";
		body += lines[i];
	}
	while (!body.empty() && (body.back() == '\n' || body.back() == '\r')) body.pop_back();
	return true;
}

static int cmd_edit(vector<string>& args)
{
	if (args.empty()) { printf(tr("用法: edit <id|标题片段>\n", "Usage: edit <id|title fragment>\n")); return 1; }
	auto ms = load_memos();
	ll id = 0;
	if (!resolve_ref(ms, args[0], id)) return 1;
	for (auto& m : ms) if (m.id == id)
	{
		memo_touch(id); // 被编辑即为上一个活跃备忘录
		ensure_data_dir();
		string tmp = data_dir() + "/.edit-" + to_string(id) + ".txt";
		string editor = resolve_editor(); // 已含平台默认，永不为空

		for (;;)
		{
			write_edit_file(tmp, m.title, m.body);   // 每次重试重置为当前内容

			string cmd = editor + " \"" + tmp + "\""; // 允许 $EDITOR 带参数
			int rc = system(cmd.c_str());
			if (rc != 0 && editor != default_editor())
			{
				printf(tr("  启动 %s 失败，回退到 %s。\n", "  Failed to launch %s; falling back to %s.\n"), editor.c_str(), default_editor().c_str());
				editor = default_editor();
				rc = system((editor + " \"" + tmp + "\"").c_str());
			}

			// 统一等待：不依赖编辑器是否阻塞
			printf(tr("  编辑完成、保存并关闭编辑器后，请按回车继续……\n",
			          "  After saving and closing the editor, press Enter to continue...\n"));
			fflush(stdout);
			string dummy;
			getline(cin, dummy);

			string nt, nb;
			bool ok = parse_edit_file(tmp, nt, nb);
			if (ok && !nt.empty())
			{
				filesystem::remove(tmp);
				if (nt == m.title && nb == m.body) { printf(tr("#%lld 未改动。\n", "#%lld unchanged.\n"), id); return 0; }
				m.title = nt;
				m.body = nb;

				// 复用段落向量重算逻辑（同原实现）
				auto paras = split_paragraphs(m.body);
				auto vs = load_vectors();
				VecSet set;
				int ok_cnt = 0, fail_cnt = 0;
				for (auto& p : paras)
				{
					auto v = embed_text(doc_prompt(m.title, p));
					if (!v.empty()) { set.push_back(v); ok_cnt++; }
					else fail_cnt++;
				}
				if (fail_cnt == 0 && !set.empty())
				{
					m.vec_ok = true;
					vs[id] = set;
					save_memos(ms);
					save_vectors(vs);
					printf(tr("已编辑 #%lld 并重算向量（%d 段）。\n", "Edited #%lld and recomputed vectors (%d paras).\n"), id, ok_cnt);
				}
				else
				{
					m.vec_ok = false;
					save_memos(ms);
					save_vectors(vs);
					printf(tr("已编辑 #%lld（embedding 失败 %d 段，标记 pending", "Edited #%lld (embedding failed for %d paras, marked pending"), id, fail_cnt);
					if (!g_last_err.empty()) printf(tr("：%s", ": %s"), g_last_err.c_str());
				printf(tr("）\n", ")\n"));
				}
				return 0;
			}
			// 格式非法：报错，重置并重试
			printf(tr("  编辑结果无效（%s），已恢复原内容并重新打开编辑器。\n",
			          "  Invalid edit result (%s); restored the original and reopened the editor.\n"),
				ok ? tr("标题为空", "empty title") : tr("缺少 ==== 分隔行", "missing ==== separator line"));
		}
	}
	printf(tr("找不到 #%lld\n", "memo #%lld not found\n"), id);
	return 1;
}

static int cmd_reindex(vector<string>& args)
{
	bool all = false;
	for (auto& a : args) if (a == "--all") all = true;

	// 旧向量格式（schema < 当前）→ 强制全量重建，用户无需手动 --all
	int dv = vec_schema_version_on_disk();
	bool force_all = (dv != 0 && dv < (int)VEC_SCHEMA_VERSION);

	auto ms = load_memos();
	auto vs = load_vectors();
	int cnt = 0, ok = 0, total_para = 0;
	for (auto& m : ms)
	{
		if (!all && !force_all && m.vec_ok) continue;
		cnt++;
		auto paras = split_paragraphs(m.body);
		VecSet set;
		bool done = true;
		for (auto& p : paras)
		{
			auto v = embed_text(doc_prompt(m.title, p));
			if (!v.empty()) { set.push_back(v); total_para++; }
			else { done = false; printf(tr("  #%lld 失败：%s\n", "  #%lld failed: %s\n"), m.id, g_last_err.c_str()); }
		}
		if (done && !set.empty()) { vs[m.id] = set; m.vec_ok = true; ok++; }
		else m.vec_ok = false;
	}
	save_memos(ms);
	save_vectors(vs);
	if (force_all) printf(tr("检测到旧向量格式 v%d，已按全量重建。\n", "Old vector format v%d detected; rebuilt fully.\n"), dv);
	if (cnt == 0) printf(tr("没有需要补算的（全部已有向量）。加 --all 可全量重建。\n", "Nothing pending (all vectors ok). Add --all to rebuild.\n"));
	else printf(tr("补算完成：%d/%d 条成功（共 %d 段）。\n", "Reindex done: %d/%d ok (%d paras total).\n"), ok, cnt, total_para);
	return 0;
}

// 来源标签（cmd_config 展示用）
static const char* cfg_src_label(bool ignored, bool has_env, bool has_cfg)
{
	if (ignored) return tr("config(优先)", "config(priority)");
	if (has_env) return tr("环境变量", "env var");
	if (has_cfg) return tr("config", "config");
	return tr("默认", "default");
}

static int cmd_config(vector<string>& args)
{
	auto env_of = [](const CfgItem& it) -> string {
		const char* e = getenv(it.env);
		return (e && *e) ? string(e) : string("");
	};
	auto show_one = [&](const CfgItem& it) {
		auto m = load_config_map();
		string env = env_of(it);
		string cfg = (m.find(it.key) != m.end()) ? m[it.key] : "";
		bool ignored = cfg_ignores_env(m, it.key);
		string eff = get_effective(it.key, it.env, it.def());
		string src = cfg_src_label(ignored, !env.empty(), !cfg.empty());
		printf("  %-16s %s\n", it.key, tr(it.desc, it.desc_en));
		printf(tr("      环境变量 %-16s =[%s]\n", "      env var  %-16s =[%s]\n"), it.env, env.c_str());
		printf(tr("      config  %-16s =[%s]%s\n", "      config  %-16s =[%s]%s\n"), it.key, cfg.c_str(), ignored ? tr("  (优先于环境变量)", "  (priority over env)") : "");
		printf(tr("      默认 =[%s]   生效=[%s]  来源:%s\n", "      default=[%s]   effective=[%s]  source:%s\n"), it.def().c_str(), eff.c_str(), src.c_str());
	};

	// config get [key]
	if (!args.empty() && args[0] == "get")
	{
		if (args.size() == 1)
		{
			printf(tr("当前配置（配置文件 %s）：\n", "Current config (file %s):\n"), config_file().c_str());
			for (auto& it : g_cfg_items) { show_one(it); printf("\n"); }
		}
		else
		{
			const CfgItem* p = cfg_find(args[1]);
			if (!p)
			{
				printf(tr("未知配置项：%s（可用：", "Unknown config key: %s (available: "), args[1].c_str());
				for (auto& it : g_cfg_items) printf("%s ", it.key);
				printf(tr("）\n", ")\n"));
				return 1;
			}
			show_one(*p);
		}
		return 0;
	}

	// config set <key> <value>
	if (!args.empty() && args[0] == "set")
	{
		if (args.size() < 3) { printf(tr("用法: config set <key> <value>\n", "Usage: config set <key> <value>\n")); return 1; }
		const CfgItem* p = cfg_find(args[1]);
		if (!p)
		{
			printf(tr("未知配置项：%s（可用：", "Unknown config key: %s (available: "), args[1].c_str());
			for (auto& it : g_cfg_items) printf("%s ", it.key);
			printf(tr("）\n", ")\n"));
			return 1;
		}
		string value = args[2];
		for (size_t i = 3; i < args.size(); i++) { value += " "; value += args[i]; }
		auto m = load_config_map();
		m[p->key] = value;
		if (!env_of(*p).empty())
		{
			cfg_set_ignore_env(m, p->key, true);
			printf(tr("注意：环境变量 %s 仍存在，已标记 %s 优先于环境变量。\n",
			          "Note: env %s is still set; marked %s to take priority over it.\n"), p->env, p->key);
		}
		save_config_map(m);
		printf(tr("已设置 %s = %s\n", "Set %s = %s\n"), p->key, value.c_str());
		return 0;
	}

	// config unset <key>
	if (!args.empty() && args[0] == "unset")
	{
		if (args.size() < 2) { printf(tr("用法: config unset <key>\n", "Usage: config unset <key>\n")); return 1; }
		const CfgItem* p = cfg_find(args[1]);
		if (!p) { printf(tr("未知配置项：%s\n", "Unknown config key: %s\n"), args[1].c_str()); return 1; }
		auto m = load_config_map();
		m.erase(p->key);
		cfg_set_ignore_env(m, p->key, false);
		save_config_map(m);
		printf(tr("已移除 %s（回归环境变量/默认）。\n", "Removed %s (falls back to env/default).\n"), p->key);
		return 0;
	}

	// 引导式（无参）：逐项显示，回车保持 / 输入新值 / 输入 - 恢复默认
	printf(tr("配置（回车=保持，输入新值=修改，输入 -=恢复默认；文件 %s）：\n\n",
	          "Config (Enter=keep, new value=change, '-'=reset to default; file %s):\n\n"), config_file().c_str());
	auto m = load_config_map();
	for (auto& it : g_cfg_items)
	{
		string env = env_of(it);
		string cfg = (m.find(it.key) != m.end()) ? m[it.key] : "";
		bool ignored = cfg_ignores_env(m, it.key);

		// 冲突：环境变量与 config 同时存在且未标记 → 立即提示二选一
		if (!env.empty() && !cfg.empty() && !ignored)
		{
			printf(tr("⚠ 冲突：%s（%s）\n", "⚠ Conflict: %s (%s)\n"), it.key, tr(it.desc, it.desc_en));
			printf(tr("    环境变量 %s = [%s]\n", "    env var  %s = [%s]\n"), it.env, env.c_str());
			printf(tr("    config  %s = [%s]\n", "    config  %s = [%s]\n"), it.key, cfg.c_str());
			printf(tr("    [1] 生效 config（标记 %s 环境变量优先级下降）\n", "    [1] keep config (lower the priority of env for %s)\n"), it.key);
			printf(tr("    [2] 生效环境变量（删除 config 里的 %s）\n", "    [2] use env var (delete %s from config)\n"), it.key);
			printf(tr("  请选择 [1/2]（默认 1）：", "  Choose [1/2] (default 1): "));
			fflush(stdout);
			string choice;
			getline(cin, choice);
			choice = trim(choice);
			if (choice == "2")
			{
				m.erase(it.key);
				cfg_set_ignore_env(m, it.key, false);
				save_config_map(m);
				printf(tr("    已删除 config.%s，环境变量生效。\n", "    Deleted config.%s; the env var takes effect.\n"), it.key);
			}
			else
			{
				cfg_set_ignore_env(m, it.key, true);
				save_config_map(m);
				printf(tr("    已标记 config.%s 优先。\n", "    Marked config.%s as priority.\n"), it.key);
			}
			printf("\n");
			continue;
		}

		string eff = get_effective(it.key, it.env, it.def());
		string src = cfg_src_label(ignored, !env.empty(), !cfg.empty());
		printf("%-16s %s\n", it.key, tr(it.desc, it.desc_en));
		printf(tr("    当前=%s  来源:%s  默认=%s\n", "    current=%s  source:%s  default=%s\n"), eff.c_str(), src.c_str(), it.def().c_str());
		printf("    > ");
		fflush(stdout);
		string input;
		getline(cin, input);
		input = trim(input);
		if (input.empty()) { /* 保持 */ }
		else if (input == "-")
		{
			m.erase(it.key);
			cfg_set_ignore_env(m, it.key, false);
			save_config_map(m);
			printf(tr("    已恢复默认/环境变量。\n", "    Reset to default/env.\n"));
		}
		else
		{
			m[it.key] = input;
			if (!env.empty()) cfg_set_ignore_env(m, it.key, true);
			save_config_map(m);
			if (!env.empty()) printf(tr("    已设置并标记 config 优先（环境变量 %s 仍存在）。\n", "    Set and marked config priority (env %s still set).\n"), it.env);
			else printf(tr("    已设置。\n", "    Set.\n"));
		}
		printf("\n");
	}
	printf(tr("配置已保存到 %s\n", "Config saved to %s\n"), config_file().c_str());
	return 0;
}

static int cmd_stop(vector<string>&)
{
	kill_server();
	printf(tr("已停止 llama-server。\n", "llama-server stopped.\n"));
	return 0;
}

// ===================== 并发锁（REPL 实例互斥） =====================
// 两个 REPL 同时写会产生重复 id / 相互覆盖 → data_dir/.lock 记录持有者 PID。
// 崩溃残留的锁通过 PID 存活检测自愈；子命令模式不锁（运行短暂）。
static string lock_path() { return data_dir() + "/.lock"; }
static bool g_holds_lock = false;

static ll pid_self()
{
#ifdef _WIN32
	return (ll)GetCurrentProcessId();
#else
	return (ll)getpid();
#endif
}

// PID 是否仍在运行（Unix: kill -0；Win: tasklist 过滤）
static bool pid_alive(ll pid)
{
	if (pid <= 0) return false;
#ifdef _WIN32
	string cmd = "tasklist /FI \"PID eq " + to_string(pid) + "\" 2>" + NULL_DEV + " | find \"" + to_string(pid) + "\" >" + NULL_DEV;
	return system(cmd.c_str()) == 0;
#else
	return kill((pid_t)pid, 0) == 0 || errno == EPERM;
#endif
}

// REPL 启动前获取锁：无锁/死锁残留 → 写入自己的 PID；活锁存在 → 询问是否仍继续
static bool acquire_lock()
{
	ensure_data_dir();
	ifstream f(lock_path());
	if (f)
	{
		ll pid = 0;
		f >> pid;
		if (pid > 0 && pid_alive(pid))
		{
			printf(tr("警告：另一个实例正在运行（PID %lld），同时写会损坏数据。仍要继续？(y/N) ",
			          "Warning: another instance is running (PID %lld); concurrent writes corrupt data. Continue anyway? (y/N) "), pid);
			fflush(stdout);
			string ans;
			if (!getline(cin, ans) || (lower(trim(ans)) != "y" && lower(trim(ans)) != "yes"))
			{
				printf(tr("已退出。\n", "Exited.\n"));
				return false;
			}
		}
		// 锁残留（进程已死）或用户强制继续 → 覆盖
		error_code ec;
		filesystem::remove(lock_path(), ec);
	}
	ofstream o(lock_path(), ios::trunc);
	o << pid_self() << "\n";
	g_holds_lock = true;
	return true;
}

static void release_lock()
{
	if (!g_holds_lock) return;
	error_code ec;
	filesystem::remove(lock_path(), ec);
	g_holds_lock = false;
}

// ===================== 自更新（GitHub Release） =====================
// "v1.2.3" → [1,2,3]（解析失败返回空）
static vector<int> ver_parts(const string& v)
{
	string s = v;
	if (!s.empty() && (s[0] == 'v' || s[0] == 'V')) s = s.substr(1);
	vector<int> r;
	string cur;
	for (char c : s)
	{
		if (c == '.') { r.push_back(atoi(cur.c_str())); cur.clear(); }
		else if (isdigit((unsigned char)c)) cur += c;
		else if (!cur.empty()) { r.push_back(atoi(cur.c_str())); cur.clear(); }
	}
	if (!cur.empty()) r.push_back(atoi(cur.c_str()));
	return r;
}

static int ver_cmp(const string& a, const string& b)
{
	auto va = ver_parts(a), vb = ver_parts(b);
	size_t n = max(va.size(), vb.size());
	for (size_t i = 0; i < n; i++)
	{
		int x = i < va.size() ? va[i] : 0;
		int y = i < vb.size() ? vb[i] : 0;
		if (x != y) return x < y ? -1 : 1;
	}
	return 0;
}

// 当前可执行文件绝对路径（Windows: GetModuleFileName；Linux: /proc/self/exe；macOS: _NSGetExecutablePath）
static string current_exe_path()
{
	char buf[4096];
#ifdef _WIN32
	DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
	if (n > 0 && n < MAX_PATH) return string(buf, n);
#elif defined(__APPLE__)
	uint32_t sz = sizeof(buf);
	if (_NSGetExecutablePath(buf, &sz) == 0) return string(buf);
#else
	ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
	if (n > 0) { buf[n] = 0; return string(buf); }
#endif
#ifdef _WIN32
	return "simple-memo.exe"; // 兜底：当前目录相对名
#else
	return "./simple-memo";
#endif
}

// 检查 GitHub 最新 release：0=成功；1=网络/解析失败；2=有 release 但没有可执行资产
static int fetch_latest_release(string& tag, string& asset_url, string& asset_name)
{
	string resp = http_get(string("https://api.github.com/repos/") + MEMO_REPO + "/releases/latest");
	if (resp.empty()) return 1;
	Json j = json_parse(resp);
	Json* t = j.find("tag_name");
	Json* assets = j.find("assets");
	if (!t || t->t != Json::Str || !assets || assets->t != Json::Arr) return 1;
	tag = t->s;
	// 优先精确本平台资产名（Win: simple-memo.exe；Unix: simple-memo），其次同前缀的其他二进制资产
#ifdef _WIN32
	const string exact = "simple-memo.exe";
#else
	const string exact = "simple-memo";
#endif
	for (auto& a : assets->arr)
	{
		Json* n = a.find("name");
		Json* u = a.find("browser_download_url");
		if (!n || !u || n->t != Json::Str || u->t != Json::Str) continue;
		if (n->s == exact) { asset_url = u->s; asset_name = n->s; return 0; }
	}
	for (auto& a : assets->arr)
	{
		Json* n = a.find("name");
		Json* u = a.find("browser_download_url");
		if (!n || !u || n->t != Json::Str || u->t != Json::Str) continue;
		const string& s = n->s;
#ifdef _WIN32
		if (s.size() > 4 && s.substr(s.size() - 4) == ".exe")
#else
		// Unix 二进制无后缀（simple-memo-linux / simple-memo-macos 等），排除带扩展名的附带文件
		if (s.find("simple-memo") == 0 && s.find('.') == string::npos)
#endif
		{ asset_url = u->s; asset_name = n->s; return 0; }
	}
	return 2;
}

static int cmd_update(vector<string>& args)
{
	bool force = false;
	for (auto& a : args) if (a == "-f" || a == "--force") force = true;

	printf(tr("正在检查更新（%s，当前 %s）...\n", "Checking for updates (%s, current %s)...\n"), MEMO_REPO, MEMO_VERSION);
	string tag, url, asset;
	int fetch = fetch_latest_release(tag, url, asset);
	if (fetch == 1)
	{
		printf(tr("无法获取最新版本信息（网络？请开代理/WARP 后重试）。\n", "Failed to fetch release info (network? try WARP/proxy).\n"));
		return 1;
	}
	if (fetch == 2)
	{
		printf(tr("最新 release %s 没有可执行资产（simple-memo.exe），无法自动更新。\n",
		          "Latest release %s has no executable asset (simple-memo.exe); cannot auto-update.\n"), tag.c_str());
		return 1;
	}

	printf(tr("当前 %s → 最新 %s\n", "Current %s → latest %s\n"), MEMO_VERSION, tag.c_str());
	int cmp = ver_cmp(MEMO_VERSION, tag);
	if (cmp >= 0 && !force) // 相同或本地更新（远端未发布新 tag）→ 无需更新
	{
		printf(tr("已是最新版本（--force 可强制重装）。\n", "Already up to date (use --force to reinstall).\n"));
		return 0;
	}
	if (cmp == 0 && force) printf(tr("版本相同，--force 强制重装。\n", "Same version; --force reinstalling.\n"));

	ensure_data_dir();
	string tmp = data_dir() + "/.update-download.bin";
	printf(tr("下载 %s ...\n", "Downloading %s ...\n"), url.c_str());
	if (!curl_download(url, tmp))
	{
		printf(tr("下载失败（github.com 可能被墙，请开代理/WARP 后重试）。\n", "Download failed (github.com may be blocked; try WARP/proxy).\n"));
		return 1;
	}

	// 校验：非空且 Windows PE 头为 MZ（防下载到 HTML 错误页）
	{
		ifstream f(tmp, ios::binary);
		char head[2] = { 0, 0 };
		streamsize sz = 0;
		if (f) { f.seekg(0, ios::end); sz = f.tellg(); f.seekg(0); f.read(head, 2); }
#ifdef _WIN32
		bool ok = sz > 100000 && head[0] == 'M' && head[1] == 'Z';
#else
		(void)head;
		bool ok = sz > 100000;
#endif
		if (!ok)
		{
			printf(tr("下载的文件不像可执行程序（%lld 字节），已中止。\n", "Downloaded file does not look like an executable (%lld bytes); aborted.\n"), (ll)sz);
			error_code ec;
			filesystem::remove(tmp, ec);
			return 1;
		}
	}

	// 自替换：运行中的 exe 不能直接覆盖，但可改名 → 旧文件挪走，新文件就位
	string self = current_exe_path();
	string old = self + ".old";
	error_code ec;
	filesystem::remove(old, ec);
	filesystem::rename(self, old, ec);
	if (ec)
	{
		printf(tr("无法替换 %s（权限？手动放置 %s 后重试）。\n", "Cannot replace %s (permission? place %s manually and retry).\n"), self.c_str(), tmp.c_str());
		return 1;
	}
	filesystem::copy_file(tmp, self, filesystem::copy_options::overwrite_existing, ec);
	filesystem::remove(tmp, ec);
	if (ec)
	{
		printf(tr("写入新版本失败：%s。旧版本已保留在 %s。\n", "Failed to write the new version: %s. The old one is kept at %s.\n"), ec.message().c_str(), self.c_str());
		return 1;
	}
#ifndef _WIN32
	// 下载的文件无执行位 → 补 chmod +x，否则替换后无法运行
	(void)system(("chmod +x \"" + self + "\" 2>/dev/null").c_str());
#endif

	// 清理旧文件：运行中删不掉则后台延迟删除
	ec.clear();
	filesystem::remove(old, ec);
	if (ec)
	{
#ifdef _WIN32
		string d = "cmd /c ping -n 3 127.0.0.1 >nul & del \"" + old + "\" >nul 2>nul";
		system(d.c_str());
#endif
		printf(tr("旧版本 %s.old 将在退出后自动删除。\n", "The old version %s.old will be deleted after exit.\n"), self.c_str());
	}

	printf(tr("已更新到 %s（%s）。REPL 中请重启后生效。\n", "Updated to %s (%s). Restart to take effect if in REPL.\n"), tag.c_str(), self.c_str());
	return 0;
}

static void print_help()
{
	if (lang_is_en())
	{
		printf(
			"Simple Memo — command help (version %s)\n"
			"\n"
			"  add <title> [body...]\n"
			"      Add a memo. Blank lines in the body split it into paragraphs; each paragraph\n"
			"      gets its own vector. A single newline stays inside the paragraph. In REPL,\n"
			"      omitting the body enters multi-line input; a lone :end line finishes.\n"
			"  edit <id>\n"
			"      Edit title and body of <id> in an external editor; vectors are recomputed on save.\n"
			"      Editor chain: config editor / $EDITOR > $VISUAL > platform default (Win: edit→notepad;\n"
			"      Linux/macOS: nano→vi). First line of the file is the title, then one ==== line, then the body.\n"
			"  delete <id>   Delete a memo (aliases: del / rm)\n"
			"\n"
			"  list   List all memos (alias: ls)\n"
			"  show <id>   Show the full content of <id>\n"
			"  show -f/--follow   Show the last active memo (add / edit / first search hit / first find hit)\n"
			"\n"
			"  search <keywords...> [--any] [-l|--list]\n"
			"      Multi-keyword substring search, case-insensitive.\n"
			"      <keywords...>  one or more keywords\n"
			"      --any          switch to OR: match if any keyword hits (default AND: all must hit)\n"
			"      -l/--list      titles only; by default prints hit lines ±2 lines of context (hit line marked <-)\n"
			"  find <semantic query> [-nK] [-l|--list]\n"
			"      Vector semantic search, matched per paragraph.\n"
			"      <semantic query>  describe what you are looking for in natural language\n"
			"      -nK               show top K results only (e.g. -n3; default 5)\n"
			"      -l/--list         titles only; by default prints the matched paragraph (truncated at 18 chars)\n"
			"\n"
			"  reindex [--all]   Recompute vectors (default: pending only; --all rebuilds everything)\n"
			"  stop              Stop the background llama-server to free memory\n"
			"  update [-f]       Self-update from GitHub Releases (-f/--force reinstall even if same version)\n"
			"  config            Interactive config (Enter=keep, new value=change, '-'=reset to default)\n"
			"  config get [key]  View config; config set <key> <value> to set; config unset <key> to remove\n"
			"      Keys: hf_endpoint / embed_hf_repo / embed_hf_file / server_port / llama_server_exe / editor / language\n"
			"\n"
			"  help / ?   Show this help\n"
			"  quit / exit / q   Exit (REPL only)\n"
			"\n"
			"Environment variables:\n"
			"  MEMO_DATA_DIR         Data directory (default ~/.simple_memo)\n"
			"  MEMO_LLAMA_SERVER_EXE Path to llama-server(.exe)\n"
			"  MEMO_SERVER_PORT      Local embedding port (default 8732)\n"
			"  MEMO_EMBED_HF_REPO    Embedding model HF repo\n"
			"  MEMO_EMBED_HF_FILE    Embedding model GGUF file name\n"
			"  MEMO_HF_ENDPOINT      HF mirror endpoint (empty = official)\n"
			"  MEMO_LANG             UI language (zh / en, default zh)\n"
			"\n"
			"  EDITOR / VISUAL       External editor for memos (default: Win=edit→notepad; Linux/macOS=nano→vi)\n",
			MEMO_VERSION);
		return;
	}
	printf(
		"简单备忘录 —— 命令帮助（版本 %s）\n"
		"\n"
		"  add <标题> [正文...]\n"
		"      新增一条备忘录。正文中的空行会把内容分成多个段落，每个段落独立计算向量；\n"
		"      单个换行不换段。REPL 模式下若省略正文，则进入多行输入，单独一行 :end 结束。\n"
		"  edit <id|标题片段>\n"
		"      用外部编辑器修改标题和正文，保存关闭后自动重算向量。\n"
		"      编辑器顺序：config editor / $EDITOR > $VISUAL > 平台默认（Win: edit→notepad；Linux/macOS: nano→vi）。\n"
		"      文件首行为标题，其后一行 ==== 分隔正文。\n"
		"  delete <id|标题片段> [-y]   删除（需确认，-y 跳过；别名：del / rm）\n"
		"\n"
		"  list   列出全部备忘录（别名：ls）\n"
		"  show <id|标题片段> [选项]\n"
		"      查看内容。选项：\n"
		"      -f/--follow    显示上一个活跃的备忘录（add / edit / search 首命中 / find 首命中）\n"
		"      -m/--markdown  渲染 Markdown（标题 / 粗体 / 斜体 / 代码 / 列表 / 引用）\n"
		"      --limit=<n>L|P 只看前 n 行（L）或前 n 段（P）\n"
		"      --from=<n>L|P  从第 n 行 / 段开始（1 起）\n"
		"      --all          显示全部（过大时仍设上限，尽可能多显示）\n"
		"      备忘录过大时默认只显示一部分；未显示完全会在首尾提示省略了多少。\n"
		"      <id|标题片段>：纯数字按 id 查找；否则按标题片段（不区分大小写，须唯一命中）。\n"
		"      edit / delete 同样支持。\n"
		"\n"
		"  search <关键词...> [--any] [-l|--list]\n"
		"      多关键词子串搜索，不区分大小写。\n"
		"      <关键词...>  一个或多个关键词\n"
		"      --any        改为 OR：命中任意一个关键词即可（默认 AND，须全部命中）\n"
		"      -l/--list    只列标题；默认输出命中行 ±2 行上下文（命中行标 <-）\n"
		"  find <语义查询> [-nK] [-l|--list]\n"
		"      向量语义搜索，按段落匹配，找语义最接近的备忘录段落。\n"
		"      <语义查询>   用自然语言描述要找的内容\n"
		"      -nK          只显示前 K 条（如 -n3；默认 5）\n"
		"      -l/--list    只列标题；默认输出匹配段落（长于 18 字则截断）\n"
		"\n"
		"  reindex [--all]   重算向量（默认只补算 pending；--all 全量重建）\n"
		"  stop              停止后台 llama-server，释放内存\n"
		"  update [-f]       从 GitHub Release 自更新（-f/--force 强制替换，即使版本相同）\n"
		"  config            交互式配置（回车保持 / 输入新值 / 输入 - 恢复默认）\n"
		"  config get [key]  查看配置；config set <key> <value> 设置；config unset <key> 移除\n"
		"      可配置项：hf_endpoint / embed_hf_repo / embed_hf_file / server_port / llama_server_exe / editor / language\n"
		"\n"
		"  help / ?   显示本帮助\n"
		"  quit / exit / q   退出（仅 REPL 模式）\n"
		"\n"
		"环境变量：\n"
		"  MEMO_DATA_DIR         数据目录（默认 ~/.simple_memo）\n"
		"  MEMO_LLAMA_SERVER_EXE llama-server 路径\n"
		"  MEMO_SERVER_PORT      本地 embedding 端口（默认 8732）\n"
		"  MEMO_EMBED_HF_REPO    嵌入模型 HF 仓库\n"
		"  MEMO_EMBED_HF_FILE    嵌入模型 GGUF 文件名\n"
		"  MEMO_HF_ENDPOINT      HF 镜像地址（国内加速，置空使用官方）\n"
		"  MEMO_LANG             界面语言（zh / en，默认 zh）\n"
		"\n"
		"  EDITOR / VISUAL      编辑备忘录使用的外部编辑器（默认：Win=edit→notepad；Linux/macOS=nano→vi）\n",
		MEMO_VERSION);
}

static int dispatch(vector<string>& args)
{
	if (args.empty()) return 0;
	string& c = args[0];
	vector<string> rest(args.begin() + 1, args.end());
	if (c == "add") return cmd_add(rest);
	if (c == "list" || c == "ls") return cmd_list(rest);
	if (c == "show") return cmd_show(rest);
	if (c == "search") return cmd_search(rest);
	if (c == "find") return cmd_find(rest);
	if (c == "delete" || c == "del" || c == "rm") return cmd_delete(rest);
	if (c == "edit") return cmd_edit(rest);
	if (c == "reindex") return cmd_reindex(rest);
	if (c == "config") return cmd_config(rest);
	if (c == "stop") return cmd_stop(rest);
	if (c == "update") return cmd_update(rest);
	if (c == "help" || c == "?") { print_help(); return 0; }
	if (c == "quit" || c == "exit" || c == "q") { printf(tr("（quit 仅在 REPL 下使用）\n", "(quit is REPL-only)\n")); return 0; }
	printf(tr("未知命令：%s（输入 help 查看）\n", "Unknown command: %s (type help)\n"), c.c_str());
	return 1;
}

int main(int argc, char** argv)
{
#ifdef _WIN32
	SetConsoleOutputCP(65001);
	SetConsoleCP(65001);
	// 开启 VT 处理 → show -m 的 ANSI 颜色可用；重定向/老终端则自动退回纯文本
	{
		HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
		DWORD mode = 0;
#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif
		if (GetConsoleMode(h, &mode))
			g_ansi_ok = SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
		else g_ansi_ok = false;
	}
#else
	setlocale(LC_ALL, "");
#endif
	ensure_data_dir();

	// 清理上次异常退出残留的编辑临时文件（.edit-<id>.txt）
	{
		error_code ec;
		for (auto& e : filesystem::directory_iterator(data_dir(), ec))
		{
			string nm = e.path().filename().string();
			if (nm.size() > 10 && nm.substr(0, 6) == ".edit-" && nm.substr(nm.size() - 4) == ".txt")
				filesystem::remove(e.path(), ec);
		}
	}

	// 启动时扫描 env 与 config 冲突：REPL 交互式，子命令非交互（值相同自动删 config）
	scan_config_conflicts(argc <= 1);

	// 子命令模式：直接执行
	if (argc > 1)
	{
		string line;
		for (int i = 1; i < argc; i++) { if (i > 1) line += " "; line += argv[i]; }
		auto toks = tokenize(line);
		return dispatch(toks);
	}

	// REPL 模式（先取锁：防两个实例并发写坏数据）
	g_repl = true;
	if (!acquire_lock()) return 1;
	int dv = vec_schema_version_on_disk();
	if (dv != 0 && dv < (int)VEC_SCHEMA_VERSION)
		printf(tr("提示：向量格式已更新（旧 v%d → 新 v%d），请运行 reindex --all 重算。\n",
		          "Note: vector format updated (old v%d → new v%d); run reindex --all to rebuild.\n"),
		       dv, (int)VEC_SCHEMA_VERSION);
	printf(tr("简单备忘录 REPL（输入 help 查看，quit 退出）\n", "Simple Memo REPL (type help for usage, quit to exit)\n"));
	string line;
	while (true)
	{
		printf("memo> ");
		fflush(stdout);
		if (!getline(cin, line)) { printf("\n"); break; }
		auto toks = tokenize(line);
		if (toks.empty()) continue;
		if (toks[0] == "quit" || toks[0] == "exit" || toks[0] == "q") break;
		dispatch(toks);
	}
	memo_clear(); // 退出 REPL 清除 --follow 追踪
	release_lock();
	return 0;
}
