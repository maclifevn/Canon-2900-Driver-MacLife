# captdriver Refactoring Plan

> **Reference:** [`info/lbp3000-protocol.md`](info/lbp3000-protocol.md) is the authoritative protocol reference.  
> All file references below use the form [`filename`](relative/path.ext:line) with line numbers where applicable.

---

## 1. Executive Summary

Analysis of the open-source `captdriver` against the authoritative protocol documentation reveals **25 distinct bugs and naming problems** across six files. Issues range from compile-time crashes (incorrect signal handler assignment), to protocol violations that will cause printer deadlocks on most real print jobs (wrong flow-control bit tested, missing BufLevel-based back-pressure), to semantic errors (wrong byte placed in TonerDensity/ResFlag fields of IC_BEGIN_PAGE), to naming confusion so severe that reading the code gives false impressions of what is actually happening.

The most critical issues that will cause observable failures are:

1. **Compile bug** — [`rastertocapt.c:352`](src/rastertocapt.c:352) calls `do_cancel()` instead of passing a function pointer, so the binary cannot be built with `-Werror` or will silently crash.
2. **Wrong flow-control bit** — `CAPT_FL_BUFFERFULL` is `CMD_BUSY` (0x04), not buffer fullness. Buffer-level checking in `lbp2900_page_prologue` spins on the wrong condition.
3. **BufLevel never read** — The primary back-pressure mechanism (bytes 5-6 of `GetBasicStatus`) is never parsed or used. Large-page jobs (≥2 MB compressed) will overflow the printer's buffer.
4. **Wrong flag checked in `capt_wait_ready`** — `CAPT_FL_BUSY` is the `ERROR_BIT` (0x80), not `CMD_BUSY` (0x04). The driver waits for errors to clear, not for commands to complete.
5. **IC_BEGIN_PAGE field byte-shift** — `dims->media_type` is placed at the `PaperType` field (byte 13) and `dims->media_adapt` at the `ResFlag` field (byte 14). ResFlag must be `0x11` (constant) for 600 dpi; putting `media_adapt` there sends an incorrect resolution code.
6. **GoOffline called unconditionally in LBP3000 prologue** — Protocol §9 item 4 says GoOffline is only safe when the printer is currently online.
7. **JobFlag=4 (abort) sent at end of normal job** in `lbp2900_job_epilogue` — should be flag=6 (Windows canonical end-of-job).
8. **ReserveUnit payload not all-zeros** — `magicbuf_0` contains `0x1E` at byte offset 2; protocol §2.5 specifies 8 zero bytes.

---

## 2. Detailed Issue List

### ISSUE-01 · Compile Bug: Signal Handler Assignment
**File:** [`src/rastertocapt.c`](src/rastertocapt.c:352)  
**Line:** 352  
**Severity:** CRITICAL (compile error / runtime crash)

```c
// Current (wrong):
act_cancel.sa_handler = do_cancel();   // CALLS the function, assigns its int return value

// Correct:
act_cancel.sa_handler = do_cancel;     // Assigns function pointer
```

`do_cancel` has signature `static void do_cancel(int s)` — calling it here with no argument is undefined behaviour. Compilers will reject this or produce a crash. The `#else` branch at line 358 correctly uses `signal(SIGTERM, do_cancel)`.

---

### ISSUE-02 · Wrong Constant: `CAPT_FL_BUSY` Maps to `ERROR_BIT`, Not `CMD_BUSY`
**File:** [`src/capt-status.h`](src/capt-status.h:42)  
**Line:** 42  
**Severity:** HIGH (wait-ready logic is incorrect)

```c
// Current:
CAPT_FL_BUSY = _FL(0, 7),   // bit 7 of byte 1 = 0x80 = ERROR_BIT per protocol
```

Protocol §2.4 byte 1:
- bit 2 (0x04) = `CMD_BUSY` — command is being processed / printing
- bit 7 (0x80) = `ERROR_BIT` — error state

[`capt_wait_ready()`](src/capt-status.c:122) loops while `CAPT_FL_BUSY` is set. It is therefore waiting for the error bit to clear, not for a command to complete. The driver should poll on `CMD_BUSY` (0x04).

**Fix:**
```c
CAPT_FL_CMD_BUSY = _FL(0, 2),   // 0x04 = CMD_BUSY — printer processing command
CAPT_FL_ERROR    = _FL(0, 7),   // 0x80 = ERROR_BIT
```
Update all callers of `CAPT_FL_BUSY` → `CAPT_FL_CMD_BUSY`.

---

### ISSUE-03 · Wrong Flag: `CAPT_FL_BUFFERFULL` Maps to `CMD_BUSY` (0x04)
**File:** [`src/capt-status.h`](src/capt-status.h:45)  
**Line:** 45  
**Severity:** HIGH (buffer-fullness check is incorrect)

```c
CAPT_FL_BUFFERFULL = _FL(0, 2),   // bit 2 = 0x04 = CMD_BUSY per protocol
```

This flag is tested in [`lbp2900_page_prologue()`](src/prn_lbp2900.c:314):
```c
if (! FLAG(lbp2900_get_status(state->ops), CAPT_FL_BUFFERFULL)) break;
```
This loop breaks when `CMD_BUSY` is *clear*, not when the data buffer has room. The correct back-pressure signal is **BufLevel** (bytes 5-6 of `GetBasicStatus`) — see ISSUE-09.

There is **no flag for `IM_DATA_BUSY`** (0x08) in the current status definitions, and no flag for `RCF_OFFLINE` (0x10). Both need to be added.

**Fix:** Rename and define correct flags:
```c
CAPT_FL_CMD_BUSY    = _FL(0, 2),   // 0x04 = CMD_BUSY (replaces both FL_BUSY and FL_BUFFERFULL)
CAPT_FL_IM_DATA_BUSY= _FL(0, 3),   // 0x08 = IM_DATA_BUSY (data buffer full indicator)
CAPT_FL_OFFLINE     = _FL(0, 4),   // 0x10 = RCF_OFFLINE
CAPT_FL_UNIT_FREE   = _FL(0, 6),   // 0x40 = UNIT_FREE
CAPT_FL_ERROR       = _FL(0, 7),   // 0x80 = ERROR_BIT
```

---

### ISSUE-04 · Wrong Flags: `CAPT_FL_UNINIT1` and `CAPT_FL_UNINIT2` are Unnamed Bit / `RCF_OFFLINE`
**File:** [`src/capt-status.h`](src/capt-status.h:43)  
**Lines:** 43–44  
**Severity:** HIGH (init condition check tests wrong bits)

```c
CAPT_FL_UNINIT1 = _FL(0, 5),  // bit 5 = 0x20 — not defined in protocol
CAPT_FL_UNINIT2 = _FL(0, 4),  // bit 4 = 0x10 = RCF_OFFLINE
```

`CAPT_FL_UNINIT2` is actually `RCF_OFFLINE` (0x10). `CAPT_FL_UNINIT1` is bit 5 (0x20) which has no defined meaning in the protocol.

These are tested in [`lbp2900_page_prologue()`](src/prn_lbp2900.c:302) to decide whether to run the ClearMisPrint/ClearError/DiscardData/GoOnline init sequence:
```c
if (FLAG(status, CAPT_FL_UNINIT1) || FLAG(status, CAPT_FL_UNINIT2)) {
```
The correct condition is: run the init sequence when `RCF_OFFLINE` (0x10) is set, i.e. when the printer is offline. Using the new `CAPT_FL_OFFLINE` flag:
```c
if (FLAG(status, CAPT_FL_OFFLINE)) {
```

---

### ISSUE-05 · Wrong Flag Name: `CAPT_FL_NOPAPER1` Is `RCF_NOTREADY`
**File:** [`src/capt-status.h`](src/capt-status.h:46)  
**Line:** 46  
**Severity:** MEDIUM (misleading name leads to misuse)

```c
CAPT_FL_NOPAPER1 = _FL(0, 1),   // bit 1 = 0x02 = RCF_NOTREADY per protocol
```

Protocol §2.4: bit 1 (0x02) = `RCF_NOTREADY` — printer not ready (covers many conditions, not just no-paper). `CAPT_FL_NOPAPER2` at `_FL(1, 14)` from `status[1]` is a different signal from the extended status.

**Fix:** Rename to `CAPT_FL_NOTREADY`. The actual no-paper detection requires checking `RCF_NOTREADY | RCF_OFFLINE` together and then confirming via `GetExtendedStatus.Cnt & 0x40` (RCF_PRINT_REJECTED) and `Pap == 0x00`.

---

### ISSUE-06 · Wrong Flag Name: `CAPT_FL_PROCESSING` Is `RCF_PRINTER_FREE`
**File:** [`src/capt-status.h`](src/capt-status.h:47)  
**Line:** 47  
**Severity:** LOW (misleading name)

```c
CAPT_FL_PROCESSING = _FL(0, 0),   // bit 0 = 0x01 = RCF_PRINTER_FREE per protocol
```

Protocol §2.4: bit 0 (0x01) = `RCF_PRINTER_FREE` — the printer unit is idle. "Processing" implies the opposite of free/idle.

**Fix:** Rename to `CAPT_FL_PRINTER_FREE`.

---

### ISSUE-07 · Flag Naming: `CAPT_FL_JOBSTAT_CHNG` and `CAPT_FL_XSTATUS_CHNG`
**File:** [`src/capt-status.h`](src/capt-status.h:40)  
**Lines:** 40–41  
**Severity:** LOW (misleading names)

```c
CAPT_FL_JOBSTAT_CHNG = _FL(0, 9),    // byte 2 bit 0x02 = needGetInputStatus
CAPT_FL_XSTATUS_CHNG = _FL(0, 8),    // byte 2 bit 0x01 = ExtendedStatus changed
```

These are semantically correct but named obscurely. Protocol §2.4 byte 2:
- 0x01 = "Extended status changed — call GetExtendedStatus again"
- 0x02 = "needGetInputStatus — paper tray state changed, call GetInputStatus"

**Fix:** Rename to `CAPT_FL_XSTATUS_CHANGED` and `CAPT_FL_NEED_INPUT_STATUS`.

---

### ISSUE-08 · `CAPT_SET_PARMS` (0xD0A9) Should Be `CAPT_MultiCommand`
**File:** [`src/capt-command.h`](src/capt-command.h:49)  
**Line:** 49  
**Severity:** MEDIUM (misleading name obscures protocol role)

```c
CAPT_SET_PARMS = 0xD0A9,   // current name
```

Protocol §2.13: `0xD0A9` is the **`CAPT_MultiCommand`** container command that wraps `D0A0 + D0A4 + D0A1 + D0A2` as a page announcement block. The name `CAPT_SET_PARMS` suggests it only sets parameters, hiding that it is a multi-command container.

**Fix:** Rename to `CAPT_MultiCommand`.

---

### ISSUE-09 · BufLevel (Bytes 5-6 of GetBasicStatus) Never Read or Used
**Files:** [`src/capt-status.c`](src/capt-status.c:53), [`src/generic-ops.c`](src/generic-ops.c:40)  
**Severity:** CRITICAL (large pages will overflow printer buffer and cause deadlock)

[`decode_status()`](src/capt-status.c:53) reads 40 bytes of GetExtendedStatus response but never reads bytes 5-6 (BufLevel). Neither [`capt_status_s`](src/capt-status.h:24) nor any caller stores or uses BufLevel.

Per protocol §2.18 and §9 item 3:
> BufLevel (GetBasicStatus bytes 5-6) is the **primary back-pressure mechanism**. When BufLevel == 0 with pending chunks, issue `StartPrint(N)`, poll until BufLevel ≥ 1, send remaining chunks, then `IC_BLACK_END`.

The current polling in [`generic-ops.c`](src/generic-ops.c:47) only waits every 16 chunks:
```c
if (state->isend % 16 == 0)
    capt_wait_ready();
```
`capt_wait_ready()` polls `CAPT_FL_BUSY` (which is the ERROR_BIT — see ISSUE-02), not BufLevel. Pages requiring > 2 MB compressed data (hardtocompress captures show ~2.05–2.24 MB) will overflow the 2 MB printer buffer.

**Fix:**
1. Add `uint16_t buf_level;` to [`capt_status_s`](src/capt-status.h:24).
2. Parse bytes 5-6 in [`decode_status()`](src/capt-status.c:53): `status.buf_level = WORD(s[4], s[5]);`
3. In [`ops_send_band_hiscoa()`](src/generic-ops.c:40), implement BufLevel polling:
   - After each chunk, check BufLevel from `GetBasicStatus`.
   - Reduce burst when BufLevel drops (see streaming mode sequence in protocol §2.18a).
   - When BufLevel == 0 with pending chunks: call `StartPrint(N)` immediately, then poll until BufLevel ≥ 1.
4. Pass the current page number into `ops_send_band_hiscoa` so `StartPrint` can be called with correct arg.
5. After all chunks sent (BufLevel ≥ 1), send `IC_BLACK_END`.

---

### ISSUE-10 · IC_BEGIN_PAGE Field Byte-Shift: `media_type` at PaperType, `media_adapt` at ResFlag
**File:** [`src/prn_lbp2900.c`](src/prn_lbp2900.c:279)  
**Lines:** 279–297  
**Severity:** HIGH (wrong resolution code sent, wrong media type encoding)

The `pageparms[]` array in `lbp2900_page_prologue()`:
```c
uint8_t pageparms[] = {
    0x00, 0x00, 0x30, 0x2A, sz, 0x00, 0x00, 0x00,
    ink_k, 0x1C, 0x1C, 0x1C, dims->media_type, dims->media_adapt, 0x04, 0x00,
    ...
```

0-indexed positions 12 and 13 (protocol bytes 13 and 14):
- **Position 12 (byte 13) = `dims->media_type`** — but protocol §2.14 says this is `PaperType` (result of `special_mode_for_papertype()`): `0x00`=Plain, `0x20`=Envelope, `0x24`=Transparency. The code puts the raw `media_type` enum value (0=plain, 1=thick, 3=thick-H, 4=transparency, 6=envelope) here directly without conversion.
- **Position 13 (byte 14) = `dims->media_adapt`** — but protocol §2.14 says this is **`ResFlag`**, a *constant* `0x11` for 600 dpi on LBP3000. The `media_adapt` field is unrelated.

**Fix:**
```c
uint8_t paper_type = 0x00;  // special_mode_for_papertype() conversion
switch (dims->media_type) {
    case 4: paper_type = 0x24; break;  // Transparency
    case 6: paper_type = 0x20; break;  // Envelope
    default: paper_type = 0x00;        // Plain / Thick uses 0x00
}
// Position 12 = paper_type, Position 13 = 0x11 (600 dpi constant for LBP3000)
```

---

### ISSUE-11 · TonerDensity Bytes Inconsistent: First Byte Is `ink_k`, Rest Are `0x1C`
**File:** [`src/prn_lbp2900.c`](src/prn_lbp2900.c:281)  
**Line:** 281  
**Severity:** MEDIUM (non-uniform toner density, differs from protocol default)

```c
ink_k, 0x1C, 0x1C, 0x1C,    // positions 8-11 = TonerDensity bytes 9-12
```

Where `ink_k = dims->ink_k << 2`. Protocol §2.14:
> TonerDensity: 4 bytes; for mono LBP3000 **all 4 bytes equal**. Default: `0x1f`.

The first byte varies with `ink_k` while the other three are hardcoded to `0x1C`. The default `0x1f` encodes density=7 in bits 5-2. Setting all four bytes to the same computed density value (or using `0x1f` default) is correct.

**Fix:**
```c
uint8_t td = (dims->ink_k > 0) ? (uint8_t)(dims->ink_k << 2) : 0x1f;
// pageparms positions 8-11:
td, td, td, td,
```

---

### ISSUE-12 · `GoOffline` Called Unconditionally in `lbp3000_job_prologue`
**File:** [`src/prn_lbp2900.c`](src/prn_lbp2900.c:169)  
**Line:** 169  
**Severity:** HIGH (can deadlock printer already in offline state)

```c
capt_sendrecv(CAPT_GoOffline, lbp3000_job_init, ARRAY_SIZE(lbp3000_job_init), NULL, 0);
```

Protocol §3.1.1 note and §9 item 4:
> GoOffline is only needed when the printer is currently **online** (Bas byte 1 bit 0x10 is **clear**). If already offline (0x10 set), go directly to ClearMisPrint/ClearError/DiscardData.

The LBP3000 prologue calls `GoOffline` without checking the printer's current online/offline state, which can cause issues if the printer starts up in offline state (common on second job).

**Fix:** Check status before calling GoOffline:
```c
const struct capt_status_s *s = lbp2900_get_status(state->ops);
if (!FLAG(s, CAPT_FL_OFFLINE)) {
    capt_sendrecv(CAPT_GoOffline, lbp3000_job_init, ARRAY_SIZE(lbp3000_job_init), NULL, 0);
}
capt_sendrecv(CAPT_ClearMisPrint, NULL, 0, NULL, 0);
capt_sendrecv(CAPT_ClearError, NULL, 0, NULL, 0);
capt_sendrecv(CAPT_DiscardData, NULL, 0, NULL, 0);
capt_sendrecv(CAPT_GoOnline, magicbuf_2, ARRAY_SIZE(magicbuf_2), NULL, 0);
```

Also: the current `lbp3000_job_prologue` sends `GoOffline` **after** `send_job_start(1, 0)`, which is incorrect. GoOffline must come **before** the ClearMisPrint/GoOnline sequence and before SetJobInfo2(flag=1). Protocol §3.1.1 order: `ReserveUnit → SetJobInfo2(flag=1) → GoOffline → ClearMisPrint → ClearError → DiscardData → GoOnline`.

---

### ISSUE-13 · `lbp2900_job_epilogue` Sends JobFlag=4 (Abort) Instead of Flag=6 (End)
**File:** [`src/prn_lbp2900.c`](src/prn_lbp2900.c:375)  
**Line:** 375  
**Severity:** HIGH (printer may not cleanly close the job)

```c
send_job_start(4, status->page_completed);
```

Protocol §2.7: `JobFlag=4` is documented as "job abort (unconfirmed)". `JobFlag=6` is the Windows canonical job-end marker. `JobFlag=3` is the Linux end marker.

`lbp2900_job_epilogue()` is called at the end of a **normal** (non-cancelled) job, so it should send `flag=6` (Windows) or at minimum `flag=3` (Linux). `flag=4` is reserved for abort/cancel.

Additionally: `lbp2900_cancel_cleanup()` at line 405 also sends `flag=4`, which is more appropriate for cancellation but still unconfirmed.

**Fix:**
```c
// In lbp2900_job_epilogue (normal end):
send_job_start(6, status->page_completed);   // Windows end-of-job flag

// In lbp2900_cancel_cleanup / lbp3010_cancel_cleanup (cancel):
send_job_start(4, status->page_completed);   // abort flag (keep as-is, or use 3)
```

---

### ISSUE-14 · `send_job_start` Uses Hardcoded TZOffset Instead of Real Timezone
**File:** [`src/prn_lbp2900.c`](src/prn_lbp2900.c:109)  
**Lines:** 113–114  
**Severity:** MEDIUM (job metadata incorrect for non-UTC+1 / UTC+2 timezones)

```c
/*-60 */ 0xC4, 0xFF,   // TZOffset = -60 minutes = UTC-1?
/*-120*/ 0x88, 0xFF,   // TZOffset2 = -120 minutes = UTC-2?
```

The comment labels suggest UTC-1 and UTC-2 but the values are actually `uint16_t` in little-endian:
- `0xFFC4` = -60 as a signed int16, but the protocol says TZOffset is "UTC offset in minutes relative to local time". A value of -60 would mean UTC+1. -120 = UTC+2.

These are hardcoded. Protocol §2.7: `TZOffset` should reflect the host's actual UTC offset.

**Fix:** Use `localtime()` result to compute:
```c
time_t t = time(NULL);
struct tm tm_local, tm_utc;
localtime_r(&t, &tm_local);
gmtime_r(&t, &tm_utc);
int tz_offset_min = (int)(mktime(&tm_local) - mktime(&tm_utc)) / 60;
int16_t tz = (int16_t)tz_offset_min;
```

---

### ISSUE-15 · `send_job_start` Sends No Hostname/Username/Jobname Strings
**File:** [`src/prn_lbp2900.c`](src/prn_lbp2900.c:101)  
**Lines:** 103–121  
**Severity:** MEDIUM (job metadata missing; mirrors known Linux driver bug)

```c
uint8_t ml = 0x00; /* host name length */
uint8_t ul = 0x00; /* user name length */
uint8_t nl = 0x00; /* document name length */
```

All three string lengths are zeroed — no hostname, username, or job name is ever sent. Protocol §2.7 shows the Windows driver sends UTF-16LE strings for all three fields. The protocol notes this as a known Linux driver bug where lengths are non-zero but no string data follows; in this open-source driver even the lengths are zero.

CUPS provides `argv[2]` (user), `argv[3]` (title/job name), and the hostname can be obtained via `gethostname()`. The `rastertocapt.c` main() receives these arguments but never passes them to the printer ops layer.

**Fix (full):**
1. Add `const char *username`, `const char *jobname`, `const char *hostname` fields to [`printer_state_s`](src/printer.h:34).
2. Populate from `argv[]` in [`rastertocapt.c:main()`](src/rastertocapt.c:340).
3. In `send_job_start()`, encode as UTF-16LE and append to the payload with correct `HostLen`/`UserLen`/`JobNameLen` fields.

**Minimal fix** (fixes the length-string mismatch that produces malformed packets):
- Keep strings empty but ensure all three `*Len` fields remain 0x00.
- The current code already does this (all zero), so no immediate breakage — but the feature is absent.

---

### ISSUE-16 · `send_job_start` Sends SetJobInfo2(flag=2) as a Per-Page Heartbeat
**File:** [`src/prn_lbp2900.c`](src/prn_lbp2900.c:330)  
**Lines:** 330–365  
**Severity:** MEDIUM (wrong semantics for flag=2)

`lbp2900_page_epilogue()` calls `send_job_start(2, ...)` inside the per-page completion loop:
```c
send_job_start(2, status->page_decoding);
```

Protocol §2.7 and §2.18a:
> `SetJobInfo2(flag=2)` should be sent **once** when `GetExtendedStatus.Printed` first becomes ≥ 1 (mid-job continuation). The Linux driver incorrectly sends it as a periodic heartbeat; the Windows driver only sends it once.

The open-source driver sends it on every `page_epilogue` call (every page) rather than just once.

**Fix:** Track whether `flag=2` has been sent. Add `bool mid_job_sent` to `printer_state_s`. Send `flag=2` once when `status->page_completed >= 1` and `!state->mid_job_sent`, then set `state->mid_job_sent = true`.

---

### ISSUE-17 · `lbp2900_page_epilogue` Flow Control: Waits on `page_received == page_decoding` Before StartPrint
**File:** [`src/prn_lbp2900.c`](src/prn_lbp2900.c:337)  
**Lines:** 337–343  
**Severity:** HIGH (wrong wait condition; `page_received` only exists in extended status)

```c
while (1) {
    sleep(1);
    status = lbp2900_get_status(state->ops);
    if (status->page_received == status->page_decoding)
        break;
}
```

`page_received` is parsed from byte offset 34-35 of the extended status in `decode_status()`. But `lbp2900_get_status()` calls `capt_get_xstatus()` which calls `GetBasicStatus` + optionally `GetExtendedStatus`. The `page_received` field is only valid when `GetExtendedStatus` was called.

More importantly: per protocol §2.20, `StartPrint(N)` should be sent **after `IC_BLACK_END`** in normal mode, and the page number passed should match `GetExtendedStatus.Start`. The current code sends `StartPrint` with `status->page_decoding` after waiting for `page_received == page_decoding`, which is a different condition.

The `page_received` field appears to correspond to protocol's `Start` counter. The correct wait is for `GetExtendedStatus.Start == N` (page N's descriptor accepted) before sending `StartPrint(N)` per protocol §3.1.1.

**Fix:** Align the wait condition and StartPrint argument with protocol §2.20:
```c
// After sending IC_BLACK_END:
// Wait for Start counter to confirm page N was accepted
while (1) {
    status = lbp2900_get_status(state->ops);
    if (status->page_decoding >= target_page)  // page_decoding = Start counter
        break;
    sleep(1);
}
// Then StartPrint with the page's Start value
uint8_t buf[2] = { LO(status->page_decoding), HI(status->page_decoding) };
capt_sendrecv(CAPT_StartPrint, buf, 2, NULL, 0);
```

---

### ISSUE-18 · `ReserveUnit` Payload Not All-Zeros
**File:** [`src/prn_lbp2900.c`](src/prn_lbp2900.c:52)  
**Lines:** 52–54  
**Severity:** MEDIUM (non-conformant payload)

```c
static const uint8_t magicbuf_0[] = {
    0x00, 0x00, 0x1E, 0x00, 0x00, 0x00, 0x00, 0x00
};
```

Byte 2 (offset 2, 0-indexed) = `0x1E`. Protocol §2.5:
> **Request (8 bytes, all zeros):** `00 00 00 00 00 00 00 00`

The comment "magicbuf_0" suggests this was reverse-engineered as a magic value, but the protocol documentation clarifies it should be all zeros.

**Fix:** Change to all zeros:
```c
static const uint8_t reserveunit_payload[8] = { 0 };
```

---

### ISSUE-19 · `CAPT_LBP6000_SETUP_0` and `CAPT3_UNK_0` Duplicate `0xE0BA`
**File:** [`src/capt-command.h`](src/capt-command.h:59)  
**Lines:** 59, 63  
**Severity:** LOW (duplicate enum values cause compiler warnings; confusing)

```c
CAPT_LBP6000_SETUP_0 = 0xE0BA,
// ...
CAPT3_UNK_0     = 0xE0BA,     // same value!
```

**Fix:** Remove `CAPT3_UNK_0` — it is a dead alias. If 0xE0BA is used for LBP6000 setup, keep only `CAPT_LBP6000_SETUP_0`.

---

### ISSUE-20 · `lbp3010_cancel_cleanup` Has Duplicate `(void) state;`
**File:** [`src/prn_lbp2900.c`](src/prn_lbp2900.c:411)  
**Lines:** 411–412  
**Severity:** LOW (cosmetic / dead code)

```c
static void lbp3010_cancel_cleanup(struct printer_state_s *state)
{
    (void) state;
    (void) state;   // duplicate
```

**Fix:** Remove the duplicate line.

---

### ISSUE-21 · `capt_sendrecv` Two-Packet Reply Split Not Robustly Handled
**File:** [`src/capt-command.c`](src/capt-command.c:146)  
**Lines:** 156–172  
**Severity:** MEDIUM (may fail on firmware that always splits > 6-byte replies)

```c
capt_recv_buf(0, 6);  // reads exactly 6 bytes first
// ...
if (WORD(capt_iobuf[2], capt_iobuf[3]) > capt_iosize && capt_iosize % 64 == 6) {
    capt_recv_buf(capt_iosize, WORD(...) - capt_iosize);
```

Protocol §1 (Packet Framing):
> Replies longer than 6 bytes arrive as **two packets**: one 6-byte packet followed by the remainder. The driver must do two reads.

The current code only reads the remainder when `capt_iosize % 64 == 6` (a 64-byte boundary condition from USB). Protocol says this split happens for **all replies > 6 bytes**, not just at 64-byte boundaries. The condition should simply be: if expected size > 6, read the remainder.

**Fix:**
```c
capt_recv_buf(0, 6);   // first packet always 6 bytes
uint16_t expected = WORD(capt_iobuf[2], capt_iobuf[3]);
// Handle BCD-encoded size (firmware bug)
if (expected < 6) expected = BCD(capt_iobuf[2], capt_iobuf[3]);
if (expected > 6) {
    capt_recv_buf(6, expected - 6);   // second packet: remainder
}
// Validate final size matches
```

---

### ISSUE-22 · `capt_wait_ready` Uses `GetBasicStatus`; Should Use `GetExtendedStatus` for LBP3010/LBP6000
**File:** [`src/capt-status.c`](src/capt-status.c:122)  
**Lines:** 122–126  

```c
void capt_wait_ready(void)
{
    while (FLAG(capt_get_status(), CAPT_FL_BUSY))   // CAPT_FL_BUSY = ERROR_BIT, wrong
        sleep(1);
}
```

LBP3010/LBP6000 use `capt_wait_xready_only()` (which correctly calls `GetExtendedStatus`) but LBP2900/LBP3000 use `capt_wait_ready()` which uses `GetBasicStatus` and checks the wrong bit (see ISSUE-02).

**Fix (after ISSUE-02 is resolved):** Replace `CAPT_FL_BUSY` with `CAPT_FL_CMD_BUSY` in `capt_wait_ready`. Note: `CAPT_FL_BUSY` at `_FL(0,7)` = ERROR_BIT means the driver currently stops waiting when there is **no error**, which accidentally works in normal conditions but fails to detect when the printer is genuinely busy with a command.

---

### ISSUE-23 · `debug` Macro Always True — Floods stderr in Production
**File:** [`src/std.h`](src/std.h:33)  
**Line:** 33  
**Severity:** LOW (performance / log noise)

```c
#define debug true
```

`debug` is used in `capt_send_buf()` and `capt_sendrecv()` to dump every send/receive buffer to stderr. This is hardcoded on, producing gigabytes of output for large print jobs.

**Fix:** Make `debug` conditional:
```c
#ifdef CAPT_DEBUG
# define debug true
#else
# define debug false
#endif
```

Or use a runtime flag, or change `debug` from a macro to a `const bool`.

---

### ISSUE-24 · `captdefilter.c` Assumes `IC_BEGIN_PAGE` LineSize at Fixed Offset 26
**File:** [`tests/captdefilter.c`](tests/captdefilter.c:132)  
**Line:** 132  
**Severity:** LOW (works for LBP3000 but fails for older 34-byte format)

```c
case 0xD0A0:
    line_size = WORD(buf[26], buf[27]);
```

Protocol §2.14: LineSize is at bytes 27-28 (1-indexed) = indices 26-27 (0-indexed) — correct for LBP3000 (40-byte payload). Older models use a 34-byte payload where bytes 27-28 are still `LineSize`, so this is actually fine for the current printer models. But the code does not validate that the packet is long enough, nor does it note the model dependency.

**Minor fix:** Add size check before accessing `buf[26]`:
```c
if (size >= 28) line_size = WORD(buf[26], buf[27]);
```

---

### ISSUE-25 · Missing Post-Job Drain: Alternate `GetInputStatus` + `GetExtendedStatus`
**File:** [`src/prn_lbp2900.c`](src/prn_lbp2900.c:368)  
**Lines:** 368–381  
**Severity:** MEDIUM (incomplete job lifecycle; printer may not cleanly release)

`lbp2900_job_epilogue()` waits for `page_completed == page_decoding` then sends `ReleaseUnit`. Protocol §3.1.1 and §9 item 7:
> After `ReleaseUnit`, alternate `GetInputStatus` + `GetExtendedStatus` until `Printed == totalPages`.

The current code sends `ReleaseUnit` and returns without doing any post-job drain. This means the driver exits while the printer may still be physically ejecting the last page.

**Fix:**
```c
capt_sendrecv(CAPT_ReleaseUnit, jbuf, 2, NULL, 0);
// Post-job drain: alternate GetInputStatus + GetExtendedStatus
while (1) {
    const struct capt_status_s *s;
    capt_sendrecv(CAPT_GetInputStatus, NULL, 0, NULL, 0);
    s = capt_get_xstatus_only();
    if (s->page_completed >= total_pages)
        break;
    sleep(1);
}
```
This requires `total_pages` to be tracked in `printer_state_s`.

---

## 3. Summary of Renames Required

| Old Name | New Name | File | Protocol Reference |
|----------|----------|------|--------------------|
| `CAPT_SET_PARMS` | `CAPT_MultiCommand` | [`capt-command.h:49`](src/capt-command.h:49) | §2.13 `0xD0A9` |
| `CAPT3_UNK_0` | *(remove — duplicate of `CAPT_LBP6000_SETUP_0`)* | [`capt-command.h:63`](src/capt-command.h:63) | — |
| `CAPT_FL_BUSY` | `CAPT_FL_ERROR` | [`capt-status.h:42`](src/capt-status.h:42) | `ERROR_BIT` 0x80 |
| `CAPT_FL_UNINIT1` | *(remove or map to 0x20 = undefined; likely dead)* | [`capt-status.h:43`](src/capt-status.h:43) | — |
| `CAPT_FL_UNINIT2` | `CAPT_FL_OFFLINE` | [`capt-status.h:44`](src/capt-status.h:44) | `RCF_OFFLINE` 0x10 |
| `CAPT_FL_BUFFERFULL` | `CAPT_FL_CMD_BUSY` | [`capt-status.h:45`](src/capt-status.h:45) | `CMD_BUSY` 0x04 |
| `CAPT_FL_NOPAPER1` | `CAPT_FL_NOTREADY` | [`capt-status.h:46`](src/capt-status.h:46) | `RCF_NOTREADY` 0x02 |
| `CAPT_FL_PROCESSING` | `CAPT_FL_PRINTER_FREE` | [`capt-status.h:47`](src/capt-status.h:47) | `RCF_PRINTER_FREE` 0x01 |
| `CAPT_FL_XSTATUS_CHNG` | `CAPT_FL_XSTATUS_CHANGED` | [`capt-status.h:41`](src/capt-status.h:41) | §2.4 byte 2 bit 0x01 |
| `CAPT_FL_JOBSTAT_CHNG` | `CAPT_FL_NEED_INPUT_STATUS` | [`capt-status.h:40`](src/capt-status.h:40) | §2.4 byte 2 bit 0x02 |
| `magicbuf_0` | `reserveunit_payload` | [`prn_lbp2900.c:52`](src/prn_lbp2900.c:52) | §2.5 |
| `magicbuf_2` | `goonline_payload` | [`prn_lbp2900.c:56`](src/prn_lbp2900.c:56) | §2.12 |
| `lbp3000_job_init` | `gooffline_payload_zeros` | [`prn_lbp2900.c:71`](src/prn_lbp2900.c:71) | §2.8 |
| `lbp6000_job_init` | `lbp6000_setup_payload` | [`prn_lbp2900.c:85`](src/prn_lbp2900.c:85) | — |

**New constants / flags to add to `capt-status.h`:**

| New Name | Bit (byte 1) | Value | Protocol |
|----------|--------------|-------|----------|
| `CAPT_FL_PRINTER_FREE` | 0 | 0x01 | `RCF_PRINTER_FREE` §2.4 |
| `CAPT_FL_NOTREADY` | 1 | 0x02 | `RCF_NOTREADY` §2.4 |
| `CAPT_FL_CMD_BUSY` | 2 | 0x04 | `CMD_BUSY` §2.4 |
| `CAPT_FL_IM_DATA_BUSY` | 3 | 0x08 | `IM_DATA_BUSY` §2.4 |
| `CAPT_FL_OFFLINE` | 4 | 0x10 | `RCF_OFFLINE` §2.4 |
| `CAPT_FL_UNIT_FREE` | 6 | 0x40 | `UNIT_FREE` §2.4 |
| `CAPT_FL_ERROR` | 7 | 0x80 | `ERROR_BIT` §2.4 |

**New field to add to `capt_status_s`:**

| New Field | Type | Source | Protocol |
|-----------|------|--------|----------|
| `buf_level` | `uint16_t` | GetBasicStatus bytes 5-6 | §2.4, §2.18 |

---

## 4. Implementation Priority Order

Issues are grouped by the risk they pose to correct printing, with CRITICAL and HIGH issues blocking correct operation.

### Phase 1 — Fix Compile / Crash Bugs (do first, unblocks testing)

| Priority | Issue | File | Change |
|----------|-------|------|--------|
| P1 | ISSUE-01 | [`rastertocapt.c:352`](src/rastertocapt.c:352) | `do_cancel()` → `do_cancel` (remove call parens) |
| P1 | ISSUE-19 | [`capt-command.h:63`](src/capt-command.h:63) | Remove duplicate `CAPT3_UNK_0` enum value |
| P1 | ISSUE-20 | [`prn_lbp2900.c:412`](src/prn_lbp2900.c:412) | Remove duplicate `(void) state;` |

### Phase 2 — Fix Status Flag Definitions (prerequisite for all flow-control fixes)

All of these are in [`src/capt-status.h`](src/capt-status.h). Do the renames first so all subsequent callers can use correct flag names.

| Priority | Issue | Old Name → New Name | Bit | Value |
|----------|-------|---------------------|-----|-------|
| P2 | ISSUE-02 | `CAPT_FL_BUSY` → `CAPT_FL_ERROR` | 7 | 0x80 |
| P2 | ISSUE-03 | `CAPT_FL_BUFFERFULL` → `CAPT_FL_CMD_BUSY` | 2 | 0x04 |
| P2 | ISSUE-04 | `CAPT_FL_UNINIT2` → `CAPT_FL_OFFLINE` | 4 | 0x10 |
| P2 | ISSUE-04 | `CAPT_FL_UNINIT1` → remove or keep as `CAPT_FL_UNK20` | 5 | 0x20 |
| P2 | ISSUE-05 | `CAPT_FL_NOPAPER1` → `CAPT_FL_NOTREADY` | 1 | 0x02 |
| P2 | ISSUE-06 | `CAPT_FL_PROCESSING` → `CAPT_FL_PRINTER_FREE` | 0 | 0x01 |
| P2 | ISSUE-07 | `CAPT_FL_XSTATUS_CHNG` → `CAPT_FL_XSTATUS_CHANGED` | 8 | — |
| P2 | ISSUE-07 | `CAPT_FL_JOBSTAT_CHNG` → `CAPT_FL_NEED_INPUT_STATUS` | 9 | — |
| P2 | ISSUE-03 | *Add* `CAPT_FL_IM_DATA_BUSY` | 3 | 0x08 |
| P2 | ISSUE-03 | *Add* `CAPT_FL_UNIT_FREE` | 6 | 0x40 |

After Phase 2, update all usages:
- [`capt-status.c:117`](src/capt-status.c:117): `CAPT_FL_XSTATUS_CHNG` → `CAPT_FL_XSTATUS_CHANGED`
- [`capt-status.c:124`](src/capt-status.c:124): `CAPT_FL_BUSY` → `CAPT_FL_CMD_BUSY`
- [`capt-status.c:130`](src/capt-status.c:130): same in `capt_wait_xready()`
- [`prn_lbp2900.c:302`](src/prn_lbp2900.c:302): `CAPT_FL_UNINIT1 || CAPT_FL_UNINIT2` → `CAPT_FL_OFFLINE`
- [`prn_lbp2900.c:314`](src/prn_lbp2900.c:314): `CAPT_FL_BUFFERFULL` → remove (replace with BufLevel check — Phase 3)
- [`prn_lbp2900.c:358`](src/prn_lbp2900.c:358): `CAPT_FL_NOPAPER2 || CAPT_FL_NOPAPER1` — review after rename

### Phase 3 — Fix Protocol-Critical Flow Control and IC_BEGIN_PAGE

| Priority | Issue | File | Change |
|----------|-------|------|--------|
| P3 | ISSUE-09 | [`capt-status.h`](src/capt-status.h:24), [`capt-status.c:53`](src/capt-status.c:53), [`generic-ops.c:40`](src/generic-ops.c:40) | Add `buf_level` field; parse bytes 5-6; implement BufLevel-based back-pressure in `ops_send_band_hiscoa` |
| P3 | ISSUE-10 | [`prn_lbp2900.c:279`](src/prn_lbp2900.c:279) | Fix IC_BEGIN_PAGE field mapping: PaperType (convert) at byte 13, `0x11` constant at byte 14 (ResFlag) |
| P3 | ISSUE-11 | [`prn_lbp2900.c:281`](src/prn_lbp2900.c:281) | Uniform TonerDensity: all 4 bytes the same value |
| P3 | ISSUE-12 | [`prn_lbp2900.c:146`](src/prn_lbp2900.c:146) | `lbp3000_job_prologue`: make GoOffline conditional on current online/offline state; fix ordering (GoOffline before GoOnline init sequence) |
| P3 | ISSUE-13 | [`prn_lbp2900.c:375`](src/prn_lbp2900.c:375) | `lbp2900_job_epilogue`: send flag=6 (normal end), not flag=4 (abort) |
| P3 | ISSUE-17 | [`prn_lbp2900.c:337`](src/prn_lbp2900.c:337) | Fix wait condition before `StartPrint`; use `page_decoding` (Start counter) correctly |
| P3 | ISSUE-21 | [`capt-command.c:156`](src/capt-command.c:156) | Fix two-packet reply split: read remainder whenever expected > 6, not only at 64-byte boundary |

### Phase 4 — Fix Job Lifecycle and Metadata

| Priority | Issue | File | Change |
|----------|-------|------|--------|
| P4 | ISSUE-08 | [`capt-command.h:49`](src/capt-command.h:49) | Rename `CAPT_SET_PARMS` → `CAPT_MultiCommand`; update all call sites |
| P4 | ISSUE-16 | [`prn_lbp2900.c:344`](src/prn_lbp2900.c:344) | Send `flag=2` only once (when `page_completed >= 1`), not per page |
| P4 | ISSUE-18 | [`prn_lbp2900.c:52`](src/prn_lbp2900.c:52) | Zero out `magicbuf_0` → `reserveunit_payload[8] = {0}` |
| P4 | ISSUE-25 | [`prn_lbp2900.c:368`](src/prn_lbp2900.c:368) | Add post-job drain loop after `ReleaseUnit` |
| P4 | ISSUE-14 | [`prn_lbp2900.c:113`](src/prn_lbp2900.c:113) | Compute real TZOffset from system timezone |
| P4 | ISSUE-15 | [`prn_lbp2900.c:101`](src/prn_lbp2900.c:101) | Send hostname/username/jobname strings from CUPS argv |

### Phase 5 — Polish and Debug

| Priority | Issue | File | Change |
|----------|-------|------|--------|
| P5 | ISSUE-22 | [`capt-status.c:122`](src/capt-status.c:122) | `capt_wait_ready` uses correct flag after Phase 2 rename |
| P5 | ISSUE-23 | [`std.h:33`](src/std.h:33) | Guard `debug` with `#ifdef CAPT_DEBUG` |
| P5 | ISSUE-24 | [`tests/captdefilter.c:132`](tests/captdefilter.c:132) | Add size check before reading `buf[26]` |

---

## 5. Additional New Flags Needed in `capt_status_s`

Beyond the `buf_level` field, the following fields are present in `decode_status()` output but not mapped to named constants or used in flow control:

| Field | Source bytes (decode_status offset) | Protocol field | Current status |
|-------|--------------------------------------|----------------|----------------|
| `buf_level` | bytes 5-6 of GetBasicStatus reply | BufLevel | **NOT decoded at all** — must add |
| `page_received` | s[34], s[35] | `Start` (GetExtendedStatus bytes 15-16) | decoded but misnamed (it's the `Start` counter, not "received") |
| `page_decoding` | s[14], s[15] | `Start` counter | decoded; naming suggests "start" |
| `page_printing` | s[16], s[17] | `Printing` counter | decoded correctly |
| `page_out` | s[18], s[19] | `Shipped` counter | decoded; "out" means "shipped/ejected" |
| `page_completed` | s[20], s[21] | `Printed` counter | decoded correctly |

**Rename recommendation for `capt_status_s` fields:**

| Old Name | New Name | Protocol Name |
|----------|----------|---------------|
| `page_decoding` | `page_start` | `Start` (page descriptor accepted) |
| `page_printing` | `page_printing` | `Printing` (keep) |
| `page_out` | `page_shipped` | `Shipped` (being ejected) |
| `page_completed` | `page_printed` | `Printed` (fully done) |
| `page_received` | *(field at s[34-35])* | Unclear — may be a duplicate or secondary Start counter; review against protocol bytes 35-36 |

The field at `s[34], s[35]` (decoded as `page_received`) corresponds to bytes 35-36 (1-indexed) of `GetExtendedStatus`, which per `lbp3000-protocol.md` §2.2 is not explicitly labelled in the 52-byte response layout shown. This should be verified against a protocol capture before renaming.

---

## 6. Issue Cross-Reference by File

| File | Issues |
|------|--------|
| [`src/capt-command.h`](src/capt-command.h) | ISSUE-08, ISSUE-19 |
| [`src/capt-command.c`](src/capt-command.c) | ISSUE-21 |
| [`src/capt-status.h`](src/capt-status.h) | ISSUE-02, ISSUE-03, ISSUE-04, ISSUE-05, ISSUE-06, ISSUE-07, ISSUE-09 (add field) |
| [`src/capt-status.c`](src/capt-status.c) | ISSUE-09 (parse BufLevel), ISSUE-22 |
| [`src/generic-ops.c`](src/generic-ops.c) | ISSUE-09 (BufLevel back-pressure) |
| [`src/prn_lbp2900.c`](src/prn_lbp2900.c) | ISSUE-10, ISSUE-11, ISSUE-12, ISSUE-13, ISSUE-14, ISSUE-15, ISSUE-16, ISSUE-17, ISSUE-18, ISSUE-20, ISSUE-25 |
| [`src/rastertocapt.c`](src/rastertocapt.c) | ISSUE-01 |
| [`src/std.h`](src/std.h) | ISSUE-23 |
| [`tests/captdefilter.c`](tests/captdefilter.c) | ISSUE-24 |
| [`tests/hiscoa-decompress.c`](tests/hiscoa-decompress.c) | None — no protocol-level issues found |
| [`src/printer.h`](src/printer.h) | Needs new fields: `total_pages`, `mid_job_sent` (for ISSUE-16, ISSUE-25) |
| [`src/word.h`](src/word.h) | None — `WORD()`, `HI()`, `LO()`, `BCD()` are all correct |