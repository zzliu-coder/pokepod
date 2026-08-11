#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "Arduino.h"

#define FILE_READ "r"
#define FILE_WRITE "w"

namespace fakefs {

enum class Operation : uint8_t {
  open,
  write,
  flush,
  seek,
  rename,
  remove,
  close,
};

enum class FaultAction : uint8_t {
  none,
  returnFailure,
  shortWrite,
  crashBefore,
  crashAfter,
};

struct SimulatedCrash final : public std::runtime_error {
  explicit SimulatedCrash(Operation operation)
      : std::runtime_error("simulated storage crash"), operation(operation) {}
  Operation operation;
};

struct FaultPlan {
  Operation operation = Operation::open;
  FaultAction action = FaultAction::none;
  uint32_t occurrence = 0;
  uint32_t seen = 0;
};

struct State {
  std::map<std::string, std::vector<uint8_t>> files;
  std::set<std::string> directories{"/"};
  FaultPlan fault;
  uint32_t operations = 0;

  void clearFault() { fault = {}; }

  void fail(Operation operation, uint32_t occurrence, FaultAction action) {
    fault.operation = operation;
    fault.action = action;
    fault.occurrence = occurrence;
    fault.seen = 0;
  }

  FaultAction before(Operation operation) {
    ++operations;
    if (fault.action == FaultAction::none || fault.operation != operation) {
      return FaultAction::none;
    }
    ++fault.seen;
    if (fault.seen != fault.occurrence) return FaultAction::none;
    if (fault.action == FaultAction::crashBefore) {
      throw SimulatedCrash(operation);
    }
    return fault.action;
  }

  void after(Operation operation, FaultAction action) {
    if (action == FaultAction::crashAfter) throw SimulatedCrash(operation);
  }

  void seed(const std::string &path, const std::string &value) {
    files[path] = std::vector<uint8_t>(value.begin(), value.end());
    addParents(path);
  }

  void seedBytes(const std::string &path, const uint8_t *bytes, size_t length) {
    files[path] = std::vector<uint8_t>(bytes, bytes + length);
    addParents(path);
  }

  std::string text(const std::string &path) const {
    const auto found = files.find(path);
    if (found == files.end()) return {};
    return std::string(found->second.begin(), found->second.end());
  }

  bool has(const std::string &path) const {
    return files.count(path) != 0 || directories.count(path) != 0;
  }

  void addParents(const std::string &path) {
    size_t slash = 0;
    while ((slash = path.find('/', slash + 1)) != std::string::npos) {
      directories.insert(path.substr(0, slash));
    }
  }
};

struct Handle {
  std::shared_ptr<State> state;
  std::string path;
  std::vector<uint8_t> buffer;
  std::vector<std::string> children;
  size_t position = 0;
  size_t childIndex = 0;
  int writeError = 0;
  bool open = false;
  bool directory = false;
  bool writable = false;
  bool dirty = false;
};

}  // namespace fakefs

class File {
 public:
  File() = default;
  explicit File(std::shared_ptr<fakefs::Handle> handle)
      : handle_(std::move(handle)) {}

  explicit operator bool() const { return handle_ && handle_->open; }
  bool isDirectory() const { return *this && handle_->directory; }
  size_t size() const { return *this ? handle_->buffer.size() : 0; }
  const char *name() const {
    return *this ? handle_->path.c_str() : "";
  }

  size_t write(const uint8_t *bytes, size_t length) {
    if (!*this || !handle_->writable || bytes == nullptr) return 0;
    const fakefs::FaultAction action =
        handle_->state->before(fakefs::Operation::write);
    if (action == fakefs::FaultAction::returnFailure) {
      handle_->writeError = 1;
      return 0;
    }
    size_t accepted = length;
    if (action == fakefs::FaultAction::shortWrite && length > 0) {
      accepted = length == 1 ? 0 : length / 2;
      handle_->writeError = 1;
    }
    if (handle_->position + accepted > handle_->buffer.size()) {
      handle_->buffer.resize(handle_->position + accepted);
    }
    if (accepted != 0) {
      std::memcpy(handle_->buffer.data() + handle_->position, bytes, accepted);
      handle_->position += accepted;
      handle_->dirty = true;
    }
    handle_->state->after(fakefs::Operation::write, action);
    return accepted;
  }

  int read(uint8_t *bytes, size_t length) {
    if (!*this || handle_->directory || bytes == nullptr) return -1;
    if (handle_->position >= handle_->buffer.size()) return 0;
    const size_t available = handle_->buffer.size() - handle_->position;
    const size_t received = std::min(length, available);
    std::memcpy(bytes, handle_->buffer.data() + handle_->position, received);
    handle_->position += received;
    return static_cast<int>(received);
  }

  bool seek(size_t position) {
    if (!*this || handle_->directory) return false;
    const fakefs::FaultAction action =
        handle_->state->before(fakefs::Operation::seek);
    if (action == fakefs::FaultAction::returnFailure ||
        action == fakefs::FaultAction::shortWrite) {
      return false;
    }
    handle_->position = position;
    handle_->state->after(fakefs::Operation::seek, action);
    return true;
  }

  void flush() {
    if (!*this || !handle_->writable) return;
    const fakefs::FaultAction action =
        handle_->state->before(fakefs::Operation::flush);
    if (action == fakefs::FaultAction::returnFailure ||
        action == fakefs::FaultAction::shortWrite) {
      handle_->writeError = 1;
      return;
    }
    if (handle_->dirty) {
      handle_->state->files[handle_->path] = handle_->buffer;
      handle_->state->addParents(handle_->path);
      handle_->dirty = false;
    }
    handle_->state->after(fakefs::Operation::flush, action);
  }

  int getWriteError() const {
    return *this ? handle_->writeError : 1;
  }

  void close() {
    if (!*this) return;
    const fakefs::FaultAction action =
        handle_->state->before(fakefs::Operation::close);
    if (action == fakefs::FaultAction::returnFailure ||
        action == fakefs::FaultAction::shortWrite) {
      handle_->writeError = 1;
      handle_->dirty = false;
      if (handle_->writable) handle_->state->files.erase(handle_->path);
      handle_->open = false;
      return;
    }
    if (handle_->writable && handle_->dirty) {
      handle_->state->files[handle_->path] = handle_->buffer;
      handle_->state->addParents(handle_->path);
      handle_->dirty = false;
    }
    handle_->open = false;
    handle_->state->after(fakefs::Operation::close, action);
  }

  File openNextFile() {
    if (!*this || !handle_->directory ||
        handle_->childIndex >= handle_->children.size()) {
      return {};
    }
    const std::string path = handle_->children[handle_->childIndex++];
    auto child = std::make_shared<fakefs::Handle>();
    child->state = handle_->state;
    child->path = path;
    child->open = true;
    child->directory = child->state->directories.count(path) != 0;
    if (!child->directory) child->buffer = child->state->files[path];
    return File(child);
  }

 private:
  std::shared_ptr<fakefs::Handle> handle_;
};

namespace fs {

class FS {
 public:
  FS() : state_(std::make_shared<fakefs::State>()) {}
  explicit FS(std::shared_ptr<fakefs::State> state)
      : state_(std::move(state)) {}

  std::shared_ptr<fakefs::State> state() const { return state_; }

  bool exists(const String &path) const { return exists(path.c_str()); }
  bool exists(const char *path) const {
    return path != nullptr && state_->has(path);
  }

  bool mkdir(const char *path) {
    if (path == nullptr || path[0] != '/') return false;
    state_->directories.insert(path);
    state_->addParents(path);
    return true;
  }

  bool mkdir(const String &path) { return mkdir(path.c_str()); }

  File open(const String &path, const char *mode = FILE_READ) {
    return open(path.c_str(), mode);
  }

  File open(const char *path, const char *mode = FILE_READ) {
    if (path == nullptr) return {};
    const fakefs::FaultAction action =
        state_->before(fakefs::Operation::open);
    if (action == fakefs::FaultAction::returnFailure ||
        action == fakefs::FaultAction::shortWrite) {
      return {};
    }
    const std::string key(path);
    const bool directory = state_->directories.count(key) != 0;
    const bool write = mode != nullptr &&
        (std::strchr(mode, 'w') != nullptr || std::strchr(mode, '+') != nullptr);
    if (!directory && !write && state_->files.count(key) == 0) return {};

    auto handle = std::make_shared<fakefs::Handle>();
    handle->state = state_;
    handle->path = key;
    handle->open = true;
    handle->directory = directory;
    handle->writable = write;
    if (directory) {
      const std::string prefix = key == "/" ? "/" : key + "/";
      std::set<std::string> children;
      for (const auto &entry : state_->directories) {
        if (entry.size() <= prefix.size() || entry.compare(0, prefix.size(), prefix) != 0) continue;
        const size_t slash = entry.find('/', prefix.size());
        children.insert(slash == std::string::npos
                            ? entry
                            : entry.substr(0, slash));
      }
      for (const auto &entry : state_->files) {
        if (entry.first.size() <= prefix.size() ||
            entry.first.compare(0, prefix.size(), prefix) != 0) continue;
        const size_t slash = entry.first.find('/', prefix.size());
        children.insert(slash == std::string::npos
                            ? entry.first
                            : entry.first.substr(0, slash));
      }
      handle->children.assign(children.begin(), children.end());
    } else if (write && mode != nullptr && std::strchr(mode, 'w') != nullptr) {
      handle->buffer.clear();
      handle->dirty = true;
    } else {
      const auto found = state_->files.find(key);
      if (found != state_->files.end()) handle->buffer = found->second;
    }
    state_->after(fakefs::Operation::open, action);
    return File(handle);
  }

  bool rename(const String &from, const String &to) {
    return rename(from.c_str(), to.c_str());
  }

  bool rename(const char *from, const char *to) {
    if (from == nullptr || to == nullptr) return false;
    const fakefs::FaultAction action =
        state_->before(fakefs::Operation::rename);
    if (action == fakefs::FaultAction::returnFailure ||
        action == fakefs::FaultAction::shortWrite) {
      return false;
    }
    const std::string source(from);
    const std::string target(to);
    bool changed = false;
    const auto file = state_->files.find(source);
    if (file != state_->files.end()) {
      state_->files[target] = file->second;
      state_->files.erase(file);
      state_->addParents(target);
      changed = true;
    } else if (state_->directories.count(source) != 0) {
      std::map<std::string, std::vector<uint8_t>> movedFiles;
      std::set<std::string> movedDirectories;
      const std::string prefix = source + "/";
      for (const auto &entry : state_->files) {
        if (entry.first == source || entry.first.compare(0, prefix.size(), prefix) == 0) {
          movedFiles[target + entry.first.substr(source.size())] = entry.second;
        }
      }
      for (const auto &entry : state_->directories) {
        if (entry == source || entry.compare(0, prefix.size(), prefix) == 0) {
          movedDirectories.insert(target + entry.substr(source.size()));
        }
      }
      for (auto it = state_->files.begin(); it != state_->files.end();) {
        if (it->first == source || it->first.compare(0, prefix.size(), prefix) == 0) it = state_->files.erase(it);
        else ++it;
      }
      for (auto it = state_->directories.begin(); it != state_->directories.end();) {
        if (*it == source || it->compare(0, prefix.size(), prefix) == 0) it = state_->directories.erase(it);
        else ++it;
      }
      state_->files.insert(movedFiles.begin(), movedFiles.end());
      state_->directories.insert(movedDirectories.begin(), movedDirectories.end());
      state_->addParents(target);
      changed = true;
    }
    state_->after(fakefs::Operation::rename, action);
    return changed;
  }

  bool remove(const String &path) { return remove(path.c_str()); }
  bool remove(const char *path) {
    if (path == nullptr) return false;
    const fakefs::FaultAction action =
        state_->before(fakefs::Operation::remove);
    if (action == fakefs::FaultAction::returnFailure ||
        action == fakefs::FaultAction::shortWrite) {
      return false;
    }
    const bool removed = state_->files.erase(path) != 0;
    state_->after(fakefs::Operation::remove, action);
    return removed;
  }

 private:
  std::shared_ptr<fakefs::State> state_;
};

}  // namespace fs
