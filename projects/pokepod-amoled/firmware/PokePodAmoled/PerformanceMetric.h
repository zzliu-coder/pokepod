#pragma once

#include <stdint.h>

namespace pokepod {

class PerformanceMetric {
 public:
  void record(uint32_t durationUs, uint32_t budgetUs) {
    lastUs_ = durationUs;
    if (durationUs > maxUs_) maxUs_ = durationUs;
    ++count_;
    if (budgetUs > 0 && durationUs > budgetUs) ++overBudgetCount_;
  }

  uint32_t lastUs() const { return lastUs_; }
  uint32_t maxUs() const { return maxUs_; }
  uint32_t count() const { return count_; }
  uint32_t overBudgetCount() const { return overBudgetCount_; }

 private:
  uint32_t lastUs_ = 0;
  uint32_t maxUs_ = 0;
  uint32_t count_ = 0;
  uint32_t overBudgetCount_ = 0;
};

}  // namespace pokepod
