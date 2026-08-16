#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <cstring>

#include "../PokePodAmoled/ProvisioningPolicy.h"

namespace {

struct DeterministicRandom {
  uint8_t next = 0;
  bool fail = false;
};

bool fillDeterministic(void *context, uint8_t *destination, size_t length) {
  auto *state = static_cast<DeterministicRandom *>(context);
  if (state == nullptr || destination == nullptr || state->fail) return false;
  for (size_t index = 0; index < length; ++index) {
    destination[index] = state->next++;
  }
  return true;
}

}  // namespace

int main() {
  using namespace pokepod;

  static_assert(kProvisioningPasswordLength >= 8U &&
                kProvisioningPasswordLength <= 12U);
  static_assert(kProvisioningPasswordAlphabetLength == 32U);

  DeterministicRandom firstSource{0, false};
  DeterministicRandom secondSource{17, false};
  char first[kProvisioningPasswordLength + 1] = {};
  char second[kProvisioningPasswordLength + 1] = {};
  assert(generateProvisioningPassword(first, sizeof(first), fillDeterministic,
                                      &firstSource));
  assert(generateProvisioningPassword(second, sizeof(second),
                                      fillDeterministic, &secondSource));
  assert(std::strlen(first) == kProvisioningPasswordLength);
  assert(std::strlen(second) == kProvisioningPasswordLength);
  assert(std::strcmp(first, second) != 0);
  for (const char character : first) {
    if (character == '\0') break;
    assert(std::strchr(kProvisioningPasswordAlphabet, character) != nullptr);
    assert(character != '0' && character != '1' && character != 'I' &&
           character != 'O');
  }

  char tooSmall[kProvisioningPasswordLength] = {'x'};
  DeterministicRandom smallSource{0, false};
  assert(!generateProvisioningPassword(tooSmall, sizeof(tooSmall),
                                       fillDeterministic, &smallSource));
  for (const char character : tooSmall) assert(character == '\0');

  char failed[kProvisioningPasswordLength + 1] = {'x'};
  DeterministicRandom failedSource{0, true};
  assert(!generateProvisioningPassword(failed, sizeof(failed),
                                       fillDeterministic, &failedSource));
  for (const char character : failed) assert(character == '\0');
  assert(!generateProvisioningPassword(nullptr, sizeof(failed),
                                       fillDeterministic, &firstSource));

  ProvisioningCredentialPolicy session;
  DeterministicRandom sessionOne{3, false};
  DeterministicRandom sessionTwo{29, false};
  assert(session.begin(fillDeterministic, &sessionOne));
  assert(session.active());
  char firstSession[kProvisioningPasswordLength + 1] = {};
  std::strcpy(firstSession, session.password());
  session.close();
  assert(!session.active());
  assert(session.password()[0] == '\0');
  assert(session.begin(fillDeterministic, &sessionTwo));
  assert(std::strcmp(firstSession, session.password()) != 0);
  session.close();

  ProvisioningCredentialPolicy fixedSession;
  assert(fixedSession.begin(ProvisioningPasswordMode::fixed88888888,
                            fillDeterministic, &sessionOne));
  assert(std::strcmp(fixedSession.password(), kFixedProvisioningPassword) ==
         0);
  fixedSession.close();
  assert(!fixedSession.active());
  assert(fixedSession.password()[0] == '\0');

  // Fixed mode does not require entropy, while random mode fails closed when
  // its entropy source cannot fill a batch.
  DeterministicRandom failedFixedSource{0, true};
  assert(fixedSession.begin(ProvisioningPasswordMode::fixed88888888,
                            fillDeterministic, &failedFixedSource));
  fixedSession.close();
  assert(!fixedSession.begin(static_cast<ProvisioningPasswordMode>(3),
                             fillDeterministic, &sessionOne));
  assert(!fixedSession.active());

  assert(kFixedProvisioningPasswordLength == 8U);
  assert(kFixedProvisioningPassword[0] == '8');
  assert(kFixedProvisioningPassword[7] == '8');

  assert(std::strcmp(provisioningStateName(ProvisioningState::ready),
                     "ready") == 0);
  assert(std::strcmp(provisioningStateName(ProvisioningState::scanning),
                     "scanning") == 0);
  assert(std::strcmp(provisioningStateName(ProvisioningState::connecting),
                     "connecting") == 0);
  assert(std::strcmp(provisioningStateName(ProvisioningState::connected),
                     "connected") == 0);
  assert(std::strcmp(provisioningStateName(ProvisioningState::error),
                     "error") == 0);
  return 0;
}
