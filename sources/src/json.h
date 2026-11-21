// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jérôme Soler — JSG Guitar
#pragma once
/*
    Petit lecteur / écrivain JSON pour les messages entre l'interface et le programme.
*/

#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace jsg::json
{

struct Value
{
    enum Type { null, boolean, number, string, array, object } type = null;
    bool b = false;
    double n = 0.0;
    std::string s;
    std::vector<Value> items;
    std::map<std::string, Value> fields;

    const Value& operator[] (const std::string& key) const
    {
        static const Value none;
        auto it = fields.find (key);
        return it != fields.end() ? it->second : none;
    }

    double num (double fallback = 0.0) const       { return type == number ? n : (type == boolean ? (b ? 1.0 : 0.0) : fallback); }
    int    integer (int fallback = 0) const        { return type == number ? (int) std::lround (n) : (type == boolean ? (int) b : fallback); }
    bool   truthy() const                          { return type == boolean ? b : (type == number ? n != 0.0 : false); }
    const std::string& str() const                 { return s; }
    bool   has (const std::string& key) const      { return fields.count (key) > 0; }
};

class Parser
{
public:
    explicit Parser (const std::string& text) : t (text) {}

    bool parse (Value& out)
    {
        i = 0;
        if (! value (out)) return false;
        skip();
        return i == t.size();
    }

private:
    void skip() { while (i < t.size() && (t[i] == ' ' || t[i] == '\n' || t[i] == '\r' || t[i] == '\t')) ++i; }

    bool literal (const char* word)
    {
        size_t k = 0;
        while (word[k] != 0) { if (i + k >= t.size() || t[i + k] != word[k]) return false; ++k; }
        i += k;
        return true;
    }

    bool value (Value& v)
    {
        skip();
        if (i >= t.size()) return false;
        const char c = t[i];
        if (c == '{') return object (v);
        if (c == '[') return array (v);
        if (c == '"') { v.type = Value::string; return string (v.s); }
        if (c == 't') { v.type = Value::boolean; v.b = true;  return literal ("true"); }
        if (c == 'f') { v.type = Value::boolean; v.b = false; return literal ("false"); }
        if (c == 'n') { v.type = Value::null; return literal ("null"); }
        return number (v);
    }

    bool number (Value& v)
    {
        const char* start = t.c_str() + i;
        char* end = nullptr;
        v.n = std::strtod (start, &end);
        if (end == start) return false;
        i += (size_t) (end - start);
        v.type = Value::number;
        return true;
    }

    static void putUtf8 (std::string& out, unsigned cp)
    {
        if (cp < 0x80) out += (char) cp;
        else if (cp < 0x800) { out += (char) (0xC0 | (cp >> 6)); out += (char) (0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { out += (char) (0xE0 | (cp >> 12)); out += (char) (0x80 | ((cp >> 6) & 0x3F)); out += (char) (0x80 | (cp & 0x3F)); }
        else { out += (char) (0xF0 | (cp >> 18)); out += (char) (0x80 | ((cp >> 12) & 0x3F)); out += (char) (0x80 | ((cp >> 6) & 0x3F)); out += (char) (0x80 | (cp & 0x3F)); }
    }

    bool hex4 (unsigned& cp)
    {
        if (i + 4 > t.size()) return false;
        cp = (unsigned) std::stoul (t.substr (i, 4), nullptr, 16);
        i += 4;
        return true;
    }

    bool string (std::string& out)
    {
        ++i;   // "
        out.clear();
        while (i < t.size())
        {
            const char c = t[i++];
            if (c == '"') return true;
            if (c != '\\') { out += c; continue; }
            if (i >= t.size()) return false;
            const char e = t[i++];
            switch (e)
            {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'u':
                {
                    unsigned cp = 0;
                    if (! hex4 (cp)) return false;
                    if (cp >= 0xD800 && cp < 0xDC00 && i + 6 <= t.size() && t[i] == '\\' && t[i + 1] == 'u')
                    {
                        i += 2;
                        unsigned low = 0;
                        if (! hex4 (low)) return false;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                    }
                    putUtf8 (out, cp);
                    break;
                }
                default: out += e; break;
            }
        }
        return false;
    }

    bool array (Value& v)
    {
        v.type = Value::array;
        ++i;
        skip();
        if (i < t.size() && t[i] == ']') { ++i; return true; }
        while (true)
        {
            Value item;
            if (! value (item)) return false;
            v.items.push_back (std::move (item));
            skip();
            if (i >= t.size()) return false;
            if (t[i] == ',') { ++i; continue; }
            if (t[i] == ']') { ++i; return true; }
            return false;
        }
    }

    bool object (Value& v)
    {
        v.type = Value::object;
        ++i;
        skip();
        if (i < t.size() && t[i] == '}') { ++i; return true; }
        while (true)
        {
            skip();
            std::string key;
            if (i >= t.size() || t[i] != '"' || ! string (key)) return false;
            skip();
            if (i >= t.size() || t[i] != ':') return false;
            ++i;
            Value item;
            if (! value (item)) return false;
            v.fields[key] = std::move (item);
            skip();
            if (i >= t.size()) return false;
            if (t[i] == ',') { ++i; continue; }
            if (t[i] == '}') { ++i; return true; }
            return false;
        }
    }

    const std::string& t;
    size_t i = 0;
};

inline bool parse (const std::string& text, Value& out) { return Parser (text).parse (out); }

/** Texte JSON d'une chaîne (guillemets compris). */
inline std::string quote (const std::string& s)
{
    std::string out = "\"";
    for (unsigned char c : s)
    {
        if (c == '"' || c == '\\') { out += '\\'; out += (char) c; }
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (c < 0x20) { char b[8]; std::snprintf (b, sizeof b, "\\u%04x", c); out += b; }
        else out += (char) c;
    }
    return out + "\"";
}

inline std::string num (double v, int decimals = 6)
{
    if (! std::isfinite (v)) return "0";
    char b[64];
    std::snprintf (b, sizeof b, "%.*f", decimals, v);
    std::string s = b;
    if (s.find ('.') != std::string::npos)
    {
        while (! s.empty() && s.back() == '0') s.pop_back();
        if (! s.empty() && s.back() == '.') s.pop_back();
    }
    return s == "-0" ? "0" : s;
}

} // namespace jsg::json
