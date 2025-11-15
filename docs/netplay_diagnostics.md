# Understanding GekkoNet host diagnostics

RetroArch's `netplay_host_diag_dump()` helper prints the `[GekkoNet][Diag]` block
whenever you start hosting or joining a session. The log is verbose by design: it
shows each stage of the libGekkoNet bring-up so you can tell whether the
front-end encountered an error before it begins waiting for the remote peer.

## What a "successful" block looks like

If every line in the "Stage …" section is `success` (or `ready` for the callback
lines), RetroArch has already finished initializing the session. At that point
it has:

- Allocated the netplay state buffer
- Hooked the core/netplay callbacks
- Bound a UDP socket to the resolved port
- Started the libGekkoNet session thread
- Registered the local player handle

After this block is printed, the host simply waits for traffic from the client –
no extra log spam means the server is idle, not that initialization failed. The
actual log strings live in `network/netplay/netplay_frontend.c` inside
`netplay_host_diag_dump()`, so you can match each field to the code path that
sets it.【F:network/netplay/netplay_frontend.c†L1180-L1264】

## Requested vs. resolved ports

The "Requested" and "Resolved" port lines tell you whether RetroArch needed to
fall back to a different UDP port. When the requested port is busy, the frontend
probes up to 16 consecutive ports, picks the first verified free one, logs the
result, and persists it back into your configuration so future sessions reuse
that fallback automatically.【F:network/netplay/netplay_frontend.c†L1197-L1234】

Because the resolved port is the one actually bound, make sure you forward or
share that port with your clients. If the host's router only forwards the
original request (e.g., 23456) but the resolved port is 23457, clients will
connect to the wrong port and sit waiting forever.

## Why the block appears twice

The helper writes the same information to `diagnosis.text` by calling
`netplay_host_diag_write_file()`, and it logs "Diagnostics written to …" after
that succeeds.【F:network/netplay/netplay_frontend.c†L1264-L1298】 Seeing the
"written" line both before and after the block is normal when verbose logging is
enabled. It does not imply that initialization restarted or failed in between.

## When you really have a problem

Investigate further only when:

- Any stage line reads `failed`/`not reached`
- `Failure stage` or `Failure reason` is populated
- `libGekkoNet reported` contains an error string

In those cases, the diagnostics point to the exact step that broke (adapter
setup, state allocation, etc.), which gives you a concrete starting point for
troubleshooting instead of guessing from the high-level UI.
