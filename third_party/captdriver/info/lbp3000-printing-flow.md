# Canon LBP3000 — Print Flow Reference (Windows XP Observed, Linux Implementation Guide)

> Condensed implementation reference derived from `lbp3000-windowsxp.txt` (4-page A4 job),
> `lbp3000-windowsxp-hardtocompress2.txt` (3-page A4 job, mixed large pages), and
> `lbp3000-windowsxp-hardtocompress4.txt` (3-page A4 job, **every page streaming** — all
> pages too large to fit the 2 MB buffer).
> Windows XP driver is a persistent daemon. **Linux captdriver is one-shot**: starts, inits,
> prints all pages, de-inits printer, waits for idle, exits. This document uses Windows XP
> observed sequencing as ground truth for protocol correctness.

---

## Fundamental Printing Model: Two-Page Pipeline

The printer operates a **two-page pipeline** to maintain continuous throughput:

- Page N is physically printing (engine consuming raster data from buffer).
- Page N+1 data is being uploaded to the printer buffer **at the same time**.

This overlap is **always expected** and is the normal operating mode. If page N+1 data is
not uploaded in time before the engine finishes page N, the engine stalls and waits, costing
~10 seconds per stall. The driver must stay ahead of the engine by pre-uploading the next
page's data as soon as the current page's `StartPrint` is issued.

**The pipelining means:**
- `D0A9` for page N+1 can (and should) be sent as soon as `GetExtendedStatus.Start == N`
  (decoder accepted page N). Do NOT wait for `StartPrint(N)` to confirm.
- `IC_VIDEO_DATA` for page N+1 begins immediately after page N's `IC_BLACK_END`.
- The buffer holds up to 2 MB; with pages typically 250 KB–500 KB, there is room for one full
  page ahead. For pages > 2 MB, see §4 (large-page streaming).

---

## 1. Startup / Idle Polling (before any job)

```
GetPrinterInfo          → store Blk=65520, Buf=64; call once, reuse for session
GetExtendedStatus       → check Bas, Bas1 flags
  if Bas1 & 0x02:       → call GetInputStatus (paper/tray state change)
GetExtendedStatus       → repeat until Bas1 & 0x02 clears (stable)
  [loop GetExtendedStatus + GetInputStatus until printer idle]
```

**Two possible startup states observed:**

| Capture | Bas on startup | Meaning | GoOffline needed? |
|---------|---------------|---------|-------------------|
| `lbp3000-windowsxp.txt` | `0x31` | RCF_PRINTER_FREE + RCF_OFFLINE | No — already offline |
| `lbp3000-windowsxp-hardtocompress2.txt` | `0x01` | RCF_PRINTER_FREE, online | Yes — must GoOffline |

**Key flags to check:**
- `Bas & 0x10` — printer offline; if already set, skip `GoOffline` during init
- `Bas1 & 0x02` — `needGetInputStatus`, call `GetInputStatus` immediately when set
- `Bas1 & 0x01` — extended status changed, re-call `GetExtendedStatus`

---

## 2. Job Initialization

```
ReserveUnit(00 00 00 00 00 00 00 00)  → JobID (uint16 LE, e.g. 01 00)
SetJobInfo2(flag=1, JobID, hostname, username, jobname)
GetBasicStatus          → check if offline
GetExtendedStatus
GetInputStatus

[conditional GoOffline — only if printer is currently ONLINE]
  if (Bas & 0x10) == 0:           ← printer online, must go offline first
    GoOffline(00 00)
    GetBasicStatus                  ← poll until byte1 = 0x10 (offline confirmed)
    GetExtendedStatus               ← confirm offline
GetInputStatus

[clear sequence — always execute]
ClearMisPrint
ClearError
DiscardData
GetBasicStatus
GoOnline(ee db ea ad 00 00 00 00 00 00 00 00 00 00 00 00)  ← 16 bytes (Windows)
GetBasicStatus          → confirm byte1 = 0x00 (online)
GetExtendedStatus       → confirm Start=0, Printing=0, Shipped=0, Printed=0
                          NOTE: GoOnline ALWAYS resets all four page counters to 0
GetBasicStatus
```

**If `Bas & 0x10` is set at init:** skip `GoOffline`, execute clear sequence directly.
Observed in `lbp3000-windowsxp.txt` (printer `Bas=0x30`) and `hardtocompress4` (`Bas=0x30`).

**If `Bas & 0x10` is CLEAR at init (printer online):** must `GoOffline` first.
Observed in `lbp3000-windowsxp-hardtocompress2.txt`: printer `Bas=0x00` → `GoOffline` → poll
`GetBasicStatus` until `byte1=0x10` → `GetExtendedStatus` → `GetInputStatus` → then clear sequence.

---

## 3. Normal Page Loop (pipeline operation)

The pipeline loop runs continuously for all pages. While sending page N+1 data,
the engine is printing page N.

### 3.1 Announce page — D0A9 multi-command

Send as a single USB transfer:

```
D0A9 container {
  D0A0  IC_BEGIN_PAGE   (40 bytes for LBP3000)
  D0A4  IC_BLACK_PLANE  (8 bytes: 01 04 01 01 00 f9 80 00)
  D0A1  IC_BEGIN_DATA   (no payload)
  D0A2  IC_END_PAGE     (no payload)
}
```

**D0A9 send conditions:**
- Page 1: send immediately after `GoOnline` confirmation + final `GetBasicStatus`.
- Page N+1: send as soon as `GetExtendedStatus.Start == N` (printer decoder accepted page N).
  Do NOT wait for `StartPrint(N)` to confirm before sending `D0A9(N+1)`.
  `D0A9(N+1)` can be sent while page N's `IC_VIDEO_DATA` is still being uploaded.
  However, `IC_VIDEO_DATA` for page N+1 must NOT start until page N's `IC_BLACK_END` is sent.

### 3.2 Send raster data — IC_VIDEO_DATA

```
loop:
  send IC_VIDEO_DATA chunk (up to Blk=65520 bytes payload)
  every ~5 chunks: GetBasicStatus
    if byte1 changed from last value: GetExtendedStatus
      if Bas1 & 0x02: GetInputStatus
      if Start == N+1: can send D0A9(N+2) now (if not already sent)
    BufLevel = GetBasicStatus bytes5-6 & 0x0F:
      0x0f (15): burst 4–5 chunks
      0x08-0x0e: reduce burst size
      0x01-0x07: 1 chunk per poll cycle
      0x00 (with chunks remaining): STREAMING MODE — see §4.3
      0x00 (all chunks sent): stop; wait for BufLevel ≥ 1 (post-IC_BLACK_END drain)
  until all page N data sent
```

**`IM_DATA_BUSY` (byte1 bit 0x08) was NEVER set on LBP3000.** Use BufLevel only.

### 3.3 Command semantics: StartPrint vs IC_BLACK_END

These two commands are **independent** and serve different purposes:

- **`StartPrint(N)`** — tells the engine to begin physically printing page N. Condition:
  `GetExtendedStatus.Start == N` confirmed (printer decoder has accepted page N's D0A9) AND
  one of: (a) all raster data sent + `IC_BLACK_END` sent (normal mode), OR (b) `BufLevel==0`
  with data remaining (streaming mode — `IC_BLACK_END` comes later).

- **`IC_BLACK_END`** — marks that ALL raster data for page N is now uploaded. Acts as a
  stream separator: any `IC_VIDEO_DATA` after this belongs to page N+1. Send after the very
  last `IC_VIDEO_DATA` chunk for page N.

**Critical distinction:**
- **Normal mode**: `BufLevel` never hits 0 while data remains → last chunk → `IC_BLACK_END` → `StartPrint(N)`
- **Streaming mode**: `BufLevel` hits 0 while data remains → `StartPrint(N)` at BufLevel=0 → poll BufLevel≥1 → remaining chunks → `IC_BLACK_END`

**`BufLevel=0` during post-`StartPrint` polling is NOT streaming mode.** If all data is sent and
`BufLevel=0` only appears *after* the last chunk and *after* `IC_BLACK_END`, that is normal drain —
the engine is consuming. This is NOT a trigger for streaming. Streaming is triggered only when
`BufLevel=0` appears while there are still chunks to send before `IC_BLACK_END`.

**`StartPrint(N)` for page 1:**
Fire immediately after `IC_BLACK_END` if `Start == 1` is already confirmed (which it will be
if `GetExtendedStatus` showed `Start=1` at any point during or after data upload). No extra polling needed.

**`StartPrint(N)` for N ≥ 2 (normal mode):**
```
after IC_BLACK_END:
  poll GetBasicStatus until byte1 changes → GetExtendedStatus
  if Start == N: fire StartPrint(N)   ← may need 1–14 polls
```

**Observed per page — `lbp3000-windowsxp.txt` (all pages fit buffer):**

| Page | Start==N first seen | StartPrint fires |
|------|---------------------|-----------------|
| 1 | Mid-stream at 326 KB of 452 KB | After IC_BLACK_END (Start=1 already confirmed) |
| 2 | After IC_BLACK_END → poll ~14× | After IC_BLACK_END + polling |
| 3 | After IC_BLACK_END → poll 1× | After IC_BLACK_END + 1 poll |
| 4 | Mid-stream at 326 KB of 370 KB | After IC_BLACK_END (Start=4 already confirmed) |

---

## 4. Buffer Cases and Streaming

### 4.1 Normal case — all data fits the buffer (BufLevel never hits 0 during send)

```
[Page N]
D0A9{D0A0,D0A4,D0A1,D0A2}
loop: send IC_VIDEO_DATA chunks; poll GetBasicStatus every ~5 chunks
      track Start — note when Start == N is first confirmed
      BufLevel drops but never reaches 0 before last chunk
IC_BLACK_END            ← after last chunk (BufLevel ≥ 1)
[if Start == N already confirmed: StartPrint(N) immediately]
[else: poll until Start == N, then StartPrint(N)]
```

**Observed in hardtocompress2 page 1 (~1.9 MB):**
BufLevel drops 0x0f → 0x0c → 0x07 → 0x02, last chunk sent at BufLevel=0x02.
IC_BLACK_END at BufLevel=0x02 → StartPrint(1) immediately (Start=1 already confirmed from earlier poll).
Total: normal mode, BufLevel never hit 0 during upload.

### 4.2 Near-buffer-full with oscillating BufLevel (normal mode variant)

When the combined size of the current page upload and previous page already in buffer approaches 2 MB,
`BufLevel` may oscillate between 0 and 1 while sending. This is **still normal mode** as long as:
- `IC_BLACK_END` has NOT yet been sent when BufLevel first hits 0
- But the hit only happens during interleaved status polling (not because data couldn't be sent)

In practice: send one chunk, poll `GetBasicStatus`, if BufLevel=0 loop-poll until BufLevel≥1, then send next chunk.
`StartPrint` fires only after `IC_BLACK_END` once `Start==N` confirmed.

**Observed in hardtocompress2 page 2 (~1.4 MB, while page 1 printing):**
BufLevel oscillates 0↔1 for every chunk (engine consuming page 1's data simultaneously).
IC_BLACK_END sent at BufLevel=1 → StartPrint(2) immediately.
This is normal mode — BufLevel=0 appeared only in inter-chunk polling gaps, not blocking a send.

### 4.3 All-pages buffer overflow — every page streaming (hardtocompress4)

When **every page** in the job exceeds the buffer, the driver never enters normal mode. All
pages use streaming mode. The pipeline still works: D0A9(N+1) is sent as soon as Start==N is
confirmed, which may happen during the drain phase of page N's StartPrint.

**Observed in hardtocompress4 (3-page job, each page 2,240,364 bytes):**

Page 1 streaming sequence:
```
D0A9(page 1)
IC_VIDEO_DATA ×5 = 326420 bytes; poll → BufLevel=0x0f
GetExtendedStatus → Start=1, Printing=0  ← Start=1 confirmed at 326 KB
D0A9(page 2)                             ← sent IMMEDIATELY; Start=1 confirmed
IC_VIDEO_DATA ×5 = 652840; poll → BufLevel=0x0f
IC_VIDEO_DATA ×5 = 979260; poll → BufLevel=0x0f
IC_VIDEO_DATA ×5 = 1305680; poll → BufLevel=0x0c
IC_VIDEO_DATA ×5 = 1632100; poll → BufLevel=0x07
IC_VIDEO_DATA ×4 = 1893236; poll → BufLevel=0x03
IC_VIDEO_DATA ×3 = 2089088; poll → BufLevel=0x00 AND byte1 changed
GetExtendedStatus → Start=1, Printing=0  ← still BufLevel=0, chunks remain
StartPrint(1)                            ← STREAMING: fire at BufLevel=0
GetBasicStatus ×10 → BufLevel=0x00      engine consuming
GetExtendedStatus → Start=1, Printing=0 ← drain ongoing
GetBasicStatus ×381 → BufLevel=0x00     engine still consuming
GetExtendedStatus → Start=2, Printing=1 ← Start=2 confirmed DURING DRAIN
GetBasicStatus ×11 → BufLevel=0x01      ← BufLevel rose
IC_VIDEO_DATA (65284) = 2154372
poll: BufLevel=0x00 ×2 → 0x01
IC_VIDEO_DATA (65284) = 2219656
poll: BufLevel=0x00 ×2 → 0x01
IC_VIDEO_DATA (20708) = 2240364         ← last chunk
poll → BufLevel=0x01
IC_BLACK_END                            ← BufLevel ≥ 1
```

**Critical new observation:** During the streaming drain (hundreds of BufLevel=0 polls after
`StartPrint(N)`), the `Start` counter can advance to N+1. This means the printer decoded D0A9(N+1)
while page N's data was being consumed. By the time the last chunk is sent and `IC_BLACK_END`
issued, `Start` is already confirmed for the next page — enabling immediate `StartPrint(N+1)`
if that page also hits streaming, or skipping the post-`IC_BLACK_END` poll.

**After IC_BLACK_END of page 1:** GetExtendedStatus → Start=2, Printing=1 — Start=2 already
confirmed during drain; page 2 data upload begins immediately in oscillating BufLevel=0↔1 pattern.

Page 2 follows the identical streaming pattern (same 2,240,364 bytes). During page 2's streaming
drain after StartPrint(2), the printer decodes D0A9(page 3) and Start advances to 3. After page
2's IC_BLACK_END, BufLevel rises. When Printed=1 is first seen AND driver is between pages,
SetJobInfo2(flag=2) fires, then D0A9(page 3) is sent (Start=3 was already confirmed during drain).

### 4.4 Single-page buffer overflow — streaming mode (BufLevel hits 0 with chunks remaining)

One page's compressed data exceeds the 2 MB buffer. Engine must start printing while data
is still being fed. BufLevel hits 0 BEFORE the last chunk, and `StartPrint` fires BEFORE `IC_BLACK_END`:

```
[Page N — large page, >2 MB compressed]
D0A9{D0A0,D0A4,D0A1,D0A2}
loop: send IC_VIDEO_DATA chunks
  poll GetBasicStatus every ~5 chunks; BufLevel dropping
  when BufLevel hits 0x01: switch to 1 chunk per poll
  when BufLevel == 0 AND chunks still remaining:
    StartPrint(N)        ← fire NOW, before IC_BLACK_END
    loop: poll GetBasicStatus until BufLevel >= 1
          [15–382 polls expected at BufLevel=0; this is normal]
          optionally call GetExtendedStatus on byte1 change during drain
    send remaining chunk(s) (1 chunk per BufLevel≥1 poll)
IC_BLACK_END             ← after very last chunk (BufLevel ≥ 1 at this point)
```

**Observed in hardtocompress2 page 3 (~2.15 MB), detailed trace:**

```
Phase                                     BufLevel    Action
-----------------------------------------------------------
D0A9 sent (after SetJobInfo2 flag=2)       0x0f        Start=3 confirmed
Burst ×5 chunks                            0x0f        —
Burst ×5 chunks                            0x0e        —
Burst ×5 chunks                            0x0b        —
Burst ×5 chunks                            0x07        —
Burst ×5 chunks                            0x04        —
Chunk (BufLevel≥1 wait per chunk)          0x01        —
Chunk (BufLevel≥1 wait)                    0x00→0x01   —
Chunk                                      0x00        StartPrint(3) fired at BufLevel=0
Poll ×17 (GetBasicStatus tight loop)       0x00        engine consuming
GetExtendedStatus → Start=3, Printing=2    0x00        —
Poll ×72 (GetBasicStatus tight loop)       0x00        engine consuming
GetExtendedStatus → Printing=3             0x00        —
Poll ×15 until BufLevel rises              0x01        —
Last chunk (62,324 bytes) sent             0x01→0x00   remaining data delivered
Poll: BufLevel confirmed ≥ 1              0x01        IC_BLACK_END sent
```

**`StartPrint(N)` general rule across all cases:**
Fire as soon as `Start == N` is confirmed AND one of:
- `IC_BLACK_END` sent (normal), OR
- `BufLevel == 0` with data still remaining (streaming — `IC_BLACK_END` comes later)

**`IC_BLACK_END` rule:** Always sent after the last `IC_VIDEO_DATA` chunk for the page,
regardless of whether `StartPrint` has been sent yet. In streaming mode, wait for
BufLevel ≥ 1 before sending `IC_BLACK_END` after the last chunk.

**BufLevel=0 during large-page streaming is NOT an error.** The engine is consuming.
Observed: 15–382 polls at BufLevel=0 before it rises to 1.

---

## 5. Mid-Job SetJobInfo2(flag=2)

```
[During page loop — once per job only]
Send SetJobInfo2(flag=2, JobID) at the first inter-page idle moment when Printed >= 1.
```

**Rule:** Send `SetJobInfo2(flag=2)` once per job, at the **first moment the driver is between
pages** (not mid-`IC_VIDEO_DATA`, not mid-polling-for-`StartPrint`) AND `Printed >= 1`.

**Observed in `lbp3000-windowsxp.txt` (4-page job):**
`Printed=1` first appeared mid-job (after page 1 done) but driver was busy uploading page 3.
`Printed=2` first appeared when driver was idle between pages 3 and 4. `SetJobInfo2(flag=2)` fired here.

**Observed in `lbp3000-windowsxp-hardtocompress2.txt` (3-page job):**
After `StartPrint(2)` → BufLevel rises → `GetExtendedStatus` shows `Printed=1, Printing=2`.
Driver is now between pages (about to start page 3). `SetJobInfo2(flag=2)` fired here.
Sequence: StartPrint(2) → poll BufLevel rising → GetExtendedStatus (Printed=1) → GetBasicStatus → **SetJobInfo2(flag=2)** → GetBasicStatus + GetExtendedStatus → D0A9(page 3).

---

## 6. Full 4-Page Command Sequence (lbp3000-windowsxp.txt — normal pages)

```
── STARTUP ──────────────────────────────────────────────────────────────────
GetPrinterInfo           → Blk=65520, Buf=64
GetExtendedStatus        → Bas=0x31 (offline+free), Bas1=0x8a (needGetInputStatus)
GetInputStatus
[×4 more GetExtendedStatus + GetInputStatus until Bas1 & 0x02 clears]

── JOB INIT ─────────────────────────────────────────────────────────────────
ReserveUnit              → JobID=1
SetJobInfo2(flag=1)      flag=1, JobID=1, hostname/username/jobname
GetBasicStatus           → 0x30 (offline; Bas & 0x10 set)
GetExtendedStatus        → Bas=0x30 (already offline)
GetInputStatus
[GoOffline SKIPPED — Bas & 0x10 set]
ClearMisPrint
ClearError
DiscardData
GetBasicStatus
GoOnline(eedbeaad 000000000000000000000000)   ← 16 bytes
GetBasicStatus           → 0x00 (online)
GetExtendedStatus        → Start=0,Printing=0,Shipped=0,Printed=0
GetBasicStatus

── PAGE 1 DATA ───────────────────────────────────────────────────────────────
D0A9{D0A0(PageSeq=0x000d),D0A4,D0A1,D0A2}
IC_VIDEO_DATA ×5 (65284 each = 326420 bytes)
GetBasicStatus→changed; GetExtendedStatus → Start=1, Printing=0  ← Start=1 confirmed
IC_VIDEO_DATA (65284) = 391704
IC_VIDEO_DATA (60068) = 451772 total
IC_BLACK_END
StartPrint(1)            ← Start=1 already confirmed; fires immediately

── PAGE 2 DATA (uploaded while page 1 prints) ───────────────────────────────
GetBasicStatus; GetExtendedStatus → Start=1, Printing=0
[GetBasicStatus ×2]
D0A9{D0A0(PageSeq=0x000e),D0A4,D0A1,D0A2}
IC_VIDEO_DATA ×7 + 3540  (460528 bytes total)
IC_BLACK_END
[poll GetBasicStatus ×~14 until byte1 changes]
GetExtendedStatus        → Start=2, Printing=1   ← Start==2
StartPrint(2)

── BETWEEN PAGE 2 & 3 (engine printing page 1, then page 2) ─────────────────
[poll ×~14]; GetExtendedStatus → Start=2, Printing=1, Shipped=1, Printed=0
[poll ×few]; GetExtendedStatus → Start=2, Printing=2, Shipped=1, Printed=0
[poll ×4]; GetBasicStatus → 0x00, Bas1 has 0x02
GetExtendedStatus        → Start=2, Printing=2, Shipped=1, Printed=1
GetInputStatus           ← Bas1 & 0x02
GetBasicStatus

── PAGE 3 DATA (uploaded while page 2 prints) ───────────────────────────────
D0A9{D0A0(PageSeq=0x0008),D0A4,D0A1,D0A2}
IC_VIDEO_DATA ×4 + 1892  (263028 bytes total)
IC_BLACK_END
GetBasicStatus→changed; GetExtendedStatus → Start=3, Printing=2   ← Start==3
StartPrint(3)

── BETWEEN PAGE 3 & 4 ───────────────────────────────────────────────────────
[poll ×~6]; GetExtendedStatus → Start=3, Printing=2, Shipped=2, Printed=1
[poll ×2]; GetExtendedStatus → Start=3, Printing=2, Shipped=2, Printed=1
[poll ×2]; GetExtendedStatus → Start=3, Printing=3, Shipped=2, Printed=1
[poll ×5]; GetExtendedStatus → Start=3, Printing=3, Shipped=2, Printed=2
GetBasicStatus
SetJobInfo2(flag=2)      ← Printed>=1 AND driver between pages (first opportunity here)
GetBasicStatus; GetExtendedStatus → Start=3, Printing=3, Shipped=2, Printed=2

── PAGE 4 DATA (uploaded while page 3 prints) ───────────────────────────────
D0A9{D0A0(PageSeq=0x000b),D0A4,D0A1,D0A2}
IC_VIDEO_DATA ×5 (65284 each = 326420)
GetBasicStatus→changed; GetExtendedStatus → Start=4, Printing=3   ← Start==4
IC_VIDEO_DATA (43284) = 369704 total
IC_BLACK_END
StartPrint(4)            ← Start=4 already confirmed; fires immediately

── JOB TERMINATION ──────────────────────────────────────────────────────────
GetBasicStatus; GetExtendedStatus → Start=4, Printing=3, Shipped=2, Printed=2
SetJobInfo2(flag=6)      ← Windows job-end (Linux: flag=3)
ReleaseUnit(01 00)       ← JobID=1

── POST-JOB DRAIN ───────────────────────────────────────────────────────────
loop (alternate GetInputStatus + GetExtendedStatus):
  break when Printed == 4 (totalPages)
  [~25–30 cycles; LED transitions 0x57 → 0x56 when last page done]
```

---

## 7. Full 3-Page Command Sequence (lbp3000-windowsxp-hardtocompress2.txt — mixed large pages)

**Page sizes:** page 1 = 1,901,688 bytes (normal mode), page 2 = 1,385,160 bytes (normal mode with oscillating BufLevel), page 3 = 2,151,412 bytes (streaming mode).

**Key difference from §6:** Printer was online at start (previous job done) → `GoOffline` IS called.

```
── STARTUP ──────────────────────────────────────────────────────────────────
GetPrinterInfo           → Blk=65520, Buf=64
GetExtendedStatus        → Bas=0x01 (online+free), Start=1,Printing=1,Shipped=1,Printed=1
GetInputStatus           ← Bas1 & 0x02
[×3 more GetExtendedStatus + GetInputStatus cycles]

── JOB INIT ─────────────────────────────────────────────────────────────────
ReserveUnit              → JobID=2
SetJobInfo2(flag=1)      flag=1, JobID=2
GetBasicStatus           → 0x00 (online; Bas & 0x10 CLEAR → must GoOffline)
GetExtendedStatus        → Bas=0x00, Start=1,Printing=1,Shipped=1,Printed=1
GetInputStatus
GoOffline(00 00)         ← printer online → must call GoOffline
GetBasicStatus           → 0x10 (offline confirmed)
GetExtendedStatus        → Bas=0x10 (offline)
[GoOffline complete]
ClearMisPrint
ClearError
DiscardData
GetBasicStatus
GoOnline(eedbeaad 000000000000000000000000)   ← 16 bytes
GetBasicStatus           → 0x00 (online)
GetExtendedStatus        → Start=0,Printing=0,Shipped=0,Printed=0  ← ALL COUNTERS RESET
GetBasicStatus

── PAGE 1 DATA (~1.9 MB, normal mode) ────────────────────────────────────────
D0A9{D0A0(PageSeq=0x003a),D0A4,D0A1,D0A2}
[page 2 D0A9 sent IMMEDIATELY after first status poll showing Start=1]

IC_VIDEO_DATA ×5 (326420 bytes)
GetBasicStatus→changed; GetExtendedStatus → Start=1, Printing=0, Aux=RCF_SAFE_TIMER
D0A9{D0A0(PageSeq=0x002a),D0A4,D0A1,D0A2}   ← page 2 announced; Start==1 confirmed
IC_VIDEO_DATA ×5 = 652840 bytes
GetBasicStatus → BufLevel=0x0f (no change yet in Bas byte1)
IC_VIDEO_DATA ×5 = 979260
GetBasicStatus → BufLevel=0x0f
IC_VIDEO_DATA ×5 = 1305680
GetBasicStatus → BufLevel=0x0c
IC_VIDEO_DATA ×5 = 1632100
GetBasicStatus → BufLevel=0x07
IC_VIDEO_DATA ×4 + small(8452) = 1901688 total   ← BufLevel=0x02 after poll
IC_BLACK_END             ← BufLevel=0x02 (never hit 0)
StartPrint(1)            ← Start=1 already confirmed; normal mode; fires immediately

── PAGE 2 DATA (~1.4 MB, normal mode with oscillating BufLevel) ─────────────
[BufLevel oscillates 0↔1 each chunk — engine consuming page 1 while we upload page 2]
IC_VIDEO_DATA ×1 (65284); poll → BufLevel=0x01
IC_VIDEO_DATA ×1; poll → BufLevel=0x00; poll ×382 → BufLevel=0x00
GetExtendedStatus        → Start=2, Printing=1   ← BufLevel=0x00 first seen here
[BufLevel=0 during inter-chunk polling; NOT streaming — no chunks pending yet]
poll until BufLevel≥1 → BufLevel=0x01
IC_VIDEO_DATA ×1; poll → BufLevel=0x00; poll ×10 → BufLevel=0x01
[pattern: 1 chunk per BufLevel≥1 poll; oscillates 0↔1 throughout]
IC_VIDEO_DATA (repeat pattern) ... 1385160 total bytes
IC_BLACK_END             ← BufLevel=0x01; last chunk sent; no more pending → IC_BLACK_END first
GetBasicStatus→changed; GetExtendedStatus  [byte1 changed during/after IC_BLACK_END]
StartPrint(2)            ← Start=2 confirmed from earlier poll

── BETWEEN PAGE 2 & 3 ───────────────────────────────────────────────────────
[poll GetBasicStatus; BufLevel rising 0x02→0x04→0x06→0x07→0x09→0x0a]
GetExtendedStatus        → Start=2, Printing=1, Shipped=1, Printed=0
GetExtendedStatus        → Start=2, Printing=1, Shipped=1, Printed=0
GetExtendedStatus        → Start=2, Printing=2, Shipped=1, Printed=0
[poll until BufLevel=0x0f; GetBasicStatus byte1 changes]
GetExtendedStatus        → Start=2, Printing=2, Shipped=1, Printed=1   ← Printed=1
GetBasicStatus
SetJobInfo2(flag=2)      ← Printed>=1 AND between pages → fire now
GetBasicStatus; GetExtendedStatus → Start=2, Printing=2, Shipped=1, Printed=1
GetBasicStatus

── PAGE 3 DATA (~2.15 MB, STREAMING MODE) ────────────────────────────────────
D0A9{D0A0(PageSeq=0x0041),D0A4,D0A1,D0A2}
[GetExtendedStatus after 5 chunks → Start=3 confirmed; Printing=2]

IC_VIDEO_DATA ×5 (326420); poll → BufLevel=0x0f
IC_VIDEO_DATA ×5 (652840); poll → BufLevel=0x0e
IC_VIDEO_DATA ×5 (979260); poll → BufLevel=0x0b
IC_VIDEO_DATA ×5 (1305680); poll → BufLevel=0x07
IC_VIDEO_DATA ×5 (1632100); poll → BufLevel=0x04
IC_VIDEO_DATA ×4 (1893236); poll → BufLevel=0x01
IC_VIDEO_DATA ×1 (1958520); poll → BufLevel=0x00; poll ×2 → BufLevel=0x01
IC_VIDEO_DATA ×1 (2023804); poll ×8 → BufLevel=0x01
IC_VIDEO_DATA ×1 (2089088); poll → BufLevel=0x00   ← STREAMING TRIGGER
GetExtendedStatus        → Start=3, Printing=2, Shipped=2, Printed=1  [still BufLevel=0]
StartPrint(3)            ← BufLevel=0 AND chunks remain → STREAMING MODE: fire now
GetBasicStatus ×17       → BufLevel=0x00 (engine consuming)
GetExtendedStatus        → Start=3, Printing=2, Shipped=2, Printed=1
GetBasicStatus ×72       → BufLevel=0x00 (engine still consuming)
GetExtendedStatus        → Start=3, Printing=3, Shipped=2, Printed=1  (Printing advanced)
GetBasicStatus ×15       → BufLevel=0x01   ← BufLevel rose; can send
IC_VIDEO_DATA (62324) = 2151412 total   ← final chunk sent
GetBasicStatus; poll     → BufLevel=0x01   ← confirmed ≥ 1
IC_BLACK_END             ← after last chunk, BufLevel ≥ 1

── JOB TERMINATION ──────────────────────────────────────────────────────────
GetBasicStatus; GetExtendedStatus → Start=3, Printing=3, Shipped=2, Printed=1
SetJobInfo2(flag=6)      ← Windows job-end
ReleaseUnit(02 00)       ← JobID=2

── POST-JOB DRAIN ───────────────────────────────────────────────────────────
loop (alternate GetInputStatus + GetExtendedStatus):
  Printed=1 → Printed=2 → Printed=3
  break when Printed == 3 (totalPages)
  [LED transitions 0x57 → 0x56; Aux: RCF_SAFE_TIMER clears; RCF_PAPER_DELIVERY clears]
```

---

## 8. Full 3-Page Command Sequence (lbp3000-windowsxp-hardtocompress4.txt — ALL pages streaming)

**Page sizes:** all 3 pages = 2,240,364 bytes each (every page streaming mode).

**Key difference from §7:** Printer was offline at start (Bas=0x31) → `GoOffline` SKIPPED.
**Every page exceeds 2 MB buffer.** Pipeline still maintained via early D0A9 announcement and
Start counter advancing during streaming drain.

```
── STARTUP ──────────────────────────────────────────────────────────────────
GetPrinterInfo           → Blk=65520, Buf=64
GetExtendedStatus        → Bas=0x31 (offline+free), Bas1=0x8a (needGetInputStatus)
                           Start=0,Printing=0,Shipped=0,Printed=0
GetInputStatus           ← Bas1 & 0x02
[×3 more GetExtendedStatus + GetInputStatus until Bas1 & 0x02 clears]

── JOB INIT ─────────────────────────────────────────────────────────────────
ReserveUnit              → JobID=1
SetJobInfo2(flag=1)      flag=1, JobID=1
GetBasicStatus           → 0x30 (Bas & 0x10 set → already offline)
GetExtendedStatus        → Bas=0x30 (offline), Start=0..Printed=0
GetInputStatus
[GoOffline SKIPPED — Bas & 0x10 already set]
ClearMisPrint
ClearError
DiscardData
GetBasicStatus
GoOnline(eedbeaad 000000000000000000000000)   ← 16 bytes
GetBasicStatus           → 0x00 (online)
GetExtendedStatus        → Start=0,Printing=0,Shipped=0,Printed=0  ← ALL RESET
GetBasicStatus ×2

── PAGE 1 DATA (~2.24 MB, STREAMING on page 1 — no previous data in buffer) ─
D0A9{D0A0(PageSeq),D0A4,D0A1,D0A2}              ← page 1
IC_VIDEO_DATA ×5 = 326420; poll → BufLevel=0x0f (byte1 changed)
GetExtendedStatus        → Start=1, Printing=0  ← Start=1 confirmed at 326 KB

D0A9{D0A0(PageSeq),D0A4,D0A1,D0A2}              ← page 2 announced (Start=1 confirmed)

IC_VIDEO_DATA ×5 = 652840; poll → BufLevel=0x0f (no byte1 change; burst continues)
IC_VIDEO_DATA ×5 = 979260; poll → BufLevel=0x0f
IC_VIDEO_DATA ×5 = 1305680; poll → BufLevel=0x0c
IC_VIDEO_DATA ×5 = 1632100; poll → BufLevel=0x07
IC_VIDEO_DATA ×4 = 1893236; poll → BufLevel=0x03
IC_VIDEO_DATA ×3 = 2089088; poll → BufLevel=0x00, byte1 changed
GetExtendedStatus        → Start=1, Printing=0, Aux=RCF_SAFE_TIMER|RCF_PAPER_DELIVERY
StartPrint(1)            ← BufLevel=0 AND chunks remain → STREAMING MODE: fire now

[Drain: BufLevel stays 0 while engine consumes]
GetBasicStatus ×10       → BufLevel=0x00
GetExtendedStatus        → Start=1, Printing=0, Shipped=0, Printed=0
GetBasicStatus ×381      → BufLevel=0x00
GetExtendedStatus        → Start=2, Printing=1, Shipped=0, Printed=0  ← Start=2 confirmed DURING DRAIN
GetBasicStatus ×11       → BufLevel=0x01  ← BufLevel rose; send resumes

[Remaining chunks — 1 chunk per BufLevel≥1 poll; oscillates 0↔1 per chunk]
IC_VIDEO_DATA (65284) = 2154372
[poll: 0×2 → 0x01]
IC_VIDEO_DATA (65284) = 2219656
[poll: 0×2 → 0x01]
IC_VIDEO_DATA (20708) = 2240364         ← last chunk
poll → BufLevel=0x01
IC_BLACK_END             ← BufLevel ≥ 1; after last chunk
GetBasicStatus→changed; GetExtendedStatus → Start=2, Printing=1  ← Start=2 already confirmed

── PAGE 2 DATA (~2.24 MB, STREAMING — page 1 data still in buffer/printing) ─
[Immediately begin page 2 upload; oscillating BufLevel pattern throughout]
IC_VIDEO_DATA ×1; poll → 0x01
IC_VIDEO_DATA ×1; poll → 0x00 ×2 → 0x01
IC_VIDEO_DATA ×1; poll → 0x00 ×2 → 0x01
[pattern repeats 1 chunk per BufLevel≥1 poll for entire page]
... (identical oscillating pattern, 2240364 bytes total) ...
IC_VIDEO_DATA (65284); poll → 0x00, byte1 changed
GetExtendedStatus        → Start=3, Printing=2, Shipped=2, Printed=0
  [NOTE: Printed=0 here — page 1 not yet ejected]
StartPrint(2)            ← BufLevel=0 AND chunks remain → STREAMING
GetBasicStatus ×18       → BufLevel=0x00
GetExtendedStatus        → Start=3, Printing=2, Shipped=2, Printed=0
  [NOTE: Start=3 confirmed during drain — printer decoded D0A9(3) we sent earlier]
GetBasicStatus ×83       → BufLevel=0x00
GetExtendedStatus        → Start=3, Printing=3, Shipped=2, Printed=0
  [Printing=3 during drain before IC_BLACK_END! Counter advances because engine started pg3]
GetBasicStatus ×3        → BufLevel=0x01
IC_VIDEO_DATA (65284) = 2154372; [poll 0×2 → 0x01]
IC_VIDEO_DATA (65284) = 2219656; [poll 0×2 → 0x01]
IC_VIDEO_DATA (20708) = 2240364; poll → 0x01
IC_BLACK_END             ← BufLevel ≥ 1

GetBasicStatus→changed; GetExtendedStatus → Start=3, Printing=3, Shipped=2, Printed=0
  [Still Printed=0 — neither page 1 nor page 2 ejected yet while we were uploading]

── BETWEEN PAGE 2 & 3 (BufLevel rising as engine consumes page 2 tail) ──────
[BufLevel rising: 0x03→0x06→0x08→0x0a]
GetExtendedStatus        → Start=3, Printing=3, Shipped=2, Printed=1  ← Printed=1 first seen
GetInputStatus           ← Bas1 & 0x02
GetBasicStatus
SetJobInfo2(flag=2)      ← Printed>=1 AND between pages (after IC_BLACK_END of page 2)
GetBasicStatus; GetExtendedStatus → Start=3, Printing=3, Shipped=2, Printed=1
  [Start=3 confirmed DURING page 2 streaming drain — printer decoded D0A9(page 3) while draining]

D0A9{D0A0(PageSeq),D0A4,D0A1,D0A2}              ← page 3 announced here
  [D0A9(page 3) is sent NOW, after SetJobInfo2(flag=2). Start=3 was already confirmed during
   the page 2 drain. This is the only time D0A9(page 3) is sent — it was NOT sent earlier.
   Start counter advancing to 3 during page 2 drain just means the printer decoded D0A9(3)
   that was transmitted earlier as part of pipelining (sent at StartPrint(2) time or just after).]

── PAGE 3 DATA (~2.24 MB, STREAMING — same pattern) ─────────────────────────
IC_VIDEO_DATA ×5 = 326420; poll → BufLevel=0x08, byte1 changed
GetExtendedStatus        → Start=3, Printing=2, Shipped=1, Printed=1
  [NOTE: Printing=2 here although Printing=3 was seen before — counters can appear out of order
   in different GetExtendedStatus calls; Start=3 confirmed at first poll]
[BufLevel drops 0x08→0x06→0x03→0x02→0x01 as chunks accumulate with page 2 still consuming]
[BufLevel oscillates 0↔1 through the upload; pattern identical to page 2]
... (2089088 bytes sent before BufLevel hits 0)
IC_VIDEO_DATA (65284) = 2089088; poll → BufLevel=0x00, byte1 changed
GetExtendedStatus        → Start=3, Printing=2, Shipped=2, Printed=1
StartPrint(3)            ← BufLevel=0 AND chunks remain → STREAMING
GetBasicStatus ×23       → BufLevel=0x00
GetExtendedStatus        → Start=3, Printing=2, Shipped=2, Printed=1
GetBasicStatus ×67       → BufLevel=0x00
GetExtendedStatus        → Start=3, Printing=3, Shipped=2, Printed=1
GetBasicStatus ×8        → BufLevel=0x01
IC_VIDEO_DATA (65284) = 2154372; [poll 0×2 → 0x01]
IC_VIDEO_DATA (65284) = 2219656; [poll 0×2 → 0x01]
IC_VIDEO_DATA (20708) = 2240364; poll → 0x01
IC_BLACK_END             ← BufLevel ≥ 1

GetBasicStatus→changed; GetExtendedStatus → Start=3, Printing=3, Shipped=2, Printed=1

── JOB TERMINATION ──────────────────────────────────────────────────────────
SetJobInfo2(flag=6)      ← Windows job-end; immediately after IC_BLACK_END + GetExtendedStatus
ReleaseUnit(01 00)       ← JobID=1

── POST-JOB DRAIN ───────────────────────────────────────────────────────────
[GetExtendedStatus → Bas=0x05 (RCF_PRINTER_FREE set), Start=3,Printing=3,Shipped=2,Printed=1]
[Aux=0x84 (RCF_PAPER_DELIVERY|RCF_SAFE_TIMER)]
loop (alternate GetInputStatus + GetExtendedStatus):
  Printed=1 → Printed=2 → Printed=3
  Aux: 0x84 (PAPER_DELIVERY|SAFE_TIMER) → 0x04 (PAPER_DELIVERY only) → 0x00 (both clear)
  break when Printed == 3 AND Aux == 0x00
  [~20 cycles; LED transitions 0x57 → 0x56 when last page done; Aux clears last]
```

**Unique observations from hardtocompress4 (all-streaming case):**

1. **D0A9(N+1) sent mid-upload of page N** (at 326 KB into page 1, Start=1 confirmed → D0A9(2) sent).
2. **Start counter can advance during streaming drain**: while polling BufLevel=0 after StartPrint(N),
   the printer decoder accepts D0A9(N+1) and Start becomes N+1. The driver notes this but takes no
   action — it simply continues draining until BufLevel≥1.
3. **Printing counter can advance during streaming drain**: `Printing` may increment to N+2 during
   the drain after `StartPrint(N+1)` before page N's IC_BLACK_END — because the engine already
   started printing page N+1's data. This is normal.
4. **SetJobInfo2(flag=2) after page 2 IC_BLACK_END**: even though no page is fully ejected yet
   (Printed=0 during page 2 upload), Printed becomes 1 as soon as BufLevel rises after IC_BLACK_END
   and the engine finishes ejecting page 1. Fire SetJobInfo2(flag=2) immediately.
5. **D0A9 timing when D0A9(N+1) was pre-confirmed during drain**: if Start already equals N+1
   when you reach the between-pages point, D0A9(N+1) was already sent and accepted — do NOT send
   it again. Only send D0A9 for the page whose Start has NOT yet been confirmed.
6. **Post-job drain exit condition**: loop `GetExtendedStatus + GetInputStatus` until BOTH
   `Printed == totalPages` AND `Aux == 0x00` (RCF_SAFE_TIMER and RCF_PAPER_DELIVERY both clear).
   In the all-streaming case, Printed may still be 1 right after job termination.

---

## 9. Linux One-Shot Job Lifecycle

Unlike the Windows persistent daemon, the Linux captdriver **starts, prints, de-inits, and exits**:

```
[Startup]
GetPrinterInfo (×2)
GetExtendedStatus (×2)

[Init — printer already offline at start (Bas=0x31)]
ReserveUnit              → JobID
DiscardData              ← Linux order differs: DiscardData FIRST
ClearMisPrint
ClearError
GoOnline(ee db ea ad 00 00 00 00)   ← 8 bytes (Linux uses 8, not 16)
GetPrinterInfo
GetExtendedStatus        → online, counters = 0
SetLEDStatus(0x16)       ← PrinterReadyToPrint; sent BEFORE SetJobInfo2
GetInputStatus
GetPrinterInfo
GetExtendedStatus
SetLEDStatus(0x17)       ← CNPrinting
SetJobInfo2(flag=1)

[Page loop — same pipeline model; small chunks (228 bytes typical), poll after every chunk]
[SetJobInfo2(flag=2) + SetLEDStatus(0x17) sent as heartbeat every ~10–15 polls — Linux bug]

[Job end]
SetJobInfo2(flag=2) + SetLEDStatus(0x17)   ← final heartbeat
SetJobInfo2(flag=3)      ← Linux uses flag=3 (not 6)
GetExtendedStatus
ClearError
DiscardData
GetExtendedStatus
GoOffline(01 00)         ← Linux passes JobID as payload (Windows sends 00 00)
ReleaseUnit(01 00)

[De-init / exit wait]
GetExtendedStatus × few
GetPrinterInfo
GetInputStatus
GetExtendedStatus        ← poll until RCF_SAFE_TIMER (Aux bit 0x80) clears
                           and Bas = 0x11 (PRINTER_FREE | RCF_OFFLINE)
[exit]
```

**De-init exit condition:** Wait until `GetExtendedStatus.Aux & 0x80` (RCF_SAFE_TIMER) is clear,
indicating the motor has stopped. Then the process can exit safely.

---

## 10. Page Counter Progression (Normal Mode, 4-Page Job)

| Event                           | Start | Printing | Shipped | Printed |
|---------------------------------|-------|----------|---------|---------|
| After GoOnline                  | 0     | 0        | 0       | 0       |
| Page 1 D0A9 accepted            | 1     | 0        | 0       | 0       |
| StartPrint(1) issued            | 1     | 0        | 0       | 0       |
| Page 2 D0A9 accepted            | 2     | 1        | 0       | 0       |
| StartPrint(2) issued            | 2     | 1        | 0       | 0       |
| Page 1 ejecting                 | 2     | 1        | 1       | 0       |
| Page 2 printing; Bas1 fires     | 2     | 2        | 1       | 1       |
| Page 3 D0A9 accepted            | 3     | 2        | 1       | 1       |
| StartPrint(3) issued            | 3     | 2        | 1       | 1       |
| Page 2 ejecting                 | 3     | 2        | 2       | 1       |
| Page 3 printing; Printed=2      | 3     | 3        | 2       | 2       |
| SetJobInfo2(flag=2) sent here   | 3     | 3        | 2       | 2       |
| Page 4 D0A9 accepted            | 4     | 3        | 2       | 2       |
| StartPrint(4) issued            | 4     | 3        | 2       | 2       |
| Page 3 ejecting                 | 4     | 3        | 3       | 2       |
| Page 4 printing                 | 4     | 4        | 3       | 2→3     |
| Page 4 ejecting                 | 4     | 4        | 4       | 3       |
| All done — LED=0x56             | 4     | 4        | 4       | 4       |

`Start` increments when the printer decoder **accepts** a D0A9 descriptor — not when D0A9 is sent.
In large-page streaming, this can lag significantly (hundreds of polls).

**Page counter progression — hardtocompress2 (3-page, streaming on page 3):**

| Event                           | Start | Printing | Shipped | Printed | BufLevel |
|---------------------------------|-------|----------|---------|---------|----------|
| After GoOnline (counters reset) | 0     | 0        | 0       | 0       | 0x0f     |
| Page 1 D0A9 accepted            | 1     | 0        | 0       | 0       | 0x0f     |
| Page 2 D0A9 sent                | 1     | 0        | 0       | 0       | 0x0f     |
| Page 1 IC_BLACK_END+StartPrint  | 1     | 0        | 0       | 0       | 0x02     |
| Page 2 upload begins            | 1     | 0        | 0       | 0       | 0x01     |
| Start rises (engine decoded pg1)| 2     | 1        | 0       | 0       | 0x00     |
| Page 2 IC_BLACK_END+StartPrint  | 2     | 1        | 0       | 0       | 0x01     |
| BufLevel rising post-SP2        | 2     | 1        | 1       | 0       | rising   |
| Printed=1; SetJobInfo2(flag=2)  | 2     | 2        | 1       | 1       | 0x0f     |
| Page 3 D0A9 accepted            | 3     | 2        | 1       | 1       | 0x0f     |
| Page 3 BufLevel=0; StartPrint(3)| 3     | 2        | 2       | 1       | 0x00     |
| Drain (Printing advances)       | 3     | 3        | 2       | 1       | 0x00     |
| BufLevel rises; last chunk sent | 3     | 3        | 2       | 1       | 0x01     |
| Page 3 IC_BLACK_END             | 3     | 3        | 2       | 1       | 0x01     |
| Post-job drain: Printed rises   | 3     | 3        | 3       | 1→3     | rising   |

**Page counter progression — hardtocompress4 (3-page, ALL pages streaming):**

| Event                                        | Start | Printing | Shipped | Printed | BufLevel |
|----------------------------------------------|-------|----------|---------|---------|----------|
| After GoOnline (counters reset)              | 0     | 0        | 0       | 0       | 0x0f     |
| Page 1 D0A9 accepted (at 326 KB upload)      | 1     | 0        | 0       | 0       | 0x0f     |
| Page 2 D0A9 sent (Start=1 confirmed)         | 1     | 0        | 0       | 0       | 0x0f     |
| Page 1 BufLevel=0; StartPrint(1)             | 1     | 0        | 0       | 0       | 0x00     |
| Drain: Printing advances                     | 1→2   | 0→1      | 0       | 0       | 0x00     |
| Page 2 D0A9 accepted (during drain)          | 2     | 1        | 0       | 0       | 0x00     |
| BufLevel rises; page 1 tail + IC_BLACK_END   | 2     | 1        | 0       | 0       | 0x01     |
| Page 2 upload begins (oscillating BufLevel)  | 2     | 1        | 0       | 0       | 0↔1      |
| Page 2 BufLevel=0; StartPrint(2)             | 3     | 2        | 2       | 0       | 0x00     |
| Drain: Printing advances to 3                | 3     | 3        | 2       | 0       | 0x00     |
| BufLevel rises; page 2 tail + IC_BLACK_END   | 3     | 3        | 2       | 0       | 0x01     |
| BufLevel rising post IC_BLACK_END pg2        | 3     | 3        | 2       | 1       | rising   |
| Printed=1 seen; SetJobInfo2(flag=2)          | 3     | 3        | 2       | 1       | 0x0a     |
| D0A9(page 3) sent (Start=3 already known)   | 3     | 3        | 2       | 1       | 0x0f     |
| Page 3 BufLevel=0; StartPrint(3)             | 3     | 2        | 2       | 1       | 0x00     |
| Drain: Printing advances to 3                | 3     | 3        | 2       | 1       | 0x00     |
| BufLevel rises; page 3 tail + IC_BLACK_END   | 3     | 3        | 2       | 1       | 0x01     |
| Job terminated; post-job drain begins        | 3     | 3        | 2       | 1       | rising   |
| Post-drain: Printed rises                   | 3     | 3        | 3       | 1→3     | 0x0f     |
| All done — LED=0x56, Aux=0x00               | 3     | 3        | 3       | 3       | 0x0f     |

Note: `Shipped` counter at page 2 StartPrint shows 2 — both pages 1 and 2 shipped to engine
simultaneously because page 2's D0A9 was pre-accepted while page 1 was still printing.

---

## 11. SetJobInfo2 Timing

| Call | flag | When |
|------|------|------|
| `flag=1` | job start | After `ReserveUnit`, before clear sequence |
| `flag=2` | mid-job | **Windows:** once, when `Printed >= 1` first seen AND driver is between pages (not mid-stream, not mid-IC_VIDEO_DATA). **Linux:** heartbeat every ~10–15 polls (bug). |
| `flag=6` | job end (Windows) | After last page's `IC_BLACK_END` + final `GetExtendedStatus`, before `ReleaseUnit`. Do NOT wait for `Printed==totalPages`. |
| `flag=3` | job end (Linux) | Same timing as `flag=6`; followed by `ClearError → DiscardData → GoOffline → ReleaseUnit` |

**hardtocompress4 specific:** `SetJobInfo2(flag=2)` fires after page 2's `IC_BLACK_END`, during
the between-pages window when `BufLevel` rises and `Printed=1` is first observed. `Start=3` was
already confirmed during page 2's streaming drain, so `D0A9(page 3)` is sent immediately after
`SetJobInfo2(flag=2)` without any additional polling.

---

## 12. StartPrint Conditions

| Scenario | Condition | Order |
|----------|-----------|-------|
| Page 1 (normal) | `Start==1` confirmed at any point during/after upload | `IC_BLACK_END` → `StartPrint(1)` immediately |
| N≥2 (normal, BufLevel never 0) | All chunks sent, `IC_BLACK_END` sent, then poll for `Start==N` | `IC_BLACK_END` → poll until `Start==N` → `StartPrint(N)` |
| N (streaming, BufLevel hits 0 with chunks remaining) | `BufLevel==0` with chunks still to send | `StartPrint(N)` at BufLevel=0 → poll BufLevel≥1 → remaining chunks → `IC_BLACK_END` |
| N (oscillating BufLevel 0↔1, no chunks remaining) | All chunks already sent; BufLevel=0 only in polling gap | `IC_BLACK_END` (once BufLevel≥1) → `StartPrint(N)` (NOT streaming mode) |
| Page 1 (streaming, hardtocompress4) | Page 1 alone fills buffer without any prior data; BufLevel hits 0 mid-upload | Identical to streaming case: `StartPrint(1)` at BufLevel=0 → drain → remaining chunks → `IC_BLACK_END` |

---

## 13. BufLevel Reference

`BufLevel` from `GetBasicStatus` bytes 5-6 `& 0x0F` (0–15 slots):

| BufLevel | Action |
|----------|--------|
| 0x0f | Burst: send 4–5 chunks before polling |
| 0x08–0x0e | Reduce burst; poll more frequently |
| 0x01–0x07 | 1 chunk per poll cycle |
| 0x00 (chunks remaining) | **STREAMING**: fire `StartPrint(N)` immediately, then poll until BufLevel ≥ 1, then send remaining chunks, then `IC_BLACK_END` |
| 0x00 (no chunks remaining, post-SP polling) | Normal drain; engine consuming; wait for BufLevel ≥ 1 before `IC_BLACK_END` if needed |
| 0x00 (oscillating 0↔1 per chunk) | Near-buffer-full normal mode; send 1 chunk per BufLevel≥1 poll; `IC_BLACK_END` when last chunk sent at BufLevel≥1 |

---

## 14. Key Implementation Rules Summary

1. **Two-page pipeline is normal**: upload page N+1 while page N prints. Failure to do so stalls the engine (~10 s penalty).
2. **D0A9(N+1) timing**: send as soon as `Start == N` confirmed. Can precede `StartPrint(N)` and even precede completion of page N data upload. Can occur during streaming drain of page N.
3. **IC_VIDEO_DATA(N+1)**: must NOT start until page N's `IC_BLACK_END` is sent.
4. **GoOffline is conditional**: skip if `Bas & 0x10` already set. Must call if printer is online (`Bas & 0x10` clear) at job start.
5. **GoOnline resets all counters**: Start/Printing/Shipped/Printed all → 0.
6. **Streaming trigger is BufLevel=0 with chunks remaining** (not a fixed page size). A 1.9 MB page can be normal mode; a 2.15 MB page or page 1 in an all-large job triggers streaming. The trigger is *when* BufLevel reaches 0 relative to *when* the last chunk is sent.
7. **Normal mode**: last chunk → `IC_BLACK_END` → `StartPrint(N)` (after confirming `Start==N`).
8. **Streaming mode**: `StartPrint(N)` at BufLevel=0 (before `IC_BLACK_END`) → poll BufLevel≥1 → remaining chunk(s) → `IC_BLACK_END`.
9. **Oscillating BufLevel 0↔1 per chunk is normal mode** if no pending chunks exist when BufLevel hits 0. Only streaming if chunks actively remain to be sent.
10. **StartPrint(1)** fires immediately after `IC_BLACK_END` if `Start==1` was confirmed anytime during upload. No additional polling needed. If page 1 is itself streaming, `StartPrint(1)` fires at BufLevel=0 before `IC_BLACK_END`.
11. **Start counter can advance during streaming drain**: after `StartPrint(N)`, while polling BufLevel=0, the printer may decode D0A9(N+1) and advance `Start` to N+1. Note this but do not act — continue waiting for BufLevel≥1 to send remaining chunks.
12. **Do NOT re-send D0A9(N+1) if Start already equals N+1**: if Start was confirmed during a drain, D0A9(N+1) was already sent earlier. Only send D0A9 for a page whose D0A9 has not been sent yet.
13. **SetJobInfo2(flag=2)**: send once at first `Printed >= 1` when driver is between pages (after any `IC_BLACK_END`, before next `IC_VIDEO_DATA`). Linux sends as heartbeat (avoid this).
14. **SetJobInfo2(flag=6)** (Windows) / **flag=3** (Linux): send after last page's `IC_BLACK_END` + final `GetExtendedStatus`. Do NOT wait for `Printed == totalPages` before sending.
15. **Post-job drain exit**: alternate `GetInputStatus + GetExtendedStatus` until BOTH `Printed == totalPages` AND `Aux == 0x00` (RCF_SAFE_TIMER and RCF_PAPER_DELIVERY both cleared). In all-streaming jobs, Printed may still be 1 immediately after job termination.
16. **Linux GoOnline**: 8-byte payload (`ee db ea ad 00 00 00 00`). Windows: 16 bytes. Both accepted.
17. **Linux init clear order**: `DiscardData → ClearMisPrint → ClearError` (Windows: `ClearMisPrint → ClearError → DiscardData`).
18. **BufLevel=0 is never an error**: during streaming it means engine is consuming; during oscillating normal mode it means buffer is nearly full — in both cases wait for BufLevel≥1 before sending next chunk.
