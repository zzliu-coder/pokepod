# Link operation production migration matrix

Every Link v2 request is admitted once and settled once. `queueFrame` only
queues bytes with an explicit role. Durable completion, request release and
coordinator release happen in the single settlement adapter after every
terminal frame has drained and every owned resource has reached a terminal
state.

| Operation kind | Admit point | Progress / data / terminal frames | Durable terminal fact | Resources acquired / released | Cancel and rollback | Completed eligibility | Maintenance retain |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Immediate read/configure/reboot | decoded request, before dispatch | none / none / response JSON | response value determined; configuration writes committed before response | request + coordinator / terminal drain | cancel suppresses response; committed configuration remains authoritative | terminal response only; busy is retryable | retain while `activeMaintenance` is set |
| Incoming staged file or system font | accepted control request, before opening `.part` | `binary_ack` / incoming binary frames are receive-side / accepted or error JSON | prepared file transaction committed and deferred `.part` cleanup terminal | request + coordinator + storage reservation + file + transaction / owner-scoped cleanup | disconnect/deadline closes transport, rolls transaction back and removes orphan `.part` in background | final accepted/error after cleanup | retain while maintenance is active |
| Incoming command: batch/text/simple | accepted upload request; command execution remains same request | `binary_ack` and command progress / receive-side data / durable accepted or error JSON | command result JSON durable; mutation journal terminal; incoming command cleanup terminal | request + coordinator + storage/file during upload + transaction/journal / cleanup terminal | cancel stops foreground apply, then durable rollback/recovery and cleanup continue without response | only durable result + cleanup terminal | begin retains; end releases only after result fetch semantics |
| Outgoing file/read/result | decoded read request, before file reservation | response metadata / file chunks / final data frame | final data drained and file/result-fetch cleanup terminal | request + coordinator + storage reservation + file / deferred close terminal | cancel discards queued data, closes file under lease, never confirms result fetch | final data drain only | retain while maintenance is active |
| Recursive/shallow manifest | decoded fingerprint request, before scan reservation | none / none / manifest response JSON | scan/hash complete and directory/file handles closed | request + coordinator + storage reservation + files / deferred close terminal | cancel aborts stepper and closes every cursor in background | manifest response after cleanup | retain while maintenance is active |
| Record start | decoded start request, before router acquisition | optional status event / none / started or failed JSON | recorder storage task start ACK and capture task start fact | request + coordinator + router + transaction / start terminal transfers router to recording session | deadline/disconnect cancels pending start and waits storage/capture cleanup | real start ACK terminal only | no maintenance-specific change |
| Record stop | decoded stop request, while recording-session router is owned | optional status event / none / queued or failed JSON | capture stopped/drained and recorder durable `RecorderOutcome` consumed | request + coordinator + router + transaction / recorder terminal then router release | cancel suppresses response; Link owner continues bounded finalize/rollback | successful/failed durable recorder terminal | retain while maintenance is active |
| Maintenance begin/end | decoded command under durable command executor | command progress / none / accepted response JSON | command result JSON durable; end completion observed only after result fetch | request + coordinator + transaction / begin retains coordinator, end releases after terminal rules | cancel/reboot replays durable intent idempotently | durable result only | begin=true; ordinary request=true; confirmed end=false |

## Static production rules

- A transport connection generation is non-zero and immutable for the life of
  one CDC/TLS connection.
- The already-armed transfer deadline is read into `LinkOperation`; admission
  never rearms or extends it.
- Progress and data frames never make a request completed and never release an
  owner.
- A terminal response may settle only after all queued progress/data/terminal
  frames drain and storage, file, router and transaction resources are clear.
- Disconnect, deadline and quiesce all enter cancellation first. Transport
  bytes are discarded immediately; rollback and cleanup continue without a
  response.
- An idle retained maintenance coordinator is cancelled explicitly on
  disconnect/quiesce before the service reports `quiesced`.
