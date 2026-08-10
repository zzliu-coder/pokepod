#include "TlsExternalMemory.h"

#include <esp_heap_caps.h>
#include <mbedtls/platform.h>

namespace pokepod {
namespace {

void *tlsPsramCalloc(size_t count, size_t size) {
  return heap_caps_calloc(count, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void tlsPsramFree(void *pointer) {
  heap_caps_free(pointer);
}

}  // namespace

bool beginTlsExternalMemory(Print &log) {
  if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == 0) {
    log.println("{\"event\":\"tls_memory\",\"ok\":false,\"reason\":\"psram_missing\"}");
    return false;
  }
  const int result = mbedtls_platform_set_calloc_free(
      tlsPsramCalloc, tlsPsramFree);
  log.printf(
      "{\"event\":\"tls_memory\",\"ok\":%s,\"allocator\":\"psram\",\"result\":%d}\n",
      result == 0 ? "true" : "false", result);
  return result == 0;
}

}  // namespace pokepod
