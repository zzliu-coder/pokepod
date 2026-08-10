#include <assert.h>

#include "../PokePodAmoled/PerformanceMetric.h"

using pokepod::PerformanceMetric;

int main() {
  PerformanceMetric metric;
  metric.record(1000, 2000);
  metric.record(3000, 2000);
  assert(metric.lastUs() == 3000);
  assert(metric.maxUs() == 3000);
  assert(metric.count() == 2);
  assert(metric.overBudgetCount() == 1);
  return 0;
}
