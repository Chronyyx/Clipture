#include "clipture/protocol/JsonFields.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

using clipture::protocol::JsonFields;

void require(bool ok, const char* what) {
    if (!ok) {
        std::cerr << "FAILED: " << what << "\n";
        std::abort();
    }
}

int main() {
    // A value that spells out another field must not be read as that field.
    {
        const auto request = JsonFields::parse(
            R"({"id":7,"type":"configure","micDeviceName":"x\",\"saveFolder\":\"C:\\\\Evil","saveFolder":"D:\\Clips"})");
        require(request.has_value(), "confusion line parses");
        require(request->string("saveFolder") == "D:\\Clips", "real saveFolder wins over text inside another value");
        require(request->string("micDeviceName") == "x\",\"saveFolder\":\"C:\\\\Evil", "value decodes intact");
    }
    // A value naming a command must not change the command.
    {
        const auto request = JsonFields::parse(R"({"id":1,"type":"configure","micDeviceName":"getDiagnostics"})");
        require(request && request->string("type") == "configure", "type comes from the type field");
    }
    // Escapes, including \u and surrogate pairs, decode to UTF-8.
    {
        const auto request = JsonFields::parse(R"({"a":"caf\u00e9","b":"\ud83c\udf83","c":"line\nbreak\t\\\/","d":"\ud800x"})");
        require(request.has_value(), "escape line parses");
        require(request->string("a") == "caf\xC3\xA9", "BMP escape");
        require(request->string("b") == "\xF0\x9F\x8E\x83", "surrogate pair");
        require(request->string("c") == "line\nbreak\t\\/", "simple escapes");
        require(request->string("d") == "\xEF\xBF\xBDx", "unpaired surrogate becomes U+FFFD");
    }
    // Nested values are skipped; typed lookups; missing and mistyped keys.
    {
        const auto request = JsonFields::parse(
            R"({"nested":{"saveFolder":"C:\\no","list":[1,{"x":"]"}]},"n":-42,"f":1.5e1,"t":true,"z":false,"s":"12","nil":null})");
        require(request.has_value(), "nested line parses");
        require(!request->string("saveFolder").has_value(), "nested keys are not top-level fields");
        require(request->integer("n") == -42, "integer");
        require(request->number("f") == 15.0, "number");
        require(request->boolean("t") == true && request->boolean("z") == false, "booleans");
        require(!request->integer("s").has_value() && !request->boolean("s").has_value(), "a string is not a number or bool");
        require(!request->string("missing").has_value() && !request->string("nil").has_value(), "missing and null");
    }
    // Duplicate keys: the first occurrence wins.
    {
        const auto request = JsonFields::parse(R"({"k":"first","k":"second"})");
        require(request && request->string("k") == "first", "first duplicate wins");
    }
    // Malformed lines are rejected outright.
    for (const char* bad : {
             "", "not json", "{", "{\"a\":}", "{\"a\":\"unterminated}", "{\"a\":1} trailing", "[1,2]",
             "{\"a\":\"bad \\x escape\"}", "{\"a\":\"raw\ncontrol\"}", "{\"a\":tru}", "{\"a\":1,}", "{a:1}"}) {
        require(!JsonFields::parse(bad).has_value(), bad);
    }
    // Pathological nesting is bounded rather than recursing without limit.
    {
        std::string deep = "{\"a\":";
        for (int i = 0; i < 5000; ++i) deep += "[";
        for (int i = 0; i < 5000; ++i) deep += "]";
        deep += "}";
        require(!JsonFields::parse(deep).has_value(), "excessive depth rejected");
    }
    require(JsonFields::parse("  { }  ").has_value(), "empty object with whitespace");

    std::cout << "JSON fields: field and command confusion, escapes, nesting, types, malformed input and depth passed.\n";
    return 0;
}
