#include <assert.h>

#include "../PokePodAmoled/StorageCoordinator.cpp"
#include "../PokePodAmoled/LinkTreeStepper.cpp"

using namespace pokepod;

namespace {

LinkTreeStepper::Result drain(LinkTreeStepper &stepper, size_t limit = 256) {
  LinkTreeStepper::Result result = LinkTreeStepper::Result::progress;
  for (size_t poll = 0; poll < limit; ++poll) {
    result = stepper.poll();
    if (result != LinkTreeStepper::Result::progress &&
        result != LinkTreeStepper::Result::wouldBlock) return result;
  }
  return result;
}

}  // namespace

int main() {
  fs::FS fs;
  std::string source(9000, 'a');
  source[8192] = 'z';
  fs.state()->seed("/source.bin", source);
  LinkTreeStepper copy;
  assert(copy.beginCopy(fs, "/source.bin", "/target.bin",
                        StorageOwner::wifiLink));
  assert(drain(copy) == LinkTreeStepper::Result::complete);
  assert(fs.state()->text("/target.bin") == source);
  assert(fs.state()->maximumReadBytes <= LinkTreeStepper::kBytesPerPoll);
  assert(fs.state()->maximumWriteBytes <= LinkTreeStepper::kBytesPerPoll);
  assert(fs.state()->openHandles == 0);

  // A successful write/flush is not publication authority. Corrupting the
  // persisted destination before its independent re-read must fail the copy.
  fs::FS corruptFs;
  corruptFs.state()->seed("/source.bin", source);
  LinkTreeStepper corrupt;
  assert(corrupt.beginCopy(corruptFs, "/source.bin", "/target.bin",
                           StorageOwner::wifiLink));
  bool corrupted = false;
  LinkTreeStepper::Result result = LinkTreeStepper::Result::progress;
  for (size_t poll = 0; poll < 256; ++poll) {
    result = corrupt.poll();
    auto found = corruptFs.state()->files.find("/target.bin");
    if (!corrupted && found != corruptFs.state()->files.end() &&
        !found->second.empty()) {
      found->second[0] ^= 0x55;
      corrupted = true;
    }
    if (result != LinkTreeStepper::Result::progress &&
        result != LinkTreeStepper::Result::wouldBlock) break;
  }
  assert(corrupted);
  assert(result == LinkTreeStepper::Result::failed);
  assert(corruptFs.state()->openHandles == 0);

  // Folder finalization is read-only and fail-closed. Any unknown/future file
  // or nested directory must block deletion rather than being swept into the
  // staging tree and erased.
  fs::FS verifyFs;
  assert(verifyFs.mkdir("/empty"));
  LinkTreeStepper verify;
  assert(verify.beginVerifyAbsent(verifyFs, "/empty", "*",
                                  StorageOwner::wifiLink));
  assert(drain(verify) == LinkTreeStepper::Result::complete);

  verifyFs.state()->seed("/folder/nested/note.txt", "keep");
  verifyFs.state()->seed("/folder/capsule/capsule.json", "{}");
  LinkTreeStepper blocked;
  assert(blocked.beginVerifyAbsent(verifyFs, "/folder", "*",
                                   StorageOwner::wifiLink));
  assert(drain(blocked) == LinkTreeStepper::Result::failed);
  assert(verifyFs.state()->has("/folder/capsule/capsule.json"));
  return 0;
}
