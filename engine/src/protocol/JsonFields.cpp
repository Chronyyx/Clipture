#include "clipture/protocol/JsonFields.hpp"

#include <cctype>
#include <charconv>
#include <cstdlib>

namespace clipture::protocol {
namespace {

constexpr int kMaximumDepth = 32;

class Cursor {
public:
    explicit Cursor(std::string_view text) : text_(text) {}

    void skipSpace() {
        while (pos_ < text_.size() && (text_[pos_] == ' ' || text_[pos_] == '\t' || text_[pos_] == '\r' || text_[pos_] == '\n')) ++pos_;
    }
    bool consume(char ch) {
        skipSpace();
        if (pos_ < text_.size() && text_[pos_] == ch) { ++pos_; return true; }
        return false;
    }
    bool atEnd() { skipSpace(); return pos_ == text_.size(); }
    char peek() { skipSpace(); return pos_ < text_.size() ? text_[pos_] : '\0'; }
    std::size_t pos() const { return pos_; }

    // Skips one string literal, validating escapes; the cursor ends after it.
    bool skipString() {
        if (!consume('"')) return false;
        while (pos_ < text_.size()) {
            const char ch = text_[pos_++];
            if (ch == '"') return true;
            if (static_cast<unsigned char>(ch) < 0x20) return false;
            if (ch == '\\') {
                if (pos_ >= text_.size()) return false;
                const char escape = text_[pos_++];
                if (escape == 'u') {
                    if (pos_ + 4 > text_.size()) return false;
                    for (int i = 0; i < 4; ++i) if (!std::isxdigit(static_cast<unsigned char>(text_[pos_ + i]))) return false;
                    pos_ += 4;
                } else if (std::string_view("\"\\/bfnrt").find(escape) == std::string_view::npos) {
                    return false;
                }
            }
        }
        return false;
    }

    bool skipValue(int depth) {
        if (depth > kMaximumDepth) return false;
        const char ch = peek();
        if (ch == '"') return skipString();
        if (ch == '{' || ch == '[') {
            const char close = ch == '{' ? '}' : ']';
            ++pos_;
            if (consume(close)) return true;
            do {
                if (ch == '{' && (!skipString() || !consume(':'))) return false;
                if (!skipValue(depth + 1)) return false;
            } while (consume(','));
            return consume(close);
        }
        const std::size_t start = pos_;
        while (pos_ < text_.size() && std::string_view(",}] \t\r\n").find(text_[pos_]) == std::string_view::npos) ++pos_;
        const std::string_view literal = text_.substr(start, pos_ - start);
        if (literal == "true" || literal == "false" || literal == "null") return true;
        char* end = nullptr;
        const std::string copy(literal);
        std::strtod(copy.c_str(), &end);
        return !literal.empty() && end == copy.c_str() + copy.size();
    }

private:
    std::string_view text_;
    std::size_t pos_ = 0;
};

void appendUtf8(std::string& out, std::uint32_t code) {
    if (code < 0x80) {
        out.push_back(static_cast<char>(code));
    } else if (code < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (code >> 6)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else if (code < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (code >> 12)));
        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (code >> 18)));
        out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    }
}

std::uint32_t hex4(std::string_view text, std::size_t at) {
    std::uint32_t value = 0;
    std::from_chars(text.data() + at, text.data() + at + 4, value, 16);
    return value;
}

// Decodes an already-validated string literal (quotes included).
std::string decodeString(std::string_view literal) {
    std::string out;
    out.reserve(literal.size());
    for (std::size_t i = 1; i + 1 < literal.size(); ++i) {
        const char ch = literal[i];
        if (ch != '\\') { out.push_back(ch); continue; }
        const char escape = literal[++i];
        switch (escape) {
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
                std::uint32_t code = hex4(literal, i + 1);
                i += 4;
                if (code >= 0xD800 && code <= 0xDBFF && i + 6 < literal.size() && literal[i + 1] == '\\' && literal[i + 2] == 'u') {
                    const std::uint32_t low = hex4(literal, i + 3);
                    if (low >= 0xDC00 && low <= 0xDFFF) {
                        code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                        i += 6;
                    }
                }
                if (code >= 0xD800 && code <= 0xDFFF) code = 0xFFFD;  // unpaired surrogate
                appendUtf8(out, code);
                break;
            }
            default: out.push_back(escape); break;  // \" \\ \/
        }
    }
    return out;
}

}  // namespace

std::optional<JsonFields> JsonFields::parse(std::string_view line) {
    JsonFields result;
    result.text_ = std::string(line);
    Cursor cursor(result.text_);
    if (!cursor.consume('{')) return std::nullopt;
    if (!cursor.consume('}')) {
        do {
            cursor.skipSpace();
            const std::size_t keyStart = cursor.pos();
            if (!cursor.skipString()) return std::nullopt;
            const std::size_t keyEnd = cursor.pos();
            if (!cursor.consume(':')) return std::nullopt;
            std::string key = decodeString(std::string_view(result.text_).substr(keyStart, keyEnd - keyStart));
            cursor.skipSpace();
            const std::size_t valueStart = cursor.pos();
            if (!cursor.skipValue(1)) return std::nullopt;
            result.fields_.emplace_back(std::move(key), std::pair{valueStart, cursor.pos() - valueStart});
        } while (cursor.consume(','));
        if (!cursor.consume('}')) return std::nullopt;
    }
    if (!cursor.atEnd()) return std::nullopt;
    return result;
}

std::optional<std::string_view> JsonFields::raw(std::string_view key) const {
    // Duplicate keys: the first occurrence wins, as it did before.
    for (const auto& [name, span] : fields_) {
        if (name == key) return std::string_view(text_).substr(span.first, span.second);
    }
    return std::nullopt;
}

std::optional<std::string> JsonFields::string(std::string_view key) const {
    const auto value = raw(key);
    if (!value || value->empty() || value->front() != '"') return std::nullopt;
    return decodeString(*value);
}

std::optional<std::int64_t> JsonFields::integer(std::string_view key) const {
    const auto value = raw(key);
    if (!value) return std::nullopt;
    std::int64_t result = 0;
    const auto [end, error] = std::from_chars(value->data(), value->data() + value->size(), result);
    if (error != std::errc() || end != value->data() + value->size()) return std::nullopt;
    return result;
}

std::optional<double> JsonFields::number(std::string_view key) const {
    const auto value = raw(key);
    if (!value || value->empty() || value->front() == '"') return std::nullopt;
    const std::string copy(*value);
    char* end = nullptr;
    const double result = std::strtod(copy.c_str(), &end);
    if (end != copy.c_str() + copy.size()) return std::nullopt;
    return result;
}

std::optional<bool> JsonFields::boolean(std::string_view key) const {
    const auto value = raw(key);
    if (!value) return std::nullopt;
    if (*value == "true") return true;
    if (*value == "false") return false;
    return std::nullopt;
}

}  // namespace clipture::protocol
