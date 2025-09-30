// C+ -> C++98 converter, multi-file in -> .cpp out
//
// 1) Tokenization
//    - Drops C/C++ comments (// and /* */).
//    - Forbids '->' in C+ input (pointers must use '.').
//
// 2) Semicolons
//    - Treats end-of-line as ';' when appropriate.
//    - Adds a trailing ';' after struct/union/enum *type definitions* where no
//    declarator follows,
//      including one-liners like:  struct S { int x; int y; }  ->  struct S {
//      int x; int y; };
//
// 3) Member access rewrite
//    - Converts '.' to '->' when the base expression is a single pointer at the
//    access point.
//    - For multi-level pointers (e.g., S**), rewrites 'pps.member' as
//    '(*pps)->member'.
//    - Tracks array indexing and function calls in the base, adjusting
//    effective pointer depth:
//         buf[8].dx   where buf is Vec2* buf[16]  ->  buf[8]->dx
//
// 4) Declarations & scopes
//    - Builds scope tree (Global / Function / Struct / Union / Enum / Block).
//    - Records variables with pointer level and array rank (including a relaxed
//    detection path,
//      so unknown typedef names like 'Vec2' still work).
//    - Uses scope info to resolve which identifiers are pointers at each '.'
//    access.
//
//
// 6) Output
//    - For each input <path>.cp, writes a sibling <path>.cpp.
//    - Spacing is preserved in a simple token-joined manner.
//
// Note: This program expects file paths as arguments (no stdin mode in this
// build).

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

struct Token {
    enum Type {
        Identifier,
        Number,
        StringLit,
        Keyword,
        Operator,
        Punct,
        Preprocessor,
        Unknown
    } type;
    std::string text;
    int line;
    int col;
    int scope_id;
    Token() : type(Unknown), line(0), col(0), scope_id(0) {}
};

struct Scope {
    int id, parent;
    std::string kind;  // "Global","Function","Struct","Enum","Union","Block"
    std::string name;
    Scope() : id(0), parent(-1) {}
};

struct VarInfo {
    int pointer_level;  // '*' count on declarator (0 for plain objects)
    int array_rank;     // number of [] suffixes on declarator
    VarInfo() : pointer_level(999), array_rank(0) {}
};

static bool isIdentStart(char c) {
    return std::isalpha((unsigned char)c) || c == '_';
}
static bool isIdentChar(char c) {
    return std::isalnum((unsigned char)c) || c == '_';
}

static bool read_file(const char* path, std::string& out) {
    std::ifstream in(path, std::ios::in | std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

static bool write_text_file(const std::string& path, const std::string& data) {
    std::ofstream out(path.c_str(),
        std::ios::out | std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(data.data(), (std::streamsize)data.size());
    return (bool)out;
}

static std::string replace_ext(const std::string& path,
    const char* newext) {  // newext like ".cpp"
    std::string::size_type sep = path.find_last_of("/\\");
    std::string::size_type dot = path.find_last_of('.');
    if (dot == std::string::npos || (sep != std::string::npos && dot < sep))
        return path + newext;
    return path.substr(0, dot) + newext;
}

// Normalize physical lines:
// - CRLF/CR -> LF
// - Remove line-continuations: backslash followed by newline
static std::string preprocess_physical_lines(const std::string& s) {
    std::string t;
    t.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '\r') {
            if (i + 1 < s.size() && s[i + 1] == '\n')
                continue;
            else
                t.push_back('\n');
        }
        else
            t.push_back(c);
    }
    std::string u;
    u.reserve(t.size());
    for (size_t i = 0; i < t.size(); ++i) {
        if (t[i] == '\\' && i + 1 < t.size() && t[i + 1] == '\n') {
            ++i;
            continue;
        }
        u.push_back(t[i]);
    }
    return u;
}

static std::set<std::string> make_keywords() {
    const char* kw[] = { "auto",     "break",    "case",    "char",   "const",
                        "continue", "default",  "do",      "double", "else",
                        "enum",     "extern",   "float",   "for",    "goto",
                        "if",       "inline",   "int",     "long",   "register",
                        "return",   "short",    "signed",  "sizeof", "static",
                        "struct",   "switch",   "typedef", "union",  "unsigned",
                        "void",     "volatile", "while",   "bool" };
    std::set<std::string> s;
    for (size_t i = 0; i < sizeof(kw) / sizeof(kw[0]); ++i) s.insert(kw[i]);
    return s;
}
static bool is_op_char(char c) {
    const char* ops = "+-*/%=&|!<>^~?:";
    return std::strchr(ops, c) != 0;
}
static bool is_punct_char(char c) {
    return c == '(' || c == ')' || c == '{' || c == '}' || c == '[' ||
        c == ']' || c == ';' || c == ',' || c == '.';
}

// ----- Lexer ('->' forbidden in C+ input) -----
static void lex(const std::string& src, std::vector<Token>& out) {
    std::set<std::string> kw = make_keywords();
    int line = 1, col = 1;
    for (size_t i = 0; i < src.size();) {
        char c = src[i];
        if (c == '\n') {
            ++line;
            col = 1;
            ++i;
            continue;
        }
        if (std::isspace((unsigned char)c)) {
            ++i;
            ++col;
            continue;
        }

        if (c == '#') {  // preprocessor line
            size_t s = i;
            int sc = col;
            while (i < src.size() && src[i] != '\n') {
                ++i;
                ++col;
            }
            Token t;
            t.type = Token::Preprocessor;
            t.text = src.substr(s, i - s);
            t.line = line;
            t.col = sc;
            out.push_back(t);
            continue;
        }

        // comments (drop)
        if (c == '/' && i + 1 < src.size()) {
            if (src[i + 1] == '/') {
                i += 2;
                col += 2;
                while (i < src.size() && src[i] != '\n') {
                    ++i;
                    ++col;
                }
                continue;
            }
            if (src[i + 1] == '*') {
                i += 2;
                col += 2;
                while (i + 1 < src.size()) {
                    if (src[i] == '\n') {
                        ++line;
                        col = 1;
                        ++i;
                    }
                    else if (src[i] == '*' && src[i + 1] == '/') {
                        i += 2;
                        col += 2;
                        break;
                    }
                    else {
                        ++i;
                        ++col;
                    }
                }
                continue;
            }
        }

        if (c == '"') {  // string literal
            size_t s = i;
            int sc = col;
            ++i;
            ++col;
            while (i < src.size()) {
                char d = src[i];
                if (d == '\\') {
                    if (i + 1 < src.size()) {
                        i += 2;
                        col += 2;
                    }
                    else {
                        ++i;
                        ++col;
                    }
                }
                else if (d == '"') {
                    ++i;
                    ++col;
                    break;
                }
                else if (d == '\n') {
                    ++i;
                    ++line;
                    col = 1;
                }
                else {
                    ++i;
                    ++col;
                }
            }
            Token t;
            t.type = Token::StringLit;
            t.text = src.substr(s, i - s);
            t.line = line;
            t.col = sc;
            out.push_back(t);
            continue;
        }

        if (std::isdigit((unsigned char)c)) {  // number (simple)
            size_t s = i;
            int sc = col;
            bool dot = false;
            while (i < src.size()) {
                char d = src[i];
                if (std::isdigit((unsigned char)d)) {
                    ++i;
                    ++col;
                }
                else if (d == '.' && !dot) {
                    dot = true;
                    ++i;
                    ++col;
                }
                else
                    break;
            }
            Token t;
            t.type = Token::Number;
            t.text = src.substr(s, i - s);
            t.line = line;
            t.col = sc;
            out.push_back(t);
            continue;
        }

        if (isIdentStart(c)) {  // identifier / keyword
            size_t s = i;
            int sc = col;
            ++i;
            ++col;
            while (i < src.size() && isIdentChar(src[i])) {
                ++i;
                ++col;
            }
            std::string w = src.substr(s, i - s);
            Token t;
            t.type = kw.count(w) ? Token::Keyword : Token::Identifier;
            t.text = w;
            t.line = line;
            t.col = sc;
            out.push_back(t);
            continue;
        }

        if (is_op_char(c)) {  // operators (two-char first) forbid '->'
            int sc = col;
            if (i + 1 < src.size()) {
                std::string two = src.substr(i, 2);
                if (two == "->") {
                    std::fprintf(stderr,
                        "C+ error: '->' is not allowed (line %d, col "
                        "%d). Pointers use '.' in C+.\n",
                        line, sc);
                    std::exit(2);
                }
                if (two == "++" || two == "--" || two == "==" || two == "!=" ||
                    two == ">=" || two == "<=" || two == "+=" || two == "-=" ||
                    two == "*=" || two == "/=" || two == "&&" || two == "||" ||
                    two == "&=" || two == "|=" || two == "^=" || two == "<<" ||
                    two == ">>") {
                    Token t;
                    t.type = Token::Operator;
                    t.text = two;
                    t.line = line;
                    t.col = sc;
                    out.push_back(t);
                    i += 2;
                    col += 2;
                    continue;
                }
            }
            Token t;
            t.type = Token::Operator;
            t.text = std::string(1, c);
            t.line = line;
            t.col = sc;
            out.push_back(t);
            ++i;
            ++col;
            continue;
        }

        if (is_punct_char(c)) {
            Token t;
            t.type = Token::Punct;
            t.text = std::string(1, c);
            t.line = line;
            t.col = col;
            out.push_back(t);
            ++i;
            ++col;
            continue;
        }

        Token t;
        t.type = Token::Unknown;
        t.text = std::string(1, c);
        t.line = line;
        t.col = col;
        out.push_back(t);
        ++i;
        ++col;
    }
}

// ----- helpers -----
static bool TKIs(const std::vector<Token>& v, int i, Token::Type t,
    const char* txt = 0) {
    if (i < 0 || (size_t)i >= v.size()) return false;
    if (v[i].type != t) return false;
    return txt ? v[i].text == txt : true;
}
static bool is_kw(const std::vector<Token>& v, int i, const char* k) {
    return TKIs(v, i, Token::Keyword, k);
}
static bool is_p(const std::vector<Token>& v, int i, const char* p) {
    return TKIs(v, i, Token::Punct, p);
}
static bool is_op(const std::vector<Token>& v, int i, const char* o) {
    return TKIs(v, i, Token::Operator, o);
}

static std::set<std::string> builtin_types() {
    const char* bt[] = { "void",  "char",   "short",  "int",      "long",
                        "float", "double", "signed", "unsigned", "bool" };
    std::set<std::string> s;
    for (size_t i = 0; i < sizeof(bt) / sizeof(bt[0]); ++i) s.insert(bt[i]);
    return s;
}

struct Param {
    std::string name;
    int stars;
    Param() : stars(0) {}
};

static bool looks_like_func_signature(const std::vector<Token>& tk, int i_type,
    int& i_name, int& i_lbrace, int& i_lp,
    int& i_rp) {
    int n = (int)tk.size();
    int i = i_type + 1;
    while (i < n && (tk[i].type == Token::Keyword || is_op(tk, i, "*") ||
        is_op(tk, i, "&")))
        ++i;
    if (i >= n || tk[i].type != Token::Identifier) return false;
    i_name = i;
    if (i + 1 < n && is_p(tk, i + 1, "(")) {
        i_lp = i + 1;
        int depth = 0;
        int j = i + 1;
        for (; j < n; ++j) {
            if (is_p(tk, j, "("))
                depth++;
            else if (is_p(tk, j, ")")) {
                depth--;
                if (depth == 0) {
                    i_rp = j;
                    ++j;
                    break;
                }
            }
        }
        if (j < n) {
            while (j < n && (tk[j].type == Token::Keyword ||
                tk[j].type == Token::Identifier ||
                is_op(tk, j, "*") || is_op(tk, j, "&")))
                ++j;
            i_lbrace = (j < n && is_p(tk, j, "{")) ? j : -1;
            return true;
        }
    }
    return false;
}

static void parse_params(const std::vector<Token>& tk, int lp, int rp,
    std::vector<Param>& out,
    const std::set<std::string>& known_types) {
    out.clear();
    int i = lp + 1;
    while (i < rp) {
        if (is_p(tk, i, ",")) {
            ++i;
            continue;
        }
        bool type_start = false;
        if (i < rp && tk[i].type == Token::Identifier &&
            known_types.count(tk[i].text))
            type_start = true;
        if (i < rp && tk[i].type == Token::Keyword &&
            (builtin_types().count(tk[i].text) || tk[i].text == "struct" ||
                tk[i].text == "enum" || tk[i].text == "union"))
            type_start = true;
        if (!type_start) {
            ++i;
            continue;
        }

        int j = i;
        if (is_kw(tk, j, "struct") || is_kw(tk, j, "enum") ||
            is_kw(tk, j, "union")) {
            if (j + 1 < rp && tk[j + 1].type == Token::Identifier)
                j += 2;
            else {
                ++i;
                continue;
            }
        }
        else {
            while (j < rp && (tk[j].type == Token::Keyword ||
                tk[j].type == Token::Identifier))
                ++j;
        }
        int stars = 0;
        while (j < rp && is_op(tk, j, "*")) {
            ++stars;
            ++j;
        }
        if (!(j < rp && tk[j].type == Token::Identifier)) {
            i = j;
            continue;
        }
        Param p;
        p.name = tk[j].text;
        p.stars = stars;
        out.push_back(p);
        ++j;

        while (j < rp && is_p(tk, j, "[")) {
            while (j < rp && !is_p(tk, j, "]")) ++j;
            if (j < rp) ++j;
        }
        while (j < rp && !is_p(tk, j, ",")) ++j;
        i = j;
    }
}

// ---- relaxed declaration detection (handles unknown typedef names like
// 'Vec2') ----
static bool detect_relaxed_declaration(const std::vector<Token>& tk, size_t i,
    size_t& j_out, std::string& name_out,
    int& stars_out, int& arrays_out) {
    size_t n = tk.size();
    size_t j = i;

    if (!(tk[j].type == Token::Identifier ||
        (tk[j].type == Token::Keyword &&
            (tk[j].text == "struct" || tk[j].text == "enum" ||
                tk[j].text == "union"))))
        return false;

    if (tk[j].type == Token::Keyword) {
        if (j + 1 < n && tk[j + 1].type == Token::Identifier)
            j += 2;
        else
            return false;
    }
    else {
        ++j;
    }

    while (j < n &&
        (tk[j].type == Token::Keyword || tk[j].type == Token::Identifier))
        ++j;

    int stars = 0;
    while (j < n && tk[j].type == Token::Operator && tk[j].text == "*") {
        ++stars;
        ++j;
    }

    if (!(j < n && tk[j].type == Token::Identifier)) return false;
    std::string name = tk[j].text;
    ++j;

    int arrays = 0;
    while (j < n && tk[j].type == Token::Punct && tk[j].text == "[") {
        size_t k = j + 1;
        while (k < n && !(tk[k].type == Token::Punct && tk[k].text == "]")) ++k;
        if (k == n) break;
        j = k + 1;
        ++arrays;
    }

    if (j < n &&
        ((tk[j].type == Token::Punct &&
            (tk[j].text == ";" || tk[j].text == "," || tk[j].text == "[")) ||
            (tk[j].type == Token::Operator && tk[j].text == "=") ||
            (tk[j].type == Token::Punct && tk[j].text == "{"))) {
        j_out = j;
        name_out = name;
        stars_out = stars;
        arrays_out = arrays;
        return true;
    }
    return false;
}

// ---------- scope & decl analysis ----------
static void analyze_scopes_and_vars(
    std::vector<Token>& tk, std::vector<Scope>& scopes,
    std::vector<std::map<std::string, VarInfo> >& scope_vars,
    std::set<std::string>& known_types) {
    scopes.clear();
    scope_vars.clear();
    Scope g;
    g.id = 0;
    g.parent = -1;
    g.kind = "Global";
    g.name = "";
    scopes.push_back(g);
    scope_vars.push_back(std::map<std::string, VarInfo>());

    int cur = 0;
    std::string pending_kind, pending_name;
    std::map<int, std::vector<Param> > params_at_lbrace;

    for (size_t i = 0; i < tk.size(); ++i) {
        tk[i].scope_id = cur;

        // typedef adds a new known type (last identifier before ';' / '}')
        if (is_kw(tk, (int)i, "typedef")) {
            int last_ident = -1;
            for (size_t j = i + 1;
                j < tk.size() && !(tk[j].type == Token::Punct &&
                    (tk[j].text == ";" || tk[j].text == "}"));
                ++j)
                if (tk[j].type == Token::Identifier) last_ident = (int)j;
            if (last_ident != -1) known_types.insert(tk[last_ident].text);
        }
        // tag names of struct/union/enum become known types
        if (is_kw(tk, (int)i, "struct") || is_kw(tk, (int)i, "enum") ||
            is_kw(tk, (int)i, "union")) {
            if (i + 1 < tk.size() && tk[i + 1].type == Token::Identifier)
                known_types.insert(tk[i + 1].text);

            // remember scope kind/name for the upcoming '{'
            if (is_kw(tk, (int)i, "struct"))
                pending_kind = "Struct";
            else if (is_kw(tk, (int)i, "enum"))
                pending_kind = "Enum";
            else
                pending_kind = "Union";
            if (i + 1 < tk.size() && tk[i + 1].type == Token::Identifier)
                pending_name = tk[i + 1].text;
            else
                pending_name.clear();
        }

        // function detection
        bool type_start = false;
        if (tk[i].type == Token::Identifier && known_types.count(tk[i].text))
            type_start = true;
        if (tk[i].type == Token::Keyword &&
            (builtin_types().count(tk[i].text) || tk[i].text == "struct" ||
                tk[i].text == "enum" || tk[i].text == "union"))
            type_start = true;

        if (type_start) {
            int i_name = -1, i_lbrace = -1, lp = -1, rp = -1;
            if (looks_like_func_signature(tk, (int)i, i_name, i_lbrace, lp,
                rp) &&
                i_lbrace != -1) {
                pending_kind = "Function";
                pending_name = tk[i_name].text;
                std::vector<Param> ps;
                parse_params(tk, lp, rp, ps, known_types);
                params_at_lbrace[i_lbrace] = ps;
            }
        }

        // variable declarators (non-function)
        bool handled_decl = false;
        if (type_start) {
            int dn = -1, lb = -1, lp = -1, rp = -1;
            if (looks_like_func_signature(tk, (int)i, dn, lb, lp, rp)) {
                // handled at '{' via params_at_lbrace
            }
            else {
                size_t j = i;
                if (is_kw(tk, (int)j, "struct") || is_kw(tk, (int)j, "enum") ||
                    is_kw(tk, (int)j, "union")) {
                    if (j + 1 < tk.size() &&
                        tk[j + 1].type == Token::Identifier)
                        j += 2;
                }
                else {
                    while (j < tk.size() && (tk[j].type == Token::Keyword ||
                        tk[j].type == Token::Identifier))
                        ++j;
                }
                while (j < tk.size()) {
                    int stars = 0;
                    while (j < tk.size() && is_op(tk, (int)j, "*")) {
                        ++stars;
                        ++j;
                    }
                    if (!(j < tk.size() && tk[j].type == Token::Identifier))
                        break;
                    const std::string name = tk[j].text;
                    ++j;
                    int arrays = 0;
                    while (j < tk.size() && is_p(tk, (int)j, "[")) {
                        while (j < tk.size() && !is_p(tk, (int)j, "]")) ++j;
                        if (j < tk.size()) ++j;
                        ++arrays;
                    }
                    VarInfo& vi = scope_vars[cur][name];
                    if (vi.pointer_level == 999)
                        vi.pointer_level = stars;
                    else if (stars < vi.pointer_level)
                        vi.pointer_level = stars;
                    if (arrays > vi.array_rank) vi.array_rank = arrays;
                    handled_decl = true;
                    if (j < tk.size() && is_p(tk, (int)j, ",")) {
                        ++j;
                        continue;
                    }
                    break;
                }
            }
        }
        // relaxed path (type unknown): try a generic declarator shape
        if (!handled_decl && tk[i].type == Token::Identifier) {
            size_t jnext = 0;
            std::string vname;
            int stars = 0, arrays = 0;
            if (detect_relaxed_declaration(tk, i, jnext, vname, stars,
                arrays)) {
                VarInfo& vi = scope_vars[cur][vname];
                if (vi.pointer_level == 999)
                    vi.pointer_level = stars;
                else if (stars < vi.pointer_level)
                    vi.pointer_level = stars;
                if (arrays > vi.array_rank) vi.array_rank = arrays;
                handled_decl = true;
            }
        }

        // scope open: create scope with pending kind/name
        if (is_p(tk, (int)i, "{")) {
            Scope s;
            s.id = (int)scopes.size();
            s.parent = cur;
            s.kind = pending_kind.empty() ? "Block" : pending_kind;
            s.name = pending_name;
            scopes.push_back(s);
            scope_vars.push_back(std::map<std::string, VarInfo>());
            cur = s.id;

            // function parameters become vars in function scope
            std::map<int, std::vector<Param> >::iterator pit =
                params_at_lbrace.find((int)i);
            if (pit != params_at_lbrace.end()) {
                for (size_t k = 0; k < pit->second.size(); ++k) {
                    const Param& p = pit->second[k];
                    VarInfo& vi = scope_vars[cur][p.name];
                    if (vi.pointer_level == 999)
                        vi.pointer_level = p.stars;
                    else if (p.stars < vi.pointer_level)
                        vi.pointer_level = p.stars;
                }
            }
            pending_kind.clear();
            pending_name.clear();
        }
        // scope close
        if (is_p(tk, (int)i, "}")) {
            if (cur != 0) cur = scopes[cur].parent;
            pending_kind.clear();
            pending_name.clear();
        }
    }
}

static int resolve_ptr_level(
    const std::vector<Scope>& scopes,
    const std::vector<std::map<std::string, VarInfo> >& scope_vars,
    int scope_id, const std::string& name, int& array_rank_out) {
    array_rank_out = 0;
    int cur = scope_id;
    while (cur != -1) {
        std::map<std::string, VarInfo>::const_iterator it =
            scope_vars[cur].find(name);
        if (it != scope_vars[cur].end()) {
            array_rank_out = it->second.array_rank;
            return it->second.pointer_level;
        }
        cur = scopes[cur].parent;
    }
    return 999;
}

// Remove any semicolons that appear *inside* enum bodies (keep the one after
// '}').
static void remove_semicolons_inside_enums(std::vector<Token>& toks,
    const std::vector<Scope>& scopes) {
    std::vector<Token> out;
    out.reserve(toks.size());
    for (size_t i = 0; i < toks.size(); ++i) {
        const Token& t = toks[i];
        if (t.type == Token::Punct && t.text == ";") {
            int sid = t.scope_id;
            if (sid >= 0 && sid < (int)scopes.size() &&
                scopes[sid].kind == "Enum")
                continue;
        }
        out.push_back(t);
    }
    toks.swap(out);
}

// Add ';' after struct/union/enum *type blocks* when no declarator follows.
static void add_semicolon_after_type_blocks(std::vector<Token>& toks,
    const std::vector<Scope>& scopes) {
    for (size_t i = 0; i < toks.size(); ++i) {
        const Token& t = toks[i];
        if (t.type != Token::Punct || t.text != "}") continue;

        int sid = t.scope_id;
        if (sid < 0 || sid >= (int)scopes.size()) continue;

        const std::string& kind =
            scopes[sid].kind;  // "Struct","Union","Enum","Block",...
        if (!(kind == "Struct" || kind == "Union" || kind == "Enum")) continue;

        // Look ahead to see if a declarator/';' already follows
        size_t j = i + 1;
        while (j < toks.size() && toks[j].type == Token::Preprocessor) ++j;

        bool declarator_follows = false;
        if (j < toks.size()) {
            const Token& n = toks[j];
            declarator_follows =
                (n.type == Token::Identifier) ||  // alias name: "} Name"
                (n.type == Token::Operator &&
                    n.text == "*") ||  // pointer declarator
                (n.type == Token::Punct &&
                    (n.text == "(" || n.text == "[" ||
                        n.text == ";"));  // fn/array or already ';'
        }

        if (!declarator_follows) {
            Token semi = t;
            semi.type = Token::Punct;
            semi.text = ";";
            toks.insert(toks.begin() + (i + 1), semi);
            ++i;  // skip the inserted ';'
        }
    }
}

// Split tokens into physical lines; track a representative scope per line.
static void split_into_lines(const std::vector<Token>& toks,
    std::vector<std::vector<Token> >& byline,
    std::vector<int>& line_scope) {
    byline.clear();
    line_scope.clear();
    if (toks.empty()) return;
    int current = toks.front().line;
    byline.push_back(std::vector<Token>());
    line_scope.push_back(toks.front().scope_id);
    for (size_t i = 0; i < toks.size(); ++i) {
        if (toks[i].line != current) {
            current = toks[i].line;
            byline.push_back(std::vector<Token>());
            line_scope.push_back(toks[i].scope_id);
        }
        byline.back().push_back(toks[i]);
    }
}

// Need a trailing ';'? (never inside enum bodies). Also handles initializer
// lists ending with '}'.
static bool needs_semicolon(const std::vector<Token>& line,
    const std::string& scope_kind) {
    if (line.empty()) return false;
    if (scope_kind == "Enum") return false;

    const Token& first = line.front();
    const Token& last = line.back();
    if (first.type == Token::Preprocessor) return false;

    // initializer list: "x = { ... }" ? needs ';'
    if (last.type == Token::Punct && last.text == "}") {
        bool has_eq = false, has_lbrace = false;
        for (size_t i = 0; i + 1 < line.size(); ++i) {
            if (line[i].type == Token::Operator && line[i].text == "=")
                has_eq = true;
            if (line[i].type == Token::Punct && line[i].text == "{")
                has_lbrace = true;
        }
        if (has_eq && has_lbrace) return true;
        return false;  // otherwise likely a block/type close
    }

    if (last.type == Token::Punct && (last.text == "{" || last.text == ";"))
        return false;

    bool has_ctrl = false;
    for (size_t i = 0; i < line.size(); ++i) {
        if (line[i].type == Token::Keyword &&
            (line[i].text == "if" || line[i].text == "for" ||
                line[i].text == "while" || line[i].text == "switch")) {
            has_ctrl = true;
            break;
        }
    }
    if (has_ctrl && last.type == Token::Punct && last.text == ")") return false;

    if (last.type == Token::Identifier || last.type == Token::Number ||
        last.type == Token::StringLit ||
        (last.type == Token::Punct && (last.text == ")" || last.text == "]")))
        return true;

    return false;
}

// '.' to '->' for pointers (scope-aware), handling postfix [ ] and ( ).
// If effective pointer depth > 1 at member access, rewrite 'base.member' as
// '(*base)->member'.
static void rewrite_member_chains(
    std::vector<Token>& line, int scope_id, const std::vector<Scope>& scopes,
    const std::vector<std::map<std::string, VarInfo> >& scope_vars) {
    for (size_t i = 0; i < line.size(); ++i) {
        if (line[i].type != Token::Identifier) continue;

        int base_arrays = 0;
        int ptr = resolve_ptr_level(scopes, scope_vars, scope_id, line[i].text,
            base_arrays);
        if (ptr == 999 && base_arrays == 0) continue;  // unknown symbol; skip

        int cur_ptr = (ptr == 999 ? 0 : ptr);
        int cur_arr = base_arrays;
        size_t j = i + 1;

        // walk postfix: [ ... ] and ( ... )
        while (j < line.size()) {
            if (line[j].type == Token::Punct && line[j].text == "[") {
                int depth = 0;
                size_t k = j;
                for (; k < line.size(); ++k) {
                    if (line[k].type == Token::Punct && line[k].text == "[")
                        depth++;
                    else if (line[k].type == Token::Punct &&
                        line[k].text == "]") {
                        depth--;
                        if (depth == 0) break;
                    }
                }
                if (k < line.size()) {
                    if (cur_arr > 0)
                        cur_arr--;  // array indexing -> element
                    else if (cur_ptr > 0)
                        cur_ptr--;  // pointer indexing -> deref
                    j = k + 1;
                    continue;
                }
                else
                    break;
            }
            else if (line[j].type == Token::Punct && line[j].text == "(") {
                int depth = 0;
                size_t k = j;
                for (; k < line.size(); ++k) {
                    if (line[k].type == Token::Punct && line[k].text == "(")
                        depth++;
                    else if (line[k].type == Token::Punct &&
                        line[k].text == ")") {
                        depth--;
                        if (depth == 0) break;
                    }
                }
                if (k < line.size()) {
                    j = k + 1;
                    continue;
                }
                else
                    break;
            }
            else
                break;
        }

        // Rewrite ". <ident>" segments based on effective pointer depth
        while (j + 1 < line.size() && line[j].type == Token::Punct &&
            line[j].text == "." && line[j + 1].type == Token::Identifier) {
            if (cur_ptr == 1) {
                line[j].type = Token::Operator;
                line[j].text = "->";
            }
            else if (cur_ptr > 1) {
                Token lpar = line[i];
                lpar.type = Token::Punct;
                lpar.text = "(";
                Token star = line[i];
                star.type = Token::Operator;
                star.text = "*";
                Token rpar = line[j];
                rpar.type = Token::Punct;
                rpar.text = ")";

                line.insert(line.begin() + i, lpar);
                line.insert(line.begin() + i + 1, star);
                j += 2;  // account for inserts

                line.insert(line.begin() + j, rpar);
                ++j;

                line[j].type = Token::Operator;
                line[j].text = "->";

                cur_ptr -= 1;  // (*base) dereferences once
            }  // else cur_ptr == 0: keep '.'

            j += 2;  // skip over the member identifier
        }

        if (j > 0) i = j - 1;
    }
}

// Insert a ';' immediately before any '}' on the same physical line when needed
// (not in enums).
static void insert_semicolon_before_closing_brace_on_line(
    std::vector<Token>& line, const std::string& scope_kind) {
    if (scope_kind == "Enum") return;
    for (size_t i = 1; i < line.size(); ++i) {
        if (line[i].type == Token::Punct && line[i].text == "}") {
            const Token& prev = line[i - 1];
            if (prev.type == Token::Punct &&
                (prev.text == ";" || prev.text == "{"))
                continue;
            bool need =
                (prev.type == Token::Identifier || prev.type == Token::Number ||
                    prev.type == Token::StringLit) ||
                (prev.type == Token::Punct &&
                    (prev.text == ")" || prev.text == "]")) ||
                (prev.type == Token::Operator);
            if (need) {
                Token semi = prev;
                semi.type = Token::Punct;
                semi.text = ";";
                line.insert(line.begin() + i, semi);
                ++i;
            }
        }
    }
}

// Emit a line to an arbitrary ostream (used to capture into a .cpp file)
static void emit_line(const std::vector<Token>& line, std::ostream& os) {
    if (line.empty()) {
        os << "\n";
        return;
    }
    bool bol = true;
    for (size_t i = 0; i < line.size(); ++i) {
        const Token& t = line[i];
        if (t.type == Token::Preprocessor) {
            if (!bol) os << "\n";
            os << t.text << "\n";
            return;
        }
        bool space = !bol;
        if (t.type == Token::Punct) {
            if (t.text == "," || t.text == ")" || t.text == "]" ||
                t.text == ";")
                space = false;
            if (t.text == "(" || t.text == "[" || t.text == ".") { /*stick*/
            }
        }
        if (t.type == Token::Operator && t.text == "->") { /*stick*/
        }
        if (space) os << " ";
        os << t.text;
        bol = false;
    }
    os << "\n";
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "Usage: %s <file1.cp> [file2.cp ...]\n", argv[0]);
        return 1;
    }

    std::set<std::string> known_types = builtin_types();

    int exit_code = 0;
    for (int ai = 1; ai < argc; ++ai) {
        const char* inpath = argv[ai];
        std::string src;
        if (!read_file(inpath, src)) {
            std::fprintf(stderr, "Error: cannot read: %s\n", inpath);
            exit_code = 1;
            continue;
        }

        std::string pre = preprocess_physical_lines(src);
        std::vector<Token> toks;
        lex(pre, toks);

        std::vector<Scope> scopes;
        std::vector<std::map<std::string, VarInfo> > scope_vars;
        // known_types starts with builtins and grows per file (typedefs add to
        // it).
        analyze_scopes_and_vars(toks, scopes, scope_vars, known_types);

        remove_semicolons_inside_enums(toks, scopes);
        add_semicolon_after_type_blocks(toks, scopes);

        std::vector<std::vector<Token> > lines;
        std::vector<int> line_scope;
        split_into_lines(toks, lines, line_scope);

        std::ostringstream outcpp;
        for (size_t li = 0; li < lines.size(); ++li) {
            std::vector<Token>& line = lines[li];
            int sid = (li < line_scope.size() ? line_scope[li] : 0);

            // '.' to '->' (scope-aware; handles arrays, calls; wraps (**+) as
            // (*x) before '->')
            rewrite_member_chains(line, sid, scopes, scope_vars);

            const std::string& kind =
                (sid < (int)scopes.size() ? scopes[sid].kind
                    : std::string("Global"));
            insert_semicolon_before_closing_brace_on_line(line, kind);

            if (!line.empty() && needs_semicolon(line, kind)) {
                Token semi;
                semi.type = Token::Punct;
                semi.text = ";";
                semi.line = line.back().line;
                semi.col = line.back().col + 1;
                line.push_back(semi);
            }
            emit_line(line, outcpp);
        }

        std::string outpath = replace_ext(inpath, ".cpp");
        if (!write_text_file(outpath, outcpp.str())) {
            std::fprintf(stderr, "Error: cannot write: %s\n", outpath.c_str());
            exit_code = 1;
            continue;
        }
        std::fprintf(stderr, "Wrote %s\n", outpath.c_str());
    }

    return exit_code;
}
