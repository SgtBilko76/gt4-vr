// Minimal JSON reader (from the gt4-results service, same author; no third-party code).
// Reads one JSON document into a tree; strings are validated UTF-8, \u escapes are decoded.
#pragma once
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace json {

struct Value {
    enum Type { Null, Bool, Number, String, Array, Object } type = Null;
    bool b = false;
    double num = 0;
    std::string str;
    std::vector<Value> arr;
    // Object members as two parallel vectors: std::vector<std::pair<std::string, Value>> needs a
    // complete Value, which clang + libstdc++ 15 reject here (vector<Value> alone is fine).
    std::vector<std::string> keys;
    std::vector<Value> vals;

    // Last occurrence wins for duplicate keys.
    const Value* get(const std::string& key) const {
        for (size_t i = keys.size(); i-- > 0;)
            if (keys[i] == key) return &vals[i];
        return nullptr;
    }
};

class Reader {
public:
    Reader(const char* begin, const char* end) : p_(begin), e_(end) {}

    // Parses exactly one value followed only by whitespace.
    bool parse(Value& out) {
        ws();
        if (!value(out, 0)) return false;
        ws();
        return p_ == e_;
    }

private:
    const char* p_;
    const char* e_;
    static constexpr int kMaxDepth = 16;

    void ws() {
        while (p_ < e_ && (*p_ == ' ' || *p_ == '\t' || *p_ == '\n' || *p_ == '\r')) ++p_;
    }
    bool lit(const char* s) {
        const char* q = p_;
        for (; *s; ++s, ++q)
            if (q >= e_ || *q != *s) return false;
        p_ = q;
        return true;
    }
    bool value(Value& v, int depth) {
        if (p_ >= e_ || depth > kMaxDepth) return false;
        switch (*p_) {
        case '{': return object(v, depth);
        case '[': return array(v, depth);
        case '"': v.type = Value::String; return string(v.str);
        case 't': v.type = Value::Bool; v.b = true; return lit("true");
        case 'f': v.type = Value::Bool; v.b = false; return lit("false");
        case 'n': v.type = Value::Null; return lit("null");
        default: return number(v);
        }
    }
    bool object(Value& v, int depth) {
        v.type = Value::Object;
        ++p_;
        ws();
        if (p_ < e_ && *p_ == '}') { ++p_; return true; }
        for (;;) {
            ws();
            if (p_ >= e_ || *p_ != '"') return false;
            std::string key;
            if (!string(key)) return false;
            ws();
            if (p_ >= e_ || *p_ != ':') return false;
            ++p_;
            ws();
            Value child;
            if (!value(child, depth + 1)) return false;
            v.keys.push_back(std::move(key));
            v.vals.push_back(std::move(child));
            ws();
            if (p_ >= e_) return false;
            if (*p_ == ',') { ++p_; continue; }
            if (*p_ == '}') { ++p_; return true; }
            return false;
        }
    }
    bool array(Value& v, int depth) {
        v.type = Value::Array;
        ++p_;
        ws();
        if (p_ < e_ && *p_ == ']') { ++p_; return true; }
        for (;;) {
            ws();
            Value child;
            if (!value(child, depth + 1)) return false;
            v.arr.push_back(std::move(child));
            ws();
            if (p_ >= e_) return false;
            if (*p_ == ',') { ++p_; continue; }
            if (*p_ == ']') { ++p_; return true; }
            return false;
        }
    }
    bool number(Value& v) {
        const char* s = p_;
        if (p_ < e_ && *p_ == '-') ++p_;
        if (p_ >= e_) return false;
        if (*p_ == '0') ++p_;
        else if (*p_ >= '1' && *p_ <= '9') while (p_ < e_ && *p_ >= '0' && *p_ <= '9') ++p_;
        else return false;
        if (p_ < e_ && *p_ == '.') {
            ++p_;
            if (p_ >= e_ || *p_ < '0' || *p_ > '9') return false;
            while (p_ < e_ && *p_ >= '0' && *p_ <= '9') ++p_;
        }
        if (p_ < e_ && (*p_ == 'e' || *p_ == 'E')) {
            ++p_;
            if (p_ < e_ && (*p_ == '+' || *p_ == '-')) ++p_;
            if (p_ >= e_ || *p_ < '0' || *p_ > '9') return false;
            while (p_ < e_ && *p_ >= '0' && *p_ <= '9') ++p_;
        }
        if (p_ - s > 40) return false;
        std::string tmp(s, p_);
        v.type = Value::Number;
        v.num = std::strtod(tmp.c_str(), nullptr);
        return true;
    }
    static void put_utf8(std::string& out, unsigned cp) {
        if (cp < 0x80) out += char(cp);
        else if (cp < 0x800) { out += char(0xC0 | (cp >> 6)); out += char(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) {
            out += char(0xE0 | (cp >> 12)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F));
        } else {
            out += char(0xF0 | (cp >> 18)); out += char(0x80 | ((cp >> 12) & 0x3F));
            out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F));
        }
    }
    bool hex4(unsigned& cp) {
        if (e_ - p_ < 4) return false;
        cp = 0;
        for (int i = 0; i < 4; ++i, ++p_) {
            char c = *p_;
            cp <<= 4;
            if (c >= '0' && c <= '9') cp |= unsigned(c - '0');
            else if (c >= 'a' && c <= 'f') cp |= unsigned(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') cp |= unsigned(c - 'A' + 10);
            else return false;
        }
        return true;
    }
    bool string(std::string& out) {
        ++p_;  // opening quote
        out.clear();
        while (p_ < e_) {
            unsigned char c = static_cast<unsigned char>(*p_);
            if (c == '"') { ++p_; return true; }
            if (c < 0x20) return false;
            if (c == '\\') {
                ++p_;
                if (p_ >= e_) return false;
                char x = *p_++;
                switch (x) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    unsigned cp;
                    if (!hex4(cp)) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        unsigned lo;
                        if (e_ - p_ < 6 || p_[0] != '\\' || p_[1] != 'u') return false;
                        p_ += 2;
                        if (!hex4(lo) || lo < 0xDC00 || lo > 0xDFFF) return false;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        return false;
                    }
                    put_utf8(out, cp);
                    break;
                }
                default: return false;
                }
                continue;
            }
            if (c < 0x80) { out += char(c); ++p_; continue; }
            // Raw UTF-8 sequence: validate (no overlongs, no surrogates, <= U+10FFFF).
            int n; unsigned cp;
            if (c >= 0xC2 && c <= 0xDF) { n = 1; cp = c & 0x1F; }
            else if (c >= 0xE0 && c <= 0xEF) { n = 2; cp = c & 0x0F; }
            else if (c >= 0xF0 && c <= 0xF4) { n = 3; cp = c & 0x07; }
            else return false;
            if (e_ - p_ < n + 1) return false;
            for (int i = 1; i <= n; ++i) {
                unsigned char cc = static_cast<unsigned char>(p_[i]);
                if ((cc & 0xC0) != 0x80) return false;
                cp = (cp << 6) | (cc & 0x3F);
            }
            if ((n == 2 && (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF))) || (n == 3 && (cp < 0x10000 || cp > 0x10FFFF)))
                return false;
            out.append(p_, size_t(n + 1));
            p_ += n + 1;
        }
        return false;
    }
};

// Quotes a UTF-8 string for JSON output. <, >, & are escaped too so the output is safe to embed anywhere.
inline void quote(std::string& out, const std::string& s) {
    static const char* hex = "0123456789abcdef";
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20 || c == '<' || c == '>' || c == '&' || c == 0x7F) {
                out += "\\u00"; out += hex[c >> 4]; out += hex[c & 15];
            } else {
                out += char(c);
            }
        }
    }
    out += '"';
}

}  // namespace json
