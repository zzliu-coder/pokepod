#include <assert.h>

#include "../PokePodAmoled/UiSignatureCore.h"

using pokepod::UiSignatureCore;

int main() {
  UiSignatureCore first;
  first.addText("ab", 2);
  first.add(12U);

  UiSignatureCore same;
  same.addText("ab", 2);
  same.add(12U);
  assert(first.value() == same.value());

  UiSignatureCore differentTextBoundary;
  differentTextBoundary.addText("a", 1);
  differentTextBoundary.addText("b", 1);
  assert(first.value() != differentTextBoundary.value());

  UiSignatureCore differentNumber;
  differentNumber.addText("ab", 2);
  differentNumber.add(13U);
  assert(first.value() != differentNumber.value());
  return 0;
}
