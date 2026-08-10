#include <cassert>

#include "NetworkTimeSyncState.h"

using namespace pokepod;

int main() {
  NetworkTimeSyncState state;
  assert(state.connectionGeneration() == 0);
  assert(state.revision() == 0);

  state.noteSynchronized();
  assert(state.revision() == 0);

  state.noteConnected(true);
  assert(state.connectionGeneration() == 1);
  state.noteConnected(true);
  assert(state.connectionGeneration() == 1);
  state.noteSynchronized();
  assert(state.revision() == 1);
  state.noteSynchronized();
  assert(state.revision() == 1);

  state.noteConnected(false);
  state.noteSynchronized();
  assert(state.revision() == 1);
  state.noteConnected(true);
  assert(state.connectionGeneration() == 2);
  state.noteSynchronized();
  assert(state.revision() == 2);
  return 0;
}
