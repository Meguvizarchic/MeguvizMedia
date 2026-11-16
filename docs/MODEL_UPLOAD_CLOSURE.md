# Failed model upload and deferred engine close

`MediaNative.frameClose(handle)` requests close. It may return while GPU upload
ownership or another retained consumer prevents native destruction. The native
engine is marked closing, rejects newly admitted model/process operations and
stays in the bounded registry until shutdown can finish. An operation admitted
before closing may still finish. Closing does not forcibly reclaim another
AndroidMediaSession or a consumer-owned output.

Worker clients needing an explicit result can call `frameTryClose`: zero means
the engine handle was removed, BUSY retains it for retry. The result is not a
device reset or an interruption of a stuck driver. Some origin-resource cleanup
may still block, so this is a worker API without a hard wall-clock guarantee.

Failed `frameLoad` and failed nonidentity AndroidMediaSession construction can
also leave abandoned upload owners without a live interpolation session. They
request the same service as deferred close. A service success reclaims failed
upload state; it does not turn the failed load into a successful model. Loading
must be explicitly retried.

The Java facade lazily creates one daemon service with at most one queued retry.
Requests arriving during a native service call mark another pass necessary.
BUSY retries back off to one second; completion shuts down the executor. There
is no static native loading or automatic model retry. Scheduling/loader errors
leave native ownership intact and expose a nonzero last-service status.

`frameServiceUploads(0, 0)` explicitly services this module's abandoned uploads
and pending native closes. A nonzero handle services that engine. These entries
remain available for manual worker service when automatic scheduling cannot
start. The last Java service status describes that service observation, not
complete graphics retirement, actual presentation or a release qualification.

The matching library and API must stay loaded through successful close and API
return. Dropping a Java backend handle is permitted because the native registry
retains a pending close; it is not permission to unload its module. The official
facade performs this handoff for existing BIL close callers.

The current service covers model-upload failures. Accepted inference retirement,
GL/EGL recovery, JNI result delivery under allocation failure and external
producer lifetimes keep their own gates. Their completion cannot be inferred
from an empty upload registry.
