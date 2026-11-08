#pragma once

// Shared, dependency-free JSON manifest reader for the import-isolation
// scoreboards. The repository has no general JSON dependency in the test
// binary, and both the 0.7.0 compatibility manifest and the 0.8.0 valid-file
// manifest use a small, closed schema. This parser is intentionally narrow
// (objects/arrays/strings/numbers/bools/null) rather than a second dependency.
//
// This helper owns only parsing, path/UTF-8 conversion and the closed error
// name table. Each scoreboard still owns its own entry schema and outcome
// semantics.

#include "model_core/ImportError.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace compat_test {

struct JsonValue {
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::string string;
    std::vector<JsonValue> array;
    std::map<std::string, JsonValue> object;

    const JsonValue& at(const std::string& key) const;
    const std::string& asString() const;
    bool asBool() const;
    const std::vector<JsonValue>& asArray() const;
    bool has(const std::string& key) const { return object.find(key) != object.end(); }
};

class JsonReader {
public:
    explicit JsonReader(const std::string& text) : text_(text) {}

    JsonValue parse();

private:
    const std::string& text_;
    size_t pos_ = 0;

    [[noreturn]] void fail(const char* message) const;
    void skipWhitespace();
    char peek() const;
    void expect(char c);
    JsonValue parseValue();
    JsonValue parseLiteral(const char* keyword, bool value);
    void parseKeyword(const char* keyword);
    JsonValue parseNumber();
    JsonValue parseObject();
    JsonValue parseArray();
    static void appendUtf8(std::string& out, uint32_t codePoint);
    uint32_t parseHex4();
    std::string parseString();
};

std::string ReadFileUtf8(const std::wstring& path);
std::wstring Utf8ToWide(const std::string& value);
std::string WideToUtf8(const std::wstring& value);
std::wstring JoinPath(const std::wstring& root, const std::wstring& relative);

// Reads and parses one JSON document. Throws std::runtime_error with a
// path-free message on read or syntax failure.
JsonValue ParseJsonFile(const std::wstring& path);

// Loads a schema-1 manifest root object and returns its "entries" array. The
// caller owns the document lifetime; the returned reference is into `root`.
const std::vector<JsonValue>& Schema1Entries(const JsonValue& root);

// Stable, path-free name for a closed ImportErrorCode, shared by both
// scoreboards and their reports.
const char* ImportErrorName(model_core::ImportErrorCode code);

} // namespace compat_test
