# D-Bus API contract

Proposed identity:
- Bus name: `io.github.multiwifi.Daemon`
- Object path: `/io/github/multiwifi/Daemon`
- Interface: `io.github.multiwifi.Daemon1`

Keep the API versioned and introspectable. Read-only methods follow system policy; mutations require polkit. Long operations return operation IDs and emit progress signals.

## Methods
- `GetVersion() -> (s version, u api_version)`
- `GetCapabilities() -> (a{sv} capabilities)`
- `GetDevices() -> (aa{sv} devices)`
- `GetPaths() -> (aa{sv} paths)`
- `GetStatus() -> (a{sv} status)`
- `StartLoadBalancing(a{sv} options) -> (s operation_id)`
- `StopLoadBalancing() -> (s operation_id)`
- `StartBonding(a{sv} profile) -> (s operation_id)`
- `StopBonding() -> (s operation_id)`
- `SetPathWeight(s device_id, u weight)`
- `RunDiagnostics() -> (s operation_id)`
- `GetDiagnostics(s operation_id) -> (a{sv} result)`

Device dictionaries may contain ifname, ifindex, type, state, connection UUID, addresses, signal quality and managed flag. Path dictionaries contain gateway, health, RTT, loss, last probe and eligibility. Capability results must distinguish unsupported from inactive.

## Signals
`DeviceChanged(a{sv})`, `PathChanged(a{sv})`, `StatusChanged(a{sv})`, `OperationProgress(s operation_id, u percent, s message)`, `OperationFinished(s operation_id, b success, a{sv} result)`, `Warning(s code, s message)`.

## Stable errors
`NotAuthorized`, `InvalidArgument`, `Unsupported`, `Conflict`, `CapabilityMissing`, `NetworkFailure`, `RollbackFailed`, `Busy`, under `io.github.multiwifi.Error.*`. Do not expose secrets.

## Operation lifecycle
`QUEUED -> VALIDATING -> APPLYING -> VERIFYING -> SUCCEEDED`. Failure: `... -> ROLLING_BACK -> FAILED`. If rollback is incomplete, preserve journal and report recovery warning.
