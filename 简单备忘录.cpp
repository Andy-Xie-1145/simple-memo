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

// llama-server.exe 路径：环境变量 > 默认下载位置
static string llama_server_path()
{
	return get_env("MEMO_LLAMA_SERVER_EXE", llama_server_default());
}
// 本地 embedding server 端口
static int server_port() { return atoi(get_env("MEMO_SERVER_PORT", "8732").c_str()); }
// 嵌入模型（embeddinggemma-300m Q8_0，官方 GGUF）
static string embed_hf_repo() { return get_env("MEMO_EMBED_HF_REPO", "ggml-org/embeddinggemma-300M-GGUF"); }
static string embed_hf_file() { return get_env("MEMO_EMBED_HF_FILE", "embeddinggemma-300M-Q8_0.gguf"); }
// HF 镜像（国内加速；置空用官方）
static string hf_endpoint() { return get_env("MEMO_HF_ENDPOINT", "https://hf-mirror.com"); }

static bool ensure_data_dir()
{
	error_code ec;
	filesystem::create_directories(data_dir(), ec);
	return !ec;
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

	// 用户用环境变量显式指定却不可用 → 不自动重下，直接报错
	const char* envExe = getenv("MEMO_LLAMA_SERVER_EXE");
	if (envExe && *envExe)
	{
		g_last_err = string("MEMO_LLAMA_SERVER_EXE 指定的文件不可用: ") + llama_server_path();
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

// vectors.bin 格式：连续记录 (uint64 id)(uint32 dim)(float[dim])
static map<ll, vector<float>> load_vectors()
{
	map<ll, vector<float>> vs;
	ifstream f(vectors_path(), ios::binary);
	if (!f) return vs;
	while (f)
	{
		uint64_t id = 0;
		uint32_t dim = 0;
		f.read((char*)&id, sizeof(id));
		if (!f || f.gcount() != (streamsize)sizeof(id)) break;
		f.read((char*)&dim, sizeof(dim));
		if (!f || f.gcount() != (streamsize)sizeof(dim)) break;
		vector<float> v(dim);
		if (dim)
		{
			f.read((char*)v.data(), (streamsize)dim * sizeof(float));
			if ((size_t)f.gcount() != (size_t)dim * sizeof(float)) break;
		}
		vs[(ll)id] = move(v);
	}
	return vs;
}

static void save_vectors(const map<ll, vector<float>>& vs)
{
	ensure_data_dir();
	ofstream f(vectors_path(), ios::binary | ios::trunc);
	for (auto& kv : vs)
	{
		uint64_t id = (uint64_t)kv.first;
		uint32_t dim = (uint32_t)kv.second.size();
		f.write((char*)&id, sizeof(id));
		f.write((char*)&dim, sizeof(dim));
		if (dim) f.write((char*)kv.second.data(), (streamsize)dim * sizeof(float));
	}
}

// ===================== 命令 =====================
static bool g_repl = false; // REPL 模式下 add 无正文时读 stdin

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
	Memo m{ id, title, body, now_iso(), false };

	auto v = embed_text(doc_prompt(title, body));

	auto vs = load_vectors();
	if (!v.empty())
	{
		m.vec_ok = true;
		vs[id] = v;
		printf("已新增 #%-4lld（向量已生成，dim=%zu）\n", id, v.size());
	}
	else
	{
		m.vec_ok = false;
		printf("已新增 #%-4lld（警告：embedding 失败，标记 pending）\n", id);
		printf("  原因：%s\n", g_last_err.c_str());
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
	if (args.empty()) { printf("用法: show <id>\n"); return 1; }
	ll id = atoll(args[0].c_str());
	auto ms = load_memos();
	for (auto& m : ms) if (m.id == id)
	{
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
	vector<string> kws;
	for (auto& a : args)
	{
		if (a == "--any") any = true;
		else kws.push_back(a);
	}
	if (kws.empty()) { printf("用法: search <关键词...> [--any]\n"); return 1; }

	vector<string> kws_l;
	for (auto& k : kws) kws_l.push_back(lower(k));

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
	printf("命中 %zu 条（%s）：\n", hits.size(), any ? "OR" : "AND");
	for (auto* m : hits)
		printf("#%-4lld [%s] %s\n", m->id, m->ctime.c_str(), m->title.c_str());
	return 0;
}

static int cmd_find(vector<string>& args)
{
	int n = 5;
	vector<string> parts;
	for (auto& a : args)
	{
		if (a.rfind("-n", 0) == 0 && a.size() > 2) n = atoi(a.c_str() + 2);
		else if (a != "-n") parts.push_back(a);
	}
	if (parts.empty()) { printf("用法: find <语义查询> [-nK]\n"); return 1; }

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
	vector<pair<double, Memo*>> ranked;
	for (auto& m : ms)
	{
		if (!m.vec_ok) continue;
		auto it = vs.find(m.id);
		if (it == vs.end()) continue;
		ranked.emplace_back(cosine(qv, it->second), &m);
	}
	if (ranked.empty())
	{
		printf("没有可语义搜索的备忘录（可能都 pending）。运行 reindex 补算。\n");
		return 0;
	}
	sort(ranked.begin(), ranked.end(), [](auto& a, auto& b) { return a.first > b.first; });

	int shown = min((int)ranked.size(), n > 0 ? n : 5);
	printf("Top-%d 语义匹配：\n", shown);
	for (int i = 0; i < shown; i++)
		printf("  %5.1f%%  #%-4lld %s\n", ranked[i].first * 100.0, ranked[i].second->id, ranked[i].second->title.c_str());
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

static int cmd_edit(vector<string>& args)
{
	if (args.size() < 2) { printf("用法: edit <id> <新标题> [新正文...]\n  只给新标题则正文不变。\n"); return 1; }
	ll id = atoll(args[0].c_str());
	auto ms = load_memos();
	for (auto& m : ms) if (m.id == id)
	{
		m.title = args[1];
		if (args.size() > 2)
		{
			string nb;
			for (size_t i = 2; i < args.size(); i++) { if (i > 2) nb += " "; nb += args[i]; }
			m.body = nb;
		}
		auto v = embed_text(doc_prompt(m.title, m.body));

		auto vs = load_vectors();
		if (!v.empty())
		{
			m.vec_ok = true;
			vs[id] = v;
			save_memos(ms);
			save_vectors(vs);
			printf("已编辑 #%lld 并重算向量。\n", id);
		}
		else
		{
			m.vec_ok = false;
			save_memos(ms);
			save_vectors(vs);
			printf("已编辑 #%lld（embedding 失败，标记 pending：%s）\n", id, g_last_err.c_str());
		}
		return 0;
	}
	printf("找不到 #%lld\n", id);
	return 1;
}

static int cmd_reindex(vector<string>& args)
{
	bool all = false;
	for (auto& a : args) if (a == "--all") all = true;

	auto ms = load_memos();
	auto vs = load_vectors();
	int cnt = 0, ok = 0;
	for (auto& m : ms)
	{
		if (!all && m.vec_ok) continue;
		cnt++;
		auto v = embed_text(doc_prompt(m.title, m.body));
		if (!v.empty()) { vs[m.id] = v; m.vec_ok = true; ok++; }
		else printf("  #%lld 失败：%s\n", m.id, g_last_err.c_str());
	}
	save_memos(ms);
	save_vectors(vs);
	if (cnt == 0) printf("没有需要补算的（全部已有向量）。加 --all 可全量重建。\n");
	else printf("补算完成：%d/%d 成功。\n", ok, cnt);
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
		"简单备忘录 - 命令：\n"
		"  add <标题> [正文...]   新增（REPL 下无正文则多行输入，:end 结束）\n"
		"  list                   列出全部\n"
		"  show <id>              查看详情\n"
		"  search <词...> [--any] 多关键词搜索（默认 AND，子串+不区分大小写）\n"
		"  find <查询> [-nK]      语义搜索（向量），默认 top5\n"
		"  delete <id>            删除\n"
		"  edit <id> <标题> [正文] 编辑（自动重算向量）\n"
		"  reindex [--all]        补算 pending / 全量重建\n"
		"  stop                   停止后台 llama-server（释放内存）\n"
		"  help                   帮助\n"
		"  quit                   退出（仅 REPL）\n"
		"环境变量：MEMO_DATA_DIR, MEMO_LLAMA_SERVER_EXE, MEMO_SERVER_PORT,\n"
		"          MEMO_EMBED_HF_REPO, MEMO_EMBED_HF_FILE, MEMO_HF_ENDPOINT\n");
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
