// 简单备忘录：标题+文本，多关键词搜索 + 向量语义搜索
// 嵌入由本地 llama.cpp（llama-embedding 子进程 + embeddinggemma-300m）提供，首次自动下载
// 编译：g++.exe -Wall -Wextra -g3 -O2 -std=c++17 "简单备忘录.cpp" -o "output/简单备忘录.exe"

#include <bits/stdc++.h>
#include <cstdio>
#include <filesystem>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

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
static string llama_server_default() { return llama_dir() + "/llama-server.exe"; }

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

static string config_file() { return data_dir() + "/config.json"; }
static const char* IGNORE_ENV_KEY = "@ignore_env";

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

// 配置项元数据（供 config 引导式交互 / get 显示）
struct CfgItem { const char* key; const char* env; const char* desc; string(*def)(); };
static string cfg_def_hf_endpoint() { return "https://hf-mirror.com"; }
static string cfg_def_embed_repo() { return "ggml-org/embeddinggemma-300M-GGUF"; }
static string cfg_def_embed_file() { return "embeddinggemma-300M-Q8_0.gguf"; }
static string cfg_def_port() { return "8732"; }
static string cfg_def_llama() { return llama_server_default(); }
static string cfg_def_editor() { return ""; }
static const CfgItem g_cfg_items[] = {
	{ "hf_endpoint", "MEMO_HF_ENDPOINT", "HF 镜像地址（国内加速，置空用官方）", cfg_def_hf_endpoint },
	{ "embed_hf_repo", "MEMO_EMBED_HF_REPO", "嵌入模型 HF 仓库", cfg_def_embed_repo },
	{ "embed_hf_file", "MEMO_EMBED_HF_FILE", "嵌入模型 GGUF 文件名", cfg_def_embed_file },
	{ "server_port", "MEMO_SERVER_PORT", "本地 embedding 服务端口", cfg_def_port },
	{ "llama_server_exe", "MEMO_LLAMA_SERVER_EXE", "llama-server.exe 路径", cfg_def_llama },
	{ "editor", "EDITOR", "外部编辑器命令（可带参数，空=notepad）", cfg_def_editor },
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
				printf("启动扫描：%s 的 config 与环境变量值相同，已自动删除冗余 config 项。\n", it.key);
			continue;
		}

		// 值不同 → 交互式二选一
		if (!interactive) continue;
		printf("⚠ 启动扫描发现冲突：%s（%s）\n", it.key, it.desc);
		printf("    环境变量 %s = [%s]\n", it.env, env.c_str());
		printf("    config  %s = [%s]\n", it.key, cfg.c_str());
		printf("    [1] 生效 config（标记其优先于环境变量）\n");
		printf("    [2] 生效环境变量（删除 config 项）\n");
		printf("  请选择 [1/2]（默认 1）：");
		fflush(stdout);
		string choice;
		getline(cin, choice);
		choice = trim(choice);
		if (choice == "2")
		{
			m.erase(it.key);
			cfg_set_ignore_env(m, it.key, false);
			printf("    已删除 config.%s，环境变量生效。\n", it.key);
		}
		else
		{
			cfg_set_ignore_env(m, it.key, true);
			printf("    已标记 config.%s 优先。\n", it.key);
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

// 大小写不敏感：仅对 ASCII 字母生效，UTF-8 中文字节(>=0x80)在 "C" locale 下保持不变
static string lower(const string& s)
{
	string r;
	r.reserve(s.size());
	for (unsigned char c : s) r += (char)tolower(c);
	return r;
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
// GET 取文本响应（用于查 GitHub release API）
// 用 system 重定向到文件再读，避免 MinGW _popen 管道读取问题
static string http_get(const string& url)
{
	ensure_data_dir();
	string out = data_dir() + "/.get.txt";
	string cmd = "curl -s -L -m 60 -A \"simple-memo\" \"" + url + "\" > \"" + out + "\" 2>nul";
	system(cmd.c_str());
	string resp;
	ifstream f(out, ios::binary);
	if (f) resp = string((istreambuf_iterator<char>(f)), istreambuf_iterator<char>());
	return resp;
}

// GET 下载到文件（用于下载 llama.cpp release zip）
static bool curl_download(const string& url, const string& dest)
{
	string cmd = "curl -s -L -m 600 --retry 2 -A \"simple-memo\" -o \"" + dest + "\" \"" + url + "\"";
	int rc = system(cmd.c_str());
	if (rc != 0) return false;
	ifstream f(dest, ios::binary | ios::ate);
	return f && f.tellg() > 0;
}

// ===================== Embedding（llama.cpp 子进程） =====================
static string g_last_err;

// 查 llama.cpp 最新 release 的 Windows AVX2 zip 下载地址
static string find_llama_release_url()
{
	string resp = http_get("https://api.github.com/repos/ggml-org/llama.cpp/releases/latest");
	if (resp.empty()) return "";
	Json j = json_parse(resp);
	Json* assets = j.find("assets");
	if (!assets || assets->t != Json::Arr) return "";
	string avx2, fallback;
	for (auto& a : assets->arr)
	{
		Json* name = a.find("name");
		Json* url = a.find("browser_download_url");
		if (!name || !url || url->t != Json::Str) continue;
		// 新版 llama.cpp 用 bin-win-cpu-x64（旧版 bin-win-avx2-x64）
		const string& n = name->s;
		if (n.find("bin-win-cpu-x64") != string::npos || n.find("bin-win-avx2-x64") != string::npos) avx2 = url->s;
		else if (n.find("bin-win-x64") != string::npos && fallback.empty()) fallback = url->s;
	}
	return !avx2.empty() ? avx2 : fallback;
}

// 验证 llama-server.exe 可正常执行（--version 打印版本后退出，不启动 server）
static bool llama_server_works()
{
	string cmd = "\"" + llama_server_path() + "\" --version > nul 2>nul";
	return system(cmd.c_str()) == 0;
}

// 确保 llama-server.exe 可用；已存在但损坏则清理重下（自愈）。失败设 g_last_err
static bool ensure_llama_binary()
{
	// 已存在且可执行 → 跳过
	if (llama_server_works()) return true;

	// 用户用环境变量 / config 显式指定却不可用 → 不自动重下，直接报错
	string explicit_exe = get_effective("llama_server_exe", "MEMO_LLAMA_SERVER_EXE", "");
	bool user_set = !explicit_exe.empty() && explicit_exe != llama_server_default();
	if (user_set)
	{
		g_last_err = string("llama-server.exe 指定的文件不可用: ") + llama_server_path();
		return false;
	}

	// 默认位置存在但损坏/不完整 → 清理后重下（自愈）
	error_code ec;
	if (filesystem::exists(llama_server_default(), ec))
	{
		printf("  llama-server.exe 不可用（损坏/不完整？），清理后重新下载...\n");
		filesystem::remove_all(llama_dir(), ec);
	}

	ensure_data_dir();
	printf("首次使用：正在获取 llama.cpp（下载 + 解压，请耐心等待）...\n");

	string url = find_llama_release_url();
	if (url.empty()) { g_last_err = "无法获取 llama.cpp 最新 release 地址（网络？请开 WARP/代理）"; return false; }

	string zip = data_dir() + "/llama-release.zip";
	printf("  下载 %s\n", url.c_str());
	if (!curl_download(url, zip))
	{
		g_last_err = "下载 llama.cpp release 失败（github.com 可能被墙，请开 WARP/代理后重试）";
		return false;
	}

	filesystem::create_directories(llama_dir(), ec);
	string untar = "tar -xf \"" + zip + "\" -C \"" + llama_dir() + "\"";
	int rc = system(untar.c_str());
	filesystem::remove(zip, ec);
	if (rc != 0) { g_last_err = "解压 llama.cpp release 失败"; return false; }

	if (!llama_server_works()) { g_last_err = "下载后 llama-server.exe 仍不可用（zip 可能损坏，请重试）"; return false; }
	printf("  llama.cpp 就绪。\n");
	return true;
}

// POST JSON 到本地 llama-server
static string http_post_json(const string& url, const string& body)
{
	ensure_data_dir();
	string bf = data_dir() + "/.body.json";
	{ ofstream f(bf, ios::binary); f << body; }
	string out = data_dir() + "/.post.txt";
	string cmd = "curl -s -m 60 -X POST -H \"Content-Type: application/json\" --data-binary \"@" + bf + "\" \"" + url + "\" > \"" + out + "\" 2>nul";
	system(cmd.c_str());
	string resp;
	ifstream f(out, ios::binary);
	if (f) resp = string((istreambuf_iterator<char>(f)), istreambuf_iterator<char>());
	return resp;
}

// llama-server 是否就绪（/health 返回 HTTP 200；模型加载中是 503）
static bool server_health()
{
	ensure_data_dir();
	string codef = data_dir() + "/.code.txt";
	string cmd = "curl -s -m 10 -w \"%{http_code}\" -o nul \"http://127.0.0.1:" + to_string(server_port()) + "/health\" > \"" + codef + "\" 2>nul";
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
	if (!hfEp.empty()) _putenv(("HF_ENDPOINT=" + hfEp).c_str());

	printf("  启动 llama-server（首次下载+加载模型，可能需几分钟）...\n");
	ensure_data_dir();
	string logf = data_dir() + "/server.log";
	string cmd = "start \"memo-embd\" /B \"" + llama_server_path() + "\""
		+ " --embedding"
		+ " --host 127.0.0.1 --port " + to_string(server_port())
		+ " -hf \"" + embed_hf_repo() + "\""
		+ " --hf-file \"" + embed_hf_file() + "\""
		+ " > nul 2> \"" + logf + "\"";
	system(cmd.c_str());

	for (int i = 0; i < 600; i++) // 最多等 300 秒（首次模型下载 + 加载）
	{
		Sleep(500);
		if (server_health()) { printf("  llama-server 就绪。\n"); return true; }
	}
	// 超时：dump server.log 末尾帮助诊断
	g_last_err = "llama-server 启动超时（首次下载模型较久，请重试）。server.log 末尾：\n";
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
	system("taskkill /IM llama-server.exe /F >nul 2>nul");
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
		if (g_last_err.empty()) g_last_err = "llama-server 不可用";
		return {};
	}

	string body = "{\"input\":" + json_escape(text) + "}";
	string resp = http_post_json("http://127.0.0.1:" + to_string(server_port()) + "/v1/embeddings", body);
	if (resp.empty()) { g_last_err = "embedding 请求无响应"; return {}; }

	// 解析 OpenAI 兼容 JSON（兼容 data[0].embedding 和顶层 embedding）
	Json j = json_parse(resp);
	Json* emb = nullptr;
	Json* data = j.find("data");
	if (data && data->t == Json::Arr && !data->arr.empty())
		emb = data->arr[0].find("embedding");
	if (!emb) emb = j.find("embedding");
	if (!emb || emb->t != Json::Arr)
	{
		g_last_err = "响应缺少 embedding 数组（原始: " + resp.substr(0, 200) + ")";
		return {};
	}
	vector<float> v;
	v.reserve(emb->arr.size());
	for (auto& x : emb->arr) if (x.t == Json::Num) v.push_back((float)x.num);
	if (v.empty()) g_last_err = "embedding 数组为空";
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
	ofstream f(memos_path(), ios::binary | ios::trunc);
	for (auto& m : ms)
	{
		f << "{\"id\":" << m.id
		  << ",\"title\":" << json_escape(m.title)
		  << ",\"body\":" << json_escape(m.body)
		  << ",\"ctime\":" << json_escape(m.ctime)
		  << ",\"vec_ok\":" << (m.vec_ok ? "true" : "false")
		  << "}\n";
	}
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
	ofstream f(vectors_path(), ios::binary | ios::trunc);
	f.write(VEC_MAGIC, 4);
	f.write((const char*)&VEC_SCHEMA_VERSION, sizeof(VEC_SCHEMA_VERSION));
	for (auto& kv : vs)
	{
		uint64_t id = (uint64_t)kv.first;
		uint32_t np = (uint32_t)kv.second.size();
		f.write((const char*)&id, sizeof(id));
		f.write((const char*)&np, sizeof(np));
		for (auto& v : kv.second)
		{
			uint32_t dim = (uint32_t)v.size();
			f.write((const char*)&dim, sizeof(dim));
			if (dim) f.write((const char*)v.data(), (streamsize)dim * sizeof(float));
		}
	}
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
static ll g_last_active_id = 0; // 上一个活跃的备忘录（add/edit/search 首命中/find 首命中/show）

static int cmd_add(vector<string>& args)
{
	if (args.empty()) { printf("用法: add <标题> [正文...]\n"); return 1; }
	string title = args[0];
	string body;
	for (size_t i = 1; i < args.size(); i++)
	{
		if (i > 1) body += " ";
		body += args[i];
	}
	if (body.empty() && g_repl)
	{
		printf("请输入正文（单独一行输入 :end 或 end 结束；或按 Ctrl+Z 回车结束）：\n");
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
	g_last_active_id = id; // 新增即为上一个活跃备忘录
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
		printf("已新增 #%-4lld（向量已生成，%d 段，dim=%zu）\n", id, ok_cnt, set[0].size());
	}
	else
	{
		m.vec_ok = false;
		printf("已新增 #%-4lld（警告：embedding 失败 %d 段，标记 pending）\n", id, fail_cnt);
		if (!g_last_err.empty()) printf("  原因：%s\n", g_last_err.c_str());
		printf("  解决后运行 reindex 补算。\n");
	}
	ms.push_back(m);
	save_memos(ms);
	save_vectors(vs);
	return 0;
}

static int cmd_list(vector<string>&)
{
	auto ms = load_memos();
	if (ms.empty()) { printf("（空，还没有备忘录）\n"); return 0; }
	printf("共 %zu 条：\n", ms.size());
	for (auto& m : ms)
	{
		printf("#%-4lld [%s] %s", m.id, m.ctime.c_str(), m.title.c_str());
		if (!m.vec_ok) printf("  (无向量)");
		printf("\n");
	}
	return 0;
}

static int cmd_show(vector<string>& args)
{
	bool follow = false;
	bool has_id = false;
	ll id = 0;
	for (auto& a : args)
	{
		if (a == "-f" || a == "--follow") follow = true;
		else { id = atoll(a.c_str()); has_id = true; }
	}
	if (!has_id && !follow) { printf("用法: show <id> [-f|--follow]\n"); return 1; }
	if (follow && !has_id)
	{
		if (g_last_active_id == 0) { printf("（还没有活跃备忘录：先 add / edit / search / find）\n"); return 0; }
		id = g_last_active_id;
	}

	auto ms = load_memos();
	for (auto& m : ms) if (m.id == id)
	{
		g_last_active_id = id;
		printf("#%lld  %s\n", m.id, m.title.c_str());
		printf("  时间: %s   向量: %s\n", m.ctime.c_str(), m.vec_ok ? "已生成" : "缺失");
		printf("  正文:\n%s\n", m.body.c_str());
		return 0;
	}
	printf("找不到 #%lld\n", id);
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
	if (kws.empty()) { printf("用法: search <关键词...> [--any] [-l|--list]\n"); return 1; }

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
	if (hits.empty()) { printf("未匹配任何备忘录。\n"); return 0; }
	g_last_active_id = hits[0]->id; // search 首个匹配即为上一个活跃备忘录

	printf("命中 %zu 条（%s）：\n", hits.size(), any ? "OR" : "AND");
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
	if (parts.empty()) { printf("用法: find <语义查询> [-nK] [-l|--list]\n"); return 1; }

	string q;
	for (size_t i = 0; i < parts.size(); i++) { if (i) q += " "; q += parts[i]; }

	auto qv = embed_text(query_prompt(q));
	if (qv.empty())
	{
		printf("无法向量化查询，语义搜索中止。\n  原因：%s\n", g_last_err.c_str());
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
		printf("没有可语义搜索的备忘录（可能都 pending）。运行 reindex 补算。\n");
		return 0;
	}
	sort(ranked.begin(), ranked.end(), [](auto& a, auto& b) { return a.score > b.score; });
	g_last_active_id = ranked[0].m->id; // find 第一匹配即为上一个活跃备忘录

	int shown = min((int)ranked.size(), n > 0 ? n : 5);
	printf("Top-%d 语义匹配：\n", shown);
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
			if (preview.empty()) preview = "(仅标题)";
			printf("  %5.1f%%  #%-4lld %s  ── %s\n", h.score * 100.0, h.m->id, h.m->title.c_str(), preview.c_str());
		}
	}
	return 0;
}

static int cmd_delete(vector<string>& args)
{
	if (args.empty()) { printf("用法: delete <id>\n"); return 1; }
	ll id = atoll(args[0].c_str());
	auto ms = load_memos();
	bool found = false;
	vector<Memo> ns;
	ns.reserve(ms.size());
	for (auto& m : ms)
	{
		if (m.id == id) found = true;
		else ns.push_back(m);
	}
	if (!found) { printf("找不到 #%lld\n", id); return 1; }
	save_memos(ns);

	auto vs = load_vectors();
	vs.erase(id);
	save_vectors(vs);
	printf("已删除 #%lld\n", id);
	return 0;
}

// 编辑器命令：config.editor / $EDITOR > $VISUAL > ""（空表示用 notepad）
static string resolve_editor()
{
	string v = get_effective("editor", "EDITOR", "");
	if (!v.empty()) return v;
	return get_env("VISUAL", "");
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
	if (args.empty()) { printf("用法: edit <id>\n"); return 1; }
	ll id = atoll(args[0].c_str());
	auto ms = load_memos();
	for (auto& m : ms) if (m.id == id)
	{
		g_last_active_id = id; // 被编辑即为上一个活跃备忘录
		ensure_data_dir();
		string tmp = data_dir() + "/.edit-" + to_string(id) + ".txt";
		string editor = resolve_editor();

		for (;;)
		{
			write_edit_file(tmp, m.title, m.body);   // 每次重试重置为当前内容

			string cmd = editor.empty()
				? ("notepad \"" + tmp + "\"")
				: (editor + " \"" + tmp + "\"");      // 允许 $EDITOR 带参数
			int rc = system(cmd.c_str());
			if (rc != 0 && !editor.empty())
			{
				printf("  启动 %s 失败，回退到 notepad。\n", editor.c_str());
				editor = "";
				rc = system(("notepad \"" + tmp + "\"").c_str());
			}

			// 统一等待：不依赖编辑器是否阻塞
			printf("  编辑完成、保存并关闭编辑器后，请按回车继续……\n");
			fflush(stdout);
			string dummy;
			getline(cin, dummy);

			string nt, nb;
			bool ok = parse_edit_file(tmp, nt, nb);
			if (ok && !nt.empty())
			{
				filesystem::remove(tmp);
				if (nt == m.title && nb == m.body) { printf("#%lld 未改动。\n", id); return 0; }
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
					printf("已编辑 #%lld 并重算向量（%d 段）。\n", id, ok_cnt);
				}
				else
				{
					m.vec_ok = false;
					save_memos(ms);
					save_vectors(vs);
					printf("已编辑 #%lld（embedding 失败 %d 段，标记 pending", id, fail_cnt);
					if (!g_last_err.empty()) printf("：%s", g_last_err.c_str());
					printf("）\n");
				}
				return 0;
			}
			// 格式非法：报错，重置并重试
			printf("  编辑结果无效（%s），已恢复原内容并重新打开编辑器。\n",
				ok ? "标题为空" : "缺少 ==== 分隔行");
		}
	}
	printf("找不到 #%lld\n", id);
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
			else { done = false; printf("  #%lld 失败：%s\n", m.id, g_last_err.c_str()); }
		}
		if (done && !set.empty()) { vs[m.id] = set; m.vec_ok = true; ok++; }
		else m.vec_ok = false;
	}
	save_memos(ms);
	save_vectors(vs);
	if (force_all) printf("检测到旧向量格式 v%d，已按全量重建。\n", dv);
	if (cnt == 0) printf("没有需要补算的（全部已有向量）。加 --all 可全量重建。\n");
	else printf("补算完成：%d/%d 条成功（共 %d 段）。\n", ok, cnt, total_para);
	return 0;
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
		string src = ignored ? "config(优先)" : (!env.empty() ? "环境变量" : (!cfg.empty() ? "config" : "默认"));
		printf("  %-16s %s\n", it.key, it.desc);
		printf("      环境变量 %-16s =[%s]\n", it.env, env.c_str());
		printf("      config  %-16s =[%s]%s\n", it.key, cfg.c_str(), ignored ? "  (优先于环境变量)" : "");
		printf("      默认 =[%s]   生效=[%s]  来源:%s\n", it.def().c_str(), eff.c_str(), src.c_str());
	};

	// config get [key]
	if (!args.empty() && args[0] == "get")
	{
		if (args.size() == 1)
		{
			printf("当前配置（配置文件 %s）：\n", config_file().c_str());
			for (auto& it : g_cfg_items) { show_one(it); printf("\n"); }
		}
		else
		{
			const CfgItem* p = cfg_find(args[1]);
			if (!p) { printf("未知配置项：%s（可用：", args[1].c_str()); for (auto& it : g_cfg_items) printf("%s ", it.key); printf("）\n"); return 1; }
			show_one(*p);
		}
		return 0;
	}

	// config set <key> <value>
	if (!args.empty() && args[0] == "set")
	{
		if (args.size() < 3) { printf("用法: config set <key> <value>\n"); return 1; }
		const CfgItem* p = cfg_find(args[1]);
		if (!p) { printf("未知配置项：%s（可用：", args[1].c_str()); for (auto& it : g_cfg_items) printf("%s ", it.key); printf("）\n"); return 1; }
		string value = args[2];
		for (size_t i = 3; i < args.size(); i++) { value += " "; value += args[i]; }
		auto m = load_config_map();
		m[p->key] = value;
		if (!env_of(*p).empty())
		{
			cfg_set_ignore_env(m, p->key, true);
			printf("注意：环境变量 %s 仍存在，已标记 %s 优先于环境变量。\n", p->env, p->key);
		}
		save_config_map(m);
		printf("已设置 %s = %s\n", p->key, value.c_str());
		return 0;
	}

	// config unset <key>
	if (!args.empty() && args[0] == "unset")
	{
		if (args.size() < 2) { printf("用法: config unset <key>\n"); return 1; }
		const CfgItem* p = cfg_find(args[1]);
		if (!p) { printf("未知配置项：%s\n", args[1].c_str()); return 1; }
		auto m = load_config_map();
		m.erase(p->key);
		cfg_set_ignore_env(m, p->key, false);
		save_config_map(m);
		printf("已移除 %s（回归环境变量/默认）。\n", p->key);
		return 0;
	}

	// 引导式（无参）：逐项显示，回车保持 / 输入新值 / 输入 - 恢复默认
	printf("配置（回车=保持，输入新值=修改，输入 -=恢复默认；文件 %s）：\n\n", config_file().c_str());
	auto m = load_config_map();
	for (auto& it : g_cfg_items)
	{
		string env = env_of(it);
		string cfg = (m.find(it.key) != m.end()) ? m[it.key] : "";
		bool ignored = cfg_ignores_env(m, it.key);

		// 冲突：环境变量与 config 同时存在且未标记 → 立即提示二选一
		if (!env.empty() && !cfg.empty() && !ignored)
		{
			printf("⚠ 冲突：%s（%s）\n", it.key, it.desc);
			printf("    环境变量 %s = [%s]\n", it.env, env.c_str());
			printf("    config  %s = [%s]\n", it.key, cfg.c_str());
			printf("    [1] 生效 config（标记 %s 环境变量优先级下降）\n", it.key);
			printf("    [2] 生效环境变量（删除 config 里的 %s）\n", it.key);
			printf("  请选择 [1/2]（默认 1）：");
			fflush(stdout);
			string choice;
			getline(cin, choice);
			choice = trim(choice);
			if (choice == "2")
			{
				m.erase(it.key);
				cfg_set_ignore_env(m, it.key, false);
				save_config_map(m);
				printf("    已删除 config.%s，环境变量生效。\n", it.key);
			}
			else
			{
				cfg_set_ignore_env(m, it.key, true);
				save_config_map(m);
				printf("    已标记 config.%s 优先。\n", it.key);
			}
			printf("\n");
			continue;
		}

		string eff = get_effective(it.key, it.env, it.def());
		string src = ignored ? "config(优先)" : (!env.empty() ? "环境变量" : (!cfg.empty() ? "config" : "默认"));
		printf("%-16s %s\n", it.key, it.desc);
		printf("    当前=%s  来源:%s  默认=%s\n", eff.c_str(), src.c_str(), it.def().c_str());
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
			printf("    已恢复默认/环境变量。\n");
		}
		else
		{
			m[it.key] = input;
			if (!env.empty()) cfg_set_ignore_env(m, it.key, true);
			save_config_map(m);
			if (!env.empty()) printf("    已设置并标记 config 优先（环境变量 %s 仍存在）。\n", it.env);
			else printf("    已设置。\n");
		}
		printf("\n");
	}
	printf("配置已保存到 %s\n", config_file().c_str());
	return 0;
}

static int cmd_stop(vector<string>&)
{
	kill_server();
	printf("已停止 llama-server。\n");
	return 0;
}

static void print_help()
{
	printf(
		"简单备忘录 —— 命令帮助\n"
		"\n"
		"  add <标题> [正文...]\n"
		"      新增一条备忘录。正文中的空行会把内容分成多个段落，每个段落独立计算向量；\n"
		"      单个换行不换段。REPL 模式下若省略正文，则进入多行输入，单独一行 :end 结束。\n"
		"  edit <id>\n"
		"      用外部编辑器修改指定 id 的标题和正文，保存关闭后自动重算向量。\n"
		"      编辑器顺序：$EDITOR > $VISUAL > notepad。文件首行为标题，其后一行 ==== 分隔正文。\n"
		"  delete <id>   删除指定 id（别名：del / rm）\n"
		"\n"
		"  list   列出全部备忘录（别名：ls）\n"
		"  show <id>   查看指定 id 的完整内容\n"
		"  show -f/--follow   显示上一个活跃的备忘录（add / edit / search 首命中 / find 首命中）\n"
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
		"  config            交互式配置（回车保持 / 输入新值 / 输入 - 恢复默认）\n"
		"  config get [key]  查看配置；config set <key> <value> 设置；config unset <key> 移除\n"
		"      可配置项：hf_endpoint / embed_hf_repo / embed_hf_file / server_port / llama_server_exe / editor\n"
		"\n"
		"  help / ?   显示本帮助\n"
		"  quit / exit / q   退出（仅 REPL 模式）\n"
		"\n"
		"环境变量：\n"
		"  MEMO_DATA_DIR         数据目录（默认 ~/.simple_memo）\n"
		"  MEMO_LLAMA_SERVER_EXE llama-server.exe 路径\n"
		"  MEMO_SERVER_PORT      本地 embedding 端口（默认 8732）\n"
		"  MEMO_EMBED_HF_REPO    嵌入模型 HF 仓库\n"
		"  MEMO_EMBED_HF_FILE    嵌入模型 GGUF 文件名\n"
		"  MEMO_HF_ENDPOINT      HF 镜像地址（国内加速，置空使用官方）\n"
		"\n"
		"  EDITOR / VISUAL      编辑备忘录使用的外部编辑器（默认 notepad）\n");
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
	if (c == "help" || c == "?") { print_help(); return 0; }
	if (c == "quit" || c == "exit" || c == "q") { printf("（quit 仅在 REPL 下使用）\n"); return 0; }
	printf("未知命令：%s（输入 help 查看）\n", c.c_str());
	return 1;
}

int main(int argc, char** argv)
{
	SetConsoleOutputCP(65001);
	SetConsoleCP(65001);
	ensure_data_dir();

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

	// REPL 模式
	g_repl = true;
	int dv = vec_schema_version_on_disk();
	if (dv != 0 && dv < (int)VEC_SCHEMA_VERSION)
		printf("提示：向量格式已更新（旧 v%d → 新 v%d），请运行 reindex --all 重算。\n",
		       dv, (int)VEC_SCHEMA_VERSION);
	printf("简单备忘录 REPL（输入 help 查看，quit 退出）\n");
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
	return 0;
}
