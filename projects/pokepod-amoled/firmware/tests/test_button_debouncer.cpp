#include <cassert>
#include <cstdint>

#include "../PokePodAmoled/ButtonDebouncer.h"

int main() {
  pokepod::ButtonDebouncer button(25);
  assert(!button.update(false, 0));
  assert(!button.update(true, 5));
  assert(!button.update(true, 20));
  assert(button.update(true, 30));
  assert(button.pressedEdge());
  assert(!button.update(true, 40));
  assert(!button.update(false, 45));
  assert(button.update(false, 75));
  assert(!button.pressedEdge());
  assert(button.releasedEdge());

  pokepod::ButtonDebouncer rolloverButton(10);
  assert(!rolloverButton.update(true, UINT32_MAX - 3));
  assert(rolloverButton.update(true, 7));
  assert(rolloverButton.pressedEdge());
  return 0;
}
