#pragma once

// Small host-only cJSON-compatible reader used to compile the production
// CapsuleMetadataCodec. It implements the object/string/number/bool surface
// exercised by capsule metadata fixtures; firmware still links ESP-IDF cJSON.

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <map>
#include <sstream>
#include <string>

enum {
  cJSON_Invalid = 0,
  cJSON_False = 1,
  cJSON_True = 2,
  cJSON_NULL = 4,
  cJSON_Number = 8,
  cJSON_String = 16,
  cJSON_Object = 64,
};

struct cJSON {
  int type = cJSON_Invalid;
  char *valuestring = nullptr;
  int valueint = 0;
  std::map<std::string, cJSON *> object;
};

namespace cjson_host {

inline void skip(const char *&cursor, const char *end) {
  while (cursor < end && std::isspace(static_cast<unsigned char>(*cursor))) {
    ++cursor;
  }
}

inline bool string(const char *&cursor, const char *end, std::string &value) {
  skip(cursor, end);
  if (cursor >= end || *cursor++ != '"') return false;
  value.clear();
  while (cursor < end) {
    const char c = *cursor++;
    if (c == '"') return true;
    if (c == '\\') {
      if (cursor >= end) return false;
      const char escaped = *cursor++;
      switch (escaped) {
        case '"': value += '"'; break;
        case '\\': value += '\\'; break;
        case '/': value += '/'; break;
        case 'b': value += '\b'; break;
        case 'f': value += '\f'; break;
        case 'n': value += '\n'; break;
        case 'r': value += '\r'; break;
        case 't': value += '\t'; break;
        default: return false;
      }
    } else {
      value += c;
    }
  }
  return false;
}

inline cJSON *value(const char *&cursor, const char *end);
inline void destroy(cJSON *node);

inline cJSON *object(const char *&cursor, const char *end) {
  skip(cursor, end);
  if (cursor >= end || *cursor++ != '{') return nullptr;
  cJSON *node = new cJSON();
  node->type = cJSON_Object;
  skip(cursor, end);
  if (cursor < end && *cursor == '}') {
    ++cursor;
    return node;
  }
  while (cursor < end) {
    std::string key;
    if (!string(cursor, end, key)) break;
    skip(cursor, end);
    if (cursor >= end || *cursor++ != ':') break;
    cJSON *child = value(cursor, end);
    if (child == nullptr) break;
    node->object[key] = child;
    skip(cursor, end);
    if (cursor < end && *cursor == ',') {
      ++cursor;
      continue;
    }
    if (cursor < end && *cursor == '}') {
      ++cursor;
      return node;
    }
    break;
  }
  destroy(node);
  return nullptr;
}

inline cJSON *value(const char *&cursor, const char *end) {
  skip(cursor, end);
  if (cursor >= end) return nullptr;
  if (*cursor == '{') return object(cursor, end);
  cJSON *node = new cJSON();
  if (*cursor == '"') {
    std::string decoded;
    if (!string(cursor, end, decoded)) {
      delete node;
      return nullptr;
    }
    node->type = cJSON_String;
    node->valuestring = static_cast<char *>(std::malloc(decoded.size() + 1));
    if (node->valuestring == nullptr) {
      delete node;
      return nullptr;
    }
    std::memcpy(node->valuestring, decoded.c_str(), decoded.size() + 1);
    return node;
  }
  if (end - cursor >= 4 && std::strncmp(cursor, "true", 4) == 0) {
    cursor += 4;
    node->type = cJSON_True;
    return node;
  }
  if (end - cursor >= 5 && std::strncmp(cursor, "false", 5) == 0) {
    cursor += 5;
    node->type = cJSON_False;
    return node;
  }
  if (end - cursor >= 4 && std::strncmp(cursor, "null", 4) == 0) {
    cursor += 4;
    node->type = cJSON_NULL;
    return node;
  }
  char *parsedEnd = nullptr;
  const long number = std::strtol(cursor, &parsedEnd, 10);
  if (parsedEnd == cursor || parsedEnd > end) {
    delete node;
    return nullptr;
  }
  cursor = parsedEnd;
  node->type = cJSON_Number;
  node->valueint = static_cast<int>(number);
  return node;
}

inline void destroy(cJSON *node) {
  if (node == nullptr) return;
  for (auto &entry : node->object) destroy(entry.second);
  std::free(node->valuestring);
  delete node;
}

}  // namespace cjson_host

inline cJSON *cJSON_ParseWithLength(const char *value, size_t length) {
  if (value == nullptr) return nullptr;
  const char *cursor = value;
  const char *end = value + length;
  cJSON *root = cjson_host::object(cursor, end);
  cjson_host::skip(cursor, end);
  if (root == nullptr || cursor != end) {
    cjson_host::destroy(root);
    return nullptr;
  }
  return root;
}

inline void cJSON_Delete(cJSON *node) { cjson_host::destroy(node); }

inline cJSON *cJSON_GetObjectItemCaseSensitive(const cJSON *root,
                                               const char *name) {
  if (root == nullptr || root->type != cJSON_Object || name == nullptr) {
    return nullptr;
  }
  const auto found = root->object.find(name);
  return found == root->object.end() ? nullptr : found->second;
}

inline bool cJSON_IsString(const cJSON *node) {
  return node != nullptr && node->type == cJSON_String;
}
inline bool cJSON_IsNumber(const cJSON *node) {
  return node != nullptr && node->type == cJSON_Number;
}
inline bool cJSON_IsTrue(const cJSON *node) {
  return node != nullptr && node->type == cJSON_True;
}

inline cJSON *cJSON_CreateObject() {
  cJSON *node = new cJSON();
  node->type = cJSON_Object;
  return node;
}

inline cJSON *cJSON_CreateString(const char *value) {
  cJSON *node = new cJSON();
  node->type = cJSON_String;
  const char *source = value == nullptr ? "" : value;
  node->valuestring = static_cast<char *>(std::malloc(std::strlen(source) + 1));
  if (node->valuestring == nullptr) {
    delete node;
    return nullptr;
  }
  std::strcpy(node->valuestring, source);
  return node;
}

inline cJSON *cJSON_CreateNumber(double value) {
  cJSON *node = new cJSON();
  node->type = cJSON_Number;
  node->valueint = static_cast<int>(value);
  return node;
}

inline cJSON *cJSON_CreateBool(int value) {
  cJSON *node = new cJSON();
  node->type = value ? cJSON_True : cJSON_False;
  return node;
}

inline cJSON *cJSON_CreateNull() {
  cJSON *node = new cJSON();
  node->type = cJSON_NULL;
  return node;
}

inline void cJSON_ReplaceItemInObjectCaseSensitive(cJSON *object,
                                                    const char *name,
                                                    cJSON *replacement) {
  if (object == nullptr || object->type != cJSON_Object || name == nullptr) {
    cJSON_Delete(replacement);
    return;
  }
  const auto found = object->object.find(name);
  if (found != object->object.end()) cJSON_Delete(found->second);
  object->object[name] = replacement;
}

inline void cJSON_AddNumberToObject(cJSON *object, const char *name,
                                    double value) {
  cJSON_ReplaceItemInObjectCaseSensitive(object, name,
                                         cJSON_CreateNumber(value));
}

inline void cJSON_AddStringToObject(cJSON *object, const char *name,
                                    const char *value) {
  cJSON_ReplaceItemInObjectCaseSensitive(object, name,
                                         cJSON_CreateString(value));
}

namespace cjson_host {

inline std::string escaped(const char *value) {
  std::string output;
  if (value == nullptr) return output;
  for (; *value != '\0'; ++value) {
    if (*value == '"' || *value == '\\') output += '\\';
    output += *value;
  }
  return output;
}

inline void encode(const cJSON *node, std::ostringstream &output) {
  if (node == nullptr) {
    output << "null";
  } else if (node->type == cJSON_Object) {
    output << '{';
    bool first = true;
    for (const auto &entry : node->object) {
      if (!first) output << ',';
      first = false;
      output << '"' << escaped(entry.first.c_str()) << "\":";
      encode(entry.second, output);
    }
    output << '}';
  } else if (node->type == cJSON_String) {
    output << '"' << escaped(node->valuestring) << '"';
  } else if (node->type == cJSON_Number) {
    output << node->valueint;
  } else if (node->type == cJSON_True) {
    output << "true";
  } else if (node->type == cJSON_False) {
    output << "false";
  } else {
    output << "null";
  }
}

}  // namespace cjson_host

inline char *cJSON_Print(const cJSON *node) {
  std::ostringstream output;
  cjson_host::encode(node, output);
  const std::string value = output.str();
  char *encoded = static_cast<char *>(std::malloc(value.size() + 1));
  if (encoded != nullptr) std::memcpy(encoded, value.c_str(), value.size() + 1);
  return encoded;
}

inline void cJSON_free(void *value) { std::free(value); }
