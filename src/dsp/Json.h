// Json.h -- minimal JSON value + parser (objects, arrays, strings, numbers, bool, null).  No external dependency.
#pragma once
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace cd57 {

struct Json {
    enum Type { Null, Bool, Number, String, Array, Object } type = Null;
    bool b = false;
    double num = 0.0;
    std::string str;
    std::vector<Json> arr;
    std::vector<std::pair<std::string, Json>> obj;   // insertion order preserved

    bool isNull() const { return type == Null; }
    bool isNumber() const { return type == Number; }
    const Json* get(const std::string& k) const { for (auto& p : obj) if (p.first == k) return &p.second; return nullptr; }
    double number(const std::string& k, double dflt) const { auto* j = get(k); return j && j->isNumber() ? j->num : dflt; }
    std::string string(const std::string& k, const std::string& dflt = "") const { auto* j = get(k); return j && j->type == String ? j->str : dflt; }

    // Serialise (compact, insertion order preserved).  Numbers use 17 significant digits so a parse/dump round trip is exact.
    std::string dump() const
    {
        std::string o;
        dumpTo(o);
        return o;
    }

    static Json parse(const std::string& s)
    {
        size_t i = 0;
        Json v = value(s, i);
        skip(s, i);
        if (i != s.size()) throw std::runtime_error("trailing characters in JSON");
        return v;
    }

private:
    static void dumpString(const std::string& v, std::string& o)
    {
        o += '"';
        for (char c : v) {
            switch (c) {
                case '"': o += "\\\""; break; case '\\': o += "\\\\"; break; case '\n': o += "\\n"; break; case '\t': o += "\\t"; break;
                default: if ((unsigned char)c < 0x20) o += ' '; else o += c;
            }
        }
        o += '"';
    }
    void dumpTo(std::string& o) const
    {
        switch (type) {
            case Null: o += "null"; break;
            case Bool: o += b ? "true" : "false"; break;
            case Number: { char buf[40]; std::snprintf(buf, sizeof buf, "%.17g", num); o += buf; break; }
            case String: dumpString(str, o); break;
            case Array: o += '['; for (size_t i = 0; i < arr.size(); ++i) { if (i) o += ','; arr[i].dumpTo(o); } o += ']'; break;
            case Object: o += '{'; for (size_t i = 0; i < obj.size(); ++i) { if (i) o += ','; dumpString(obj[i].first, o); o += ':'; obj[i].second.dumpTo(o); } o += '}'; break;
        }
    }
    static void skip(const std::string& s, size_t& i) { while (i < s.size() && std::isspace((unsigned char)s[i])) ++i; }
    static Json value(const std::string& s, size_t& i)
    {
        skip(s, i);
        if (i >= s.size()) throw std::runtime_error("unexpected end of JSON");
        const char c = s[i];
        Json j;
        if (c == '{') {
            j.type = Object; ++i; skip(s, i);
            if (s[i] == '}') { ++i; return j; }
            for (;;) {
                skip(s, i);
                Json k = value(s, i);
                if (k.type != String) throw std::runtime_error("object key must be a string");
                skip(s, i);
                if (s[i++] != ':') throw std::runtime_error("expected ':'");
                j.obj.emplace_back(k.str, value(s, i));
                skip(s, i);
                if (s[i] == ',') { ++i; continue; }
                if (s[i] == '}') { ++i; break; }
                throw std::runtime_error("expected ',' or '}'");
            }
        } else if (c == '[') {
            j.type = Array; ++i; skip(s, i);
            if (s[i] == ']') { ++i; return j; }
            for (;;) {
                j.arr.push_back(value(s, i));
                skip(s, i);
                if (s[i] == ',') { ++i; continue; }
                if (s[i] == ']') { ++i; break; }
                throw std::runtime_error("expected ',' or ']'");
            }
        } else if (c == '"') {
            j.type = String; ++i;
            while (i < s.size() && s[i] != '"') {
                if (s[i] == '\\' && i + 1 < s.size()) {
                    ++i;
                    switch (s[i]) { case 'n': j.str += '\n'; break; case 't': j.str += '\t'; break; case 'u': j.str += '?'; i += 4; break; default: j.str += s[i]; }
                } else j.str += s[i];
                ++i;
            }
            ++i;
        } else if (s.compare(i, 4, "true") == 0) { j.type = Bool; j.b = true; i += 4; }
        else if (s.compare(i, 5, "false") == 0) { j.type = Bool; i += 5; }
        else if (s.compare(i, 4, "null") == 0) { j.type = Null; i += 4; }
        else {
            char* end = nullptr;
            j.num = std::strtod(s.c_str() + i, &end);
            if (end == s.c_str() + i) throw std::runtime_error("bad JSON token");
            j.type = Number; i = (size_t)(end - s.c_str());
        }
        return j;
    }
};

} // namespace cd57
