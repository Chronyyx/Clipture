#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace clipture::protocol {

// The top-level fields of one JSON object (one stdio request line). Values are
// kept as raw JSON text and decoded on lookup. A real parse, rather than a
// substring search, means text inside one string value can never be mistaken
// for another field.
class JsonFields {
public:
    // nullopt unless `line` is exactly one well-formed JSON object.
    static std::optional<JsonFields> parse(std::string_view line);

    std::optional<std::string> string(std::string_view key) const;
    std::optional<std::int64_t> integer(std::string_view key) const;
    std::optional<double> number(std::string_view key) const;
    std::optional<bool> boolean(std::string_view key) const;

private:
    std::optional<std::string_view> raw(std::string_view key) const;

    std::string text_;
    std::vector<std::pair<std::string, std::pair<std::size_t, std::size_t>>> fields_;
};

}  // namespace clipture::protocol
