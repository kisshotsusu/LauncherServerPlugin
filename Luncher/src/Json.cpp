#include "Json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace {

class Parser {
public:
    explicit Parser(const std::string& s) : p_(s.c_str()) { p0_ = p_; }

    Json parse(std::string* err) {
        skipWs();
        Json v = value();
        skipWs();
        if (err && *p_ != '\0') {
            *err = "trailing characters at offset " + std::to_string(offset());
        }
        return v;
    }

private:
    const char* p_;

    size_t offset() const { return static_cast<size_t>(p_ - p0_); }

    void skipWs() {
        while (*p_ == ' ' || *p_ == '\t' || *p_ == '\r' || *p_ == '\n') ++p_;
    }

    Json fail(const char* msg) {
        fail_ = msg;
        return Json();
    }

    Json value() {
        skipWs();
        switch (*p_) {
            case '{': return object();
            case '[': return array();
            case '"': return stringValue();
            case 't': return literal(true, "true");
            case 'f': return literal(false, "false");
            case 'n': return nullValue();
            default: return numberValue();
        }
    }

    Json object() {
        Json j; j.type = Json::Type::Object;
        ++p_;
        skipWs();
        if (*p_ == '}') { ++p_; return j; }
        for (;;) {
            skipWs();
            if (*p_ != '"') return fail("expected key string");
            std::string key = stringValue().str;
            skipWs();
            if (*p_ != ':') return fail("expected ':'");
            ++p_;
            j.obj[key] = value();
            skipWs();
            if (*p_ == ',') { ++p_; continue; }
            if (*p_ == '}') { ++p_; return j; }
            return fail("expected ',' or '}'");
        }
    }

    Json array() {
        Json j; j.type = Json::Type::Array;
        ++p_;
        skipWs();
        if (*p_ == ']') { ++p_; return j; }
        for (;;) {
            j.arr.push_back(value());
            skipWs();
            if (*p_ == ',') { ++p_; continue; }
            if (*p_ == ']') { ++p_; return j; }
            return fail("expected ',' or ']'");
        }
    }

    Json stringValue() {
        Json j; j.type = Json::Type::String;
        ++p_;
        std::string out;
        while (*p_ && *p_ != '"') {
            if (*p_ == '\\') {
                ++p_;
                switch (*p_) {
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'n': out += '\n'; break;
                    case 'r': out += '\r'; break;
                    case 't': out += '\t'; break;
                    case 'u': {
                        unsigned cp = 0;
                        for (int i = 0; i < 4; ++i) {
                            ++p_;
                            cp = (cp << 4) | hexVal(*p_);
                        }
                        if (cp >= 0xD800 && cp <= 0xDBFF && p_[1] == '\\' && p_[2] == 'u') {
                            unsigned lo = 0;
                            p_ += 2;
                            for (int i = 0; i < 4; ++i) {
                                ++p_;
                                lo = (lo << 4) | hexVal(*p_);
                            }
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        }
                        appendUtf8(out, cp);
                        break;
                    }
                    default: break;
                }
                ++p_;
            } else {
                out += *p_++;
            }
        }
        if (*p_ == '"') ++p_;
        j.str = out;
        return j;
    }

    static int hexVal(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return 0;
    }

    static void appendUtf8(std::string& out, unsigned cp) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    Json literal(bool v, const char* lit) {
        const char* q = lit;
        while (*q && *p_ == *q) { ++p_; ++q; }
        return Json::boolean(v);
    }

    Json nullValue() {
        const char* q = "null";
        while (*q && *p_ == *q) { ++p_; ++q; }
        return Json();
    }

    Json numberValue() {
        Json j; j.type = Json::Type::Number;
        std::string s;
        if (*p_ == '-') { s += *p_++; }
        while (*p_ >= '0' && *p_ <= '9') s += *p_++;
        if (*p_ == '.') { s += *p_++; while (*p_ >= '0' && *p_ <= '9') s += *p_++; }
        if (*p_ == 'e' || *p_ == 'E') {
            s += *p_++;
            if (*p_ == '+' || *p_ == '-') s += *p_++;
            while (*p_ >= '0' && *p_ <= '9') s += *p_++;
        }
        j.num = atof(s.c_str());
        return j;
    }

    std::string fail_;
    const char* p0_ = nullptr;
};

void writeString(std::ostream& os, const std::string& s) {
    os << '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"': os << "\\\""; break;
            case '\\': os << "\\\\"; break;
            case '\b': os << "\\b"; break;
            case '\f': os << "\\f"; break;
            case '\n': os << "\\n"; break;
            case '\r': os << "\\r"; break;
            case '\t': os << "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    os << buf;
                } else {
                    os << static_cast<char>(c);
                }
        }
    }
    os << '"';
}

void dumpValue(std::ostream& os, const Json& j, int indent, int depth) {
    switch (j.type) {
        case Json::Type::Null: os << "null"; break;
        case Json::Type::Bool: os << (j.b ? "true" : "false"); break;
        case Json::Type::Number: {
            char buf[64];
            if (std::floor(j.num) == j.num && std::fabs(j.num) < 1e15) {
                snprintf(buf, sizeof(buf), "%.0f", j.num);
            } else {
                snprintf(buf, sizeof(buf), "%.9g", j.num);
            }
            os << buf;
            break;
        }
        case Json::Type::String: writeString(os, j.str); break;
        case Json::Type::Array: {
            os << '[';
            if (!j.arr.empty()) {
                if (indent > 0) os << '\n';
                for (size_t i = 0; i < j.arr.size(); ++i) {
                    if (indent > 0) os << std::string(static_cast<size_t>(indent) * (depth + 1), ' ');
                    dumpValue(os, j.arr[i], indent, depth + 1);
                    if (i + 1 < j.arr.size()) os << ',';
                    if (indent > 0) os << '\n';
                }
                if (indent > 0) os << std::string(static_cast<size_t>(indent) * depth, ' ');
            }
            os << ']';
            break;
        }
        case Json::Type::Object: {
            os << '{';
            if (!j.obj.empty()) {
                if (indent > 0) os << '\n';
                size_t i = 0;
                for (const auto& kv : j.obj) {
                    if (indent > 0) os << std::string(static_cast<size_t>(indent) * (depth + 1), ' ');
                    writeString(os, kv.first);
                    os << (indent > 0 ? ": " : ":");
                    dumpValue(os, kv.second, indent, depth + 1);
                    if (++i < j.obj.size()) os << ',';
                    if (indent > 0) os << '\n';
                }
                if (indent > 0) os << std::string(static_cast<size_t>(indent) * depth, ' ');
            }
            os << '}';
            break;
        }
    }
}

}  // namespace

Json Json::parse(const std::string& text, std::string* err) {
    Parser parser(text);
    return parser.parse(err);
}

std::string Json::dump(int indent) const {
    std::ostringstream os;
    dumpValue(os, *this, indent, 0);
    return os.str();
}

const Json& Json::get(const std::string& key) const {
    static const Json nullJson;
    auto it = obj.find(key);
    return it == obj.end() ? nullJson : it->second;
}

const Json& Json::at(size_t i) const {
    static const Json nullJson;
    return i < arr.size() ? arr[i] : nullJson;
}

std::string Json::getString(const std::string& key, const std::string& def) const {
    auto it = obj.find(key);
    if (it == obj.end() || it->second.type != Type::String) return def;
    return it->second.str;
}

double Json::getNumber(const std::string& key, double def) const {
    auto it = obj.find(key);
    if (it == obj.end() || it->second.type != Type::Number) return def;
    return it->second.num;
}

bool Json::getBool(const std::string& key, bool def) const {
    auto it = obj.find(key);
    if (it == obj.end() || it->second.type != Type::Bool) return def;
    return it->second.b;
}

std::string Json::asString() const {
    if (type == Type::String) return str;
    if (type == Type::Number) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%.9g", num);
        return buf;
    }
    if (type == Type::Bool) return b ? "true" : "false";
    return "";
}
