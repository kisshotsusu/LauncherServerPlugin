#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

// 极简 JSON 解析/序列化（无第三方依赖）
class Json {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type = Type::Null;
    bool b = false;
    double num = 0;
    std::string str;
    std::vector<Json> arr;
    std::map<std::string, Json> obj;

    Json() = default;

    static Json parse(const std::string& text, std::string* err = nullptr);
    std::string dump(int indent = 0) const;

    bool has(const std::string& key) const { return obj.count(key) > 0; }
    const Json& get(const std::string& key) const;
    const Json& at(size_t i) const;
    const std::vector<Json>& array() const { return arr; }
    const std::map<std::string, Json>& members() const { return obj; }

    std::string getString(const std::string& key, const std::string& def = "") const;
    double getNumber(const std::string& key, double def = 0) const;
    bool getBool(const std::string& key, bool def = false) const;

    bool isNull() const { return type == Type::Null; }
    std::string asString() const;

    static Json object() { Json j; j.type = Type::Object; return j; }
    static Json string(const std::string& s) { Json j; j.type = Type::String; j.str = s; return j; }
    static Json number(double n) { Json j; j.type = Type::Number; j.num = n; return j; }
    static Json boolean(bool v) { Json j; j.type = Type::Bool; j.b = v; return j; }
    static Json arrayValue() { Json j; j.type = Type::Array; return j; }
};
