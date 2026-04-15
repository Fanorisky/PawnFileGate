namespace Json {
    inline std::string Escape(const std::string& s) {
        std::string out;
        out.reserve(s.size() + 16);
        for (unsigned char c : s) {
            switch (c) {
                case '\"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\b': out += "\\b"; break;
                case '\f': out += "\\f"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (c < 0x20) {
                        char buf[8];
                        std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                        out += buf;
                    } else {
                        out += static_cast<char>(c);
                    }
                    break;
            }
        }
        return out;
    }

    inline std::string Unescape(const std::string& s) {
        std::string out;
        out.reserve(s.size());
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '\\' && i + 1 < s.size()) {
                switch (s[i + 1]) {
                    case '"': out += '"'; ++i; break;
                    case '\\': out += '\\'; ++i; break;
                    case '/': out += '/'; ++i; break;
                    case 'b': out += '\b'; ++i; break;
                    case 'f': out += '\f'; ++i; break;
                    case 'n': out += '\n'; ++i; break;
                    case 'r': out += '\r'; ++i; break;
                    case 't': out += '\t'; ++i; break;
                    case 'u':
                        if (i + 5 < s.size()) {
                            char hex[5] = { s[i+2], s[i+3], s[i+4], s[i+5], 0 };
                            unsigned int cp = std::strtoul(hex, nullptr, 16);
                            if (cp < 0x80) out += static_cast<char>(cp);
                            i += 5;
                        }
                        break;
                    default: out += s[i]; break;
                }
            } else {
                out += s[i];
            }
        }
        return out;
    }

    inline std::string Str(const std::string& key, const std::string& value, bool comma = true) {
        return "\"" + Escape(key) + "\":\"" + Escape(value) + "\"" + (comma ? "," : "");
    }

    inline std::string Num(const std::string& key, long long value, bool comma = true) {
        return "\"" + Escape(key) + "\":" + std::to_string(value) + (comma ? "," : "");
    }

    inline std::string Float(const std::string& key, double value, bool comma = true) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.6f", value);
        return "\"" + Escape(key) + "\":" + std::string(buf) + (comma ? "," : "");
    }

    inline std::string Bool(const std::string& key, bool value, bool comma = true) {
        return "\"" + Escape(key) + "\":" + std::string(value ? "true" : "false") + (comma ? "," : "");
    }

    inline std::string Null(const std::string& key, bool comma = true) {
        return "\"" + Escape(key) + "\":null" + (comma ? "," : "");
    }

    inline std::string Obj(std::initializer_list<std::string> fields) {
        std::string out = "{";
        for (const auto& f : fields) out += f;
        if (!out.empty() && out.back() == ',') out.pop_back();
        out += "}";
        return out;
    }

    // ═══════════════════════════════════════════════════════════════════════
    // JSON Parser - Simple recursive descent parser for request bodies
    // ═══════════════════════════════════════════════════════════════════════
    
    inline void SkipWhitespace(const std::string& s, size_t& pos) {
        while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos]))) ++pos;
    }

    inline bool ParseString(const std::string& s, size_t& pos, std::string& out) {
        SkipWhitespace(s, pos);
        if (pos >= s.size() || s[pos] != '"') return false;
        ++pos;
        
        out.clear();
        while (pos < s.size() && s[pos] != '"') {
            if (s[pos] == '\\' && pos + 1 < s.size()) {
                ++pos;
                switch (s[pos]) {
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'n': out += '\n'; break;
                    case 'r': out += '\r'; break;
                    case 't': out += '\t'; break;
                    default: out += s[pos]; break;
                }
            } else {
                out += s[pos];
            }
            ++pos;
        }
        if (pos >= s.size()) return false;
        ++pos; // skip closing quote
        return true;
    }

    inline bool ParseNumber(const std::string& s, size_t& pos, double& out) {
        SkipWhitespace(s, pos);
        size_t start = pos;
        
        if (pos < s.size() && s[pos] == '-') ++pos;
        while (pos < s.size() && std::isdigit(static_cast<unsigned char>(s[pos]))) ++pos;
        if (pos < s.size() && s[pos] == '.') {
            ++pos;
            while (pos < s.size() && std::isdigit(static_cast<unsigned char>(s[pos]))) ++pos;
        }
        if (pos < s.size() && (s[pos] == 'e' || s[pos] == 'E')) {
            ++pos;
            if (pos < s.size() && (s[pos] == '+' || s[pos] == '-')) ++pos;
            while (pos < s.size() && std::isdigit(static_cast<unsigned char>(s[pos]))) ++pos;
        }
        
        if (pos == start) return false;
        out = std::strtod(s.substr(start, pos - start).c_str(), nullptr);
        return true;
    }

    inline bool ParseBool(const std::string& s, size_t& pos, bool& out) {
        SkipWhitespace(s, pos);
        if (s.compare(pos, 4, "true") == 0) {
            out = true;
            pos += 4;
            return true;
        }
        if (s.compare(pos, 5, "false") == 0) {
            out = false;
            pos += 5;
            return true;
        }
        return false;
    }

    inline bool ParseNull(const std::string& s, size_t& pos) {
        SkipWhitespace(s, pos);
        if (s.compare(pos, 4, "null") == 0) {
            pos += 4;
            return true;
        }
        return false;
    }

    // Skip a JSON value (for navigating to nested keys)
    inline bool SkipValue(const std::string& s, size_t& pos) {
        SkipWhitespace(s, pos);
        if (pos >= s.size()) return false;
        
        char c = s[pos];
        if (c == '"') {
            std::string dummy;
            return ParseString(s, pos, dummy);
        } else if (c == '{') {
            ++pos;
            SkipWhitespace(s, pos);
            if (pos < s.size() && s[pos] == '}') { ++pos; return true; }
            while (pos < s.size()) {
                std::string key;
                if (!ParseString(s, pos, key)) return false;
                SkipWhitespace(s, pos);
                if (pos >= s.size() || s[pos] != ':') return false;
                ++pos;
                if (!SkipValue(s, pos)) return false;
                SkipWhitespace(s, pos);
                if (pos < s.size() && s[pos] == '}') { ++pos; return true; }
                if (pos >= s.size() || s[pos] != ',') return false;
                ++pos;
            }
            return false;
        } else if (c == '[') {
            ++pos;
            SkipWhitespace(s, pos);
            if (pos < s.size() && s[pos] == ']') { ++pos; return true; }
            while (pos < s.size()) {
                if (!SkipValue(s, pos)) return false;
                SkipWhitespace(s, pos);
                if (pos < s.size() && s[pos] == ']') { ++pos; return true; }
                if (pos >= s.size() || s[pos] != ',') return false;
                ++pos;
            }
            return false;
        } else if (c == 't' || c == 'f') {
            bool dummy;
            return ParseBool(s, pos, dummy);
        } else if (c == 'n') {
            return ParseNull(s, pos);
        } else {
            double dummy;
            return ParseNumber(s, pos, dummy);
        }
    }

    // Find a key in JSON object, return position after ':'
    inline bool FindKey(const std::string& json, const std::string& key, size_t& outPos) {
        size_t pos = 0;
        SkipWhitespace(json, pos);
        if (pos >= json.size() || json[pos] != '{') return false;
        ++pos;
        
        while (pos < json.size()) {
            SkipWhitespace(json, pos);
            if (pos >= json.size()) return false;
            if (json[pos] == '}') return false;
            
            std::string k;
            if (!ParseString(json, pos, k)) return false;
            SkipWhitespace(json, pos);
            if (pos >= json.size() || json[pos] != ':') return false;
            ++pos;
            SkipWhitespace(json, pos);
            
            if (k == key) {
                outPos = pos;
                return true;
            }
            
            if (!SkipValue(json, pos)) return false;
            SkipWhitespace(json, pos);
            if (pos < json.size() && json[pos] == ',') ++pos;
        }
        return false;
    }

    // Find nested key using dot notation: "user.profile.name"
    inline bool FindNestedKey(const std::string& json, const std::string& path, size_t& outPos, std::string& subJson) {
        subJson = json;
        std::stringstream ss(path);
        std::string segment;
        
        while (std::getline(ss, segment, '.')) {
            // Check for array index: items[0]
            size_t bracketPos = segment.find('[');
            std::string key = segment;
            int arrayIndex = -1;
            
            if (bracketPos != std::string::npos) {
                key = segment.substr(0, bracketPos);
                size_t endBracket = segment.find(']', bracketPos);
                if (endBracket != std::string::npos) {
                    arrayIndex = std::atoi(segment.substr(bracketPos + 1, endBracket - bracketPos - 1).c_str());
                }
            }
            
            if (!key.empty()) {
                size_t valPos;
                if (!FindKey(subJson, key, valPos)) return false;
                
                // Extract the value as new subJson
                size_t start = valPos;
                size_t end = valPos;
                if (!SkipValue(subJson, end)) return false;
                subJson = subJson.substr(start, end - start);
            }
            
            // Handle array index
            if (arrayIndex >= 0) {
                size_t pos = 0;
                SkipWhitespace(subJson, pos);
                if (pos >= subJson.size() || subJson[pos] != '[') return false;
                ++pos;
                
                for (int i = 0; i <= arrayIndex; ++i) {
                    SkipWhitespace(subJson, pos);
                    if (i == arrayIndex) {
                        size_t start = pos;
                        size_t end = pos;
                        if (!SkipValue(subJson, end)) return false;
                        subJson = subJson.substr(start, end - start);
                        break;
                    }
                    if (!SkipValue(subJson, pos)) return false;
                    SkipWhitespace(subJson, pos);
                    if (pos < subJson.size() && subJson[pos] == ',') ++pos;
                }
            }
        }
        
        outPos = 0;
        return true;
    }

    // High-level getters
    inline std::string GetString(const std::string& json, const std::string& key, const std::string& def = "") {
        size_t pos;
        if (!FindKey(json, key, pos)) return def;
        std::string out;
        if (!ParseString(json, pos, out)) return def;
        return out;
    }

    inline int GetInt(const std::string& json, const std::string& key, int def = 0) {
        size_t pos;
        if (!FindKey(json, key, pos)) return def;
        double val;
        if (!ParseNumber(json, pos, val)) return def;
        return static_cast<int>(val);
    }

    inline double GetFloat(const std::string& json, const std::string& key, double def = 0.0) {
        size_t pos;
        if (!FindKey(json, key, pos)) return def;
        double val;
        if (!ParseNumber(json, pos, val)) return def;
        return val;
    }

    inline bool GetBool(const std::string& json, const std::string& key, bool def = false) {
        size_t pos;
        if (!FindKey(json, key, pos)) return def;
        bool val;
        if (!ParseBool(json, pos, val)) return def;
        return val;
    }

    inline bool HasKey(const std::string& json, const std::string& key) {
        size_t pos;
        return FindKey(json, key, pos);
    }

    inline int ArrayLength(const std::string& json, const std::string& key) {
        size_t pos;
        if (!key.empty()) {
            if (!FindKey(json, key, pos)) return -1;
        } else {
            pos = 0;
        }
        
        SkipWhitespace(json, pos);
        if (pos >= json.size() || json[pos] != '[') return -1;
        ++pos;
        SkipWhitespace(json, pos);
        if (pos >= json.size()) return -1;
        if (json[pos] == ']') return 0;
        
        int count = 0;
        while (pos < json.size()) {
            if (!SkipValue(json, pos)) return -1;
            ++count;
            SkipWhitespace(json, pos);
            if (pos < json.size() && json[pos] == ']') break;
            if (pos >= json.size() || json[pos] != ',') return -1;
            ++pos;
        }
        return count;
    }

    // Nested getters
    inline std::string GetNestedString(const std::string& json, const std::string& path, const std::string& def = "") {
        size_t pos;
        std::string sub;
        if (!FindNestedKey(json, path, pos, sub)) return def;
        
        SkipWhitespace(sub, pos);
        std::string out;
        if (!ParseString(sub, pos, out)) return def;
        return out;
    }

    inline int GetNestedInt(const std::string& json, const std::string& path, int def = 0) {
        size_t pos;
        std::string sub;
        if (!FindNestedKey(json, path, pos, sub)) return def;
        
        SkipWhitespace(sub, pos);
        double val;
        if (!ParseNumber(sub, pos, val)) return def;
        return static_cast<int>(val);
    }

    enum class NodeType {
        Number = 0,
        Boolean = 1,
        String = 2,
        Object = 3,
        Array = 4,
        Null = 5
    };

    struct Node {
        NodeType type = NodeType::Null;
        double numberValue = 0.0;
        bool boolValue = false;
        std::string stringValue;
        std::vector<std::pair<std::string, std::shared_ptr<Node>>> objectValue;
        std::vector<std::shared_ptr<Node>> arrayValue;
    };

    using NodePtr = std::shared_ptr<Node>;

    inline NodePtr MakeNull() {
        return std::make_shared<Node>();
    }

    inline NodePtr MakeNumber(double value) {
        auto node = std::make_shared<Node>();
        node->type = NodeType::Number;
        node->numberValue = value;
        return node;
    }

    inline NodePtr MakeBoolean(bool value) {
        auto node = std::make_shared<Node>();
        node->type = NodeType::Boolean;
        node->boolValue = value;
        return node;
    }

    inline NodePtr MakeString(const std::string& value) {
        auto node = std::make_shared<Node>();
        node->type = NodeType::String;
        node->stringValue = value;
        return node;
    }

    inline NodePtr MakeObject() {
        auto node = std::make_shared<Node>();
        node->type = NodeType::Object;
        return node;
    }

    inline NodePtr MakeArray() {
        auto node = std::make_shared<Node>();
        node->type = NodeType::Array;
        return node;
    }

    inline NodePtr Clone(const NodePtr& node) {
        if (!node) return nullptr;

        auto copy = std::make_shared<Node>();
        copy->type = node->type;
        copy->numberValue = node->numberValue;
        copy->boolValue = node->boolValue;
        copy->stringValue = node->stringValue;

        if (node->type == NodeType::Object) {
            copy->objectValue.reserve(node->objectValue.size());
            for (const auto& [key, value] : node->objectValue) {
                copy->objectValue.emplace_back(key, Clone(value));
            }
        } else if (node->type == NodeType::Array) {
            copy->arrayValue.reserve(node->arrayValue.size());
            for (const auto& value : node->arrayValue) {
                copy->arrayValue.push_back(Clone(value));
            }
        }

        return copy;
    }

    inline NodePtr GetObjectMember(const NodePtr& obj, const std::string& key) {
        if (!obj || obj->type != NodeType::Object) return nullptr;
        for (const auto& [memberKey, memberValue] : obj->objectValue) {
            if (memberKey == key) return memberValue;
        }
        return nullptr;
    }

    inline bool SetObjectMember(const NodePtr& obj, const std::string& key, const NodePtr& value) {
        if (!obj || obj->type != NodeType::Object || key.empty() || !value) return false;
        for (auto& [memberKey, memberValue] : obj->objectValue) {
            if (memberKey == key) {
                memberValue = value;
                return true;
            }
        }
        obj->objectValue.emplace_back(key, value);
        return true;
    }

    inline bool ParseNodeValue(const std::string& s, size_t& pos, NodePtr& out);

    inline bool ParseObjectNode(const std::string& s, size_t& pos, NodePtr& out) {
        SkipWhitespace(s, pos);
        if (pos >= s.size() || s[pos] != '{') return false;
        ++pos;

        auto obj = MakeObject();
        SkipWhitespace(s, pos);
        if (pos < s.size() && s[pos] == '}') {
            ++pos;
            out = obj;
            return true;
        }

        while (pos < s.size()) {
            std::string key;
            if (!ParseString(s, pos, key)) return false;

            SkipWhitespace(s, pos);
            if (pos >= s.size() || s[pos] != ':') return false;
            ++pos;

            NodePtr value;
            if (!ParseNodeValue(s, pos, value)) return false;
            obj->objectValue.emplace_back(key, value);

            SkipWhitespace(s, pos);
            if (pos < s.size() && s[pos] == '}') {
                ++pos;
                out = obj;
                return true;
            }
            if (pos >= s.size() || s[pos] != ',') return false;
            ++pos;
        }
        return false;
    }

    inline bool ParseArrayNode(const std::string& s, size_t& pos, NodePtr& out) {
        SkipWhitespace(s, pos);
        if (pos >= s.size() || s[pos] != '[') return false;
        ++pos;

        auto arr = MakeArray();
        SkipWhitespace(s, pos);
        if (pos < s.size() && s[pos] == ']') {
            ++pos;
            out = arr;
            return true;
        }

        while (pos < s.size()) {
            NodePtr value;
            if (!ParseNodeValue(s, pos, value)) return false;
            arr->arrayValue.push_back(value);

            SkipWhitespace(s, pos);
            if (pos < s.size() && s[pos] == ']') {
                ++pos;
                out = arr;
                return true;
            }
            if (pos >= s.size() || s[pos] != ',') return false;
            ++pos;
        }
        return false;
    }

    inline bool ParseNodeValue(const std::string& s, size_t& pos, NodePtr& out) {
        SkipWhitespace(s, pos);
        if (pos >= s.size()) return false;

        char c = s[pos];
        if (c == '{') {
            return ParseObjectNode(s, pos, out);
        }
        if (c == '[') {
            return ParseArrayNode(s, pos, out);
        }
        if (c == '"') {
            std::string str;
            if (!ParseString(s, pos, str)) return false;
            out = MakeString(str);
            return true;
        }
        if (c == 't' || c == 'f') {
            bool b = false;
            if (!ParseBool(s, pos, b)) return false;
            out = MakeBoolean(b);
            return true;
        }
        if (c == 'n') {
            if (!ParseNull(s, pos)) return false;
            out = MakeNull();
            return true;
        }

        double number = 0.0;
        if (!ParseNumber(s, pos, number)) return false;
        out = MakeNumber(number);
        return true;
    }

    inline bool ParseNode(const std::string& json, NodePtr& out) {
        size_t pos = 0;
        if (!ParseNodeValue(json, pos, out)) return false;
        SkipWhitespace(json, pos);
        return pos == json.size();
    }

    inline std::string StringifyNode(const NodePtr& node) {
        if (!node) return "null";

        switch (node->type) {
            case NodeType::Null:
                return "null";
            case NodeType::Boolean:
                return node->boolValue ? "true" : "false";
            case NodeType::Number: {
                char buf[64];
                std::snprintf(buf, sizeof(buf), "%.15g", node->numberValue);
                return buf;
            }
            case NodeType::String:
                return "\"" + Escape(node->stringValue) + "\"";
            case NodeType::Object: {
                std::string out = "{";
                for (size_t i = 0; i < node->objectValue.size(); ++i) {
                    if (i > 0) out += ",";
                    out += "\"" + Escape(node->objectValue[i].first) + "\":";
                    out += StringifyNode(node->objectValue[i].second);
                }
                out += "}";
                return out;
            }
            case NodeType::Array: {
                std::string out = "[";
                for (size_t i = 0; i < node->arrayValue.size(); ++i) {
                    if (i > 0) out += ",";
                    out += StringifyNode(node->arrayValue[i]);
                }
                out += "]";
                return out;
            }
        }
        return "null";
    }
}
