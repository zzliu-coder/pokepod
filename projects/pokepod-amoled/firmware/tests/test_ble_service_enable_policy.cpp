#include <cassert>

#include "BleServiceEnablePolicy.h"

using namespace pokepod;

int main() {
  BleServiceEnablePolicy policy;

  // A persisted-off boot never asks the runtime to advertise.
  policy.begin(false);
  assert(!policy.userEnabled());
  assert(!policy.acceptsNewWork());
  assert(policy.phase() == BleServiceEnablePhase::disabled);
  assert(!policy.poll(false, false, 0).startAdvertising);

  // Enabling is idempotent and starts advertising only while disconnected.
  auto actions = policy.requestEnable(false);
  assert(actions.startAdvertising);
  assert(policy.acceptsNewWork());
  actions = policy.requestEnable(false);
  assert(!actions.startAdvertising);

  // An idle connected disable stops advertising and requests a disconnect.
  actions = policy.requestDisable(false, true, 100);
  assert(actions.stopAdvertising);
  assert(actions.disconnect);
  assert(!actions.clearRuntime);
  assert(policy.disablePending());
  assert(!policy.acceptsNewWork());

  // A missing callback is retried on a bounded cadence without pretending
  // that the transport is already disconnected.
  actions = policy.poll(false, true, 349);
  assert(!actions.disconnect);
  actions = policy.poll(false, true, 350);
  assert(actions.disconnect);
  assert(policy.phase() == BleServiceEnablePhase::disconnecting);
  actions = policy.poll(false, false, 351);
  assert(actions.clearRuntime);
  assert(!policy.disablePending());
  assert(policy.phase() == BleServiceEnablePhase::disabled);

  // A streaming disable first ends the session. It cannot disconnect or
  // clear runtime state until the session reaches a terminal state.
  policy.requestEnable(true);
  actions = policy.requestDisable(true, true, 1000);
  assert(actions.requestSessionStop);
  assert(!actions.disconnect);
  assert(!actions.clearRuntime);
  assert(policy.phase() == BleServiceEnablePhase::stoppingSession);
  actions = policy.poll(true, true, 1100);
  assert(actions.requestSessionStop);
  assert(!actions.disconnect);
  actions = policy.poll(false, true, 1200);
  assert(actions.disconnect);
  actions = policy.poll(false, false, 1201);
  assert(actions.clearRuntime);

  // A second enable changes the persisted intent immediately but cannot
  // admit new work until the old session and connection are terminal.
  policy.requestEnable(true);
  policy.requestDisable(true, true, 2000);
  actions = policy.requestEnable(true);
  assert(policy.userEnabled());
  assert(!policy.acceptsNewWork());
  assert(policy.transitionPending());
  assert(!actions.startAdvertising);
  actions = policy.poll(true, true, 2100);
  assert(actions.requestSessionStop);
  actions = policy.poll(false, true, 2200);
  assert(actions.disconnect);
  actions = policy.poll(false, false, 2201);
  assert(actions.clearRuntime);
  assert(actions.startAdvertising);
  assert(policy.acceptsNewWork());
  assert(!policy.transitionPending());

  return 0;
}
