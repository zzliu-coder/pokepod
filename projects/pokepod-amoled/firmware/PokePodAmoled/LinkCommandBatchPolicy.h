#pragma once

#include <stddef.h>

namespace pokepod {

struct LinkBatchRollbackResult {
  size_t attempted = 0;
  size_t failed = 0;

  bool ok() const { return failed == 0; }
};

// Mutating Link commands keep a journal in execution order. Rollback always
// walks the applied prefix in reverse so nested moves cannot orphan a capsule.
template <typename Rollback>
LinkBatchRollbackResult rollbackLinkBatch(size_t applied, Rollback rollback) {
  LinkBatchRollbackResult result;
  for (size_t index = applied; index > 0; --index) {
    ++result.attempted;
    if (!rollback(index - 1)) ++result.failed;
  }
  return result;
}

}  // namespace pokepod
