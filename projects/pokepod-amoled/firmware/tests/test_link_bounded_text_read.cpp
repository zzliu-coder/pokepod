#include <assert.h>

#include "../PokePodAmoled/LinkBoundedTextRead.h"

using namespace pokepod;

int main() {
  LinkBoundedTextRead reader;
  assert(reader.begin(8192));
  unsigned polls = 0;
  while (reader.active()) {
    const size_t wanted = reader.nextReadBytes();
    assert(wanted > 0 && wanted <= 1024);
    assert(reader.acceptRead(wanted));
    ++polls;
  }
  assert(reader.ready());
  assert(reader.bytesRead() == 8192);
  assert(polls == 8);

  reader.reset();
  assert(reader.begin(1025));
  assert(reader.acceptRead(1024));
  assert(reader.active());
  reader.cancel();
  assert(reader.failed());
  assert(reader.bytesRead() == 1024);
  reader.reset();
  assert(!reader.begin(8193));
  assert(reader.failed());
  return 0;
}
