// 简单备忘录：标题+文本，多关键词搜索 + 向量语义搜索
// 嵌入由本地 LM Studio（OpenAI 兼容 API，底层 llama.cpp）提供，禁用 Ollama
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
static string tmp_body_path() { return data_dir() + "/.body.json"; }

static string embed_url()
{
	return get_env("MEMO_EMBED_URL", "http://localhost:1234/v1/embeddings");
}
static string embed_model()
{
	return get_env("MEMO_EMBED_MODEL", "text-embedding-nomic-embed-text-v1.5");
}

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

// ===================== HTTP（curl 子进程） =====================
// 请求体写入临时文件，避免命令行转义地狱；响应从 curl stdout 读取
static string http_post_json(const string& url, const string& body)
{
	ensure_data_dir();
	string bf = tmp_body_path();
	ofstream f(bf, ios::binary);
	f << body;
	f.close();

	string cmd = "curl -s -m 60 -X POST -H \"Content-Type: application/json\" --data-binary \"@";
	cmd += bf;
	cmd += "\" \"";
	cmd += url;
	cmd += "\"";

	FILE* p = _popen(cmd.c_str(), "r");
	if (!p) return "";
	string resp;
	char buf[8192];
	while (fgets(buf, sizeof(buf), p)) resp += buf;
	_pclose(p);
	return resp;
}

// ===================== Embedding =====================
static string g_last_err;

// 成功返回向量；失败返回空并设置 g_last_err
static vector<float> embed_text(const string& text)
{
	g_last_err.clear();
	string body = "{\"model\":" + json_escape(embed_model()) + ",\"input\":" + json_escape(text) + "}";
	string resp = http_post_json(embed_url(), body);
	if (resp.empty()) { g_last_err = "curl 无响应（LM Studio 未启动或端口不对？）"; return {}; }

	Json j = json_parse(resp);
	if (Json* err = j.find("error"))
	{
		Json* msg = err->find("message");
		g_last_err = msg ? msg->s : "embedding API 返回错误";
		return {};
	}
	Json* data = j.find("data");
	if (!data || data->t != Json::Arr || data->arr.empty()) { g_last_err = "响应缺少 data 字段"; return {}; }
	Json* emb = data->arr[0].find("embedding");
	if (!emb || emb->t != Json::Arr) { g_last_err = "响应缺少 embedding 数组"; return {}; }

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

	string text = title;
	if (!body.empty()) text += "\n" + body;
	auto v = embed_text(text);

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
		printf("  启动 LM Studio 后运行 reindex 补算。\n");
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

	auto qv = embed_text(q);
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
		string text = m.title;
		if (!m.body.empty()) text += "\n" + m.body;
		auto v = embed_text(text);

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
		string text = m.title;
		if (!m.body.empty()) text += "\n" + m.body;
		auto v = embed_text(text);
		if (!v.empty()) { vs[m.id] = v; m.vec_ok = true; ok++; }
		else printf("  #%lld 失败：%s\n", m.id, g_last_err.c_str());
	}
	save_memos(ms);
	save_vectors(vs);
	if (cnt == 0) printf("没有需要补算的（全部已有向量）。加 --all 可全量重建。\n");
	else printf("补算完成：%d/%d 成功。\n", ok, cnt);
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
		"  help                   帮助\n"
		"  quit                   退出（仅 REPL）\n"
		"环境变量：MEMO_EMBED_URL, MEMO_EMBED_MODEL, MEMO_DATA_DIR\n");
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
