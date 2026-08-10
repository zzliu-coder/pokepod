#include <assert.h>

#include "../PokePodAmoled/DeferredPublish.h"

using pokepod::DeferredPublish;

int main() {
  DeferredPublish publish;
  assert(publish.request());

  publish.begin();
  assert(publish.active());
  assert(!publish.request());
  assert(!publish.request());
  assert(publish.finish());
  assert(!publish.active());

  publish.begin();
  assert(!publish.finish());
  assert(publish.request());
  return 0;
}
