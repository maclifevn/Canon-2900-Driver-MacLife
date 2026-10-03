# Canon LBP3000 CAPT Protocol Reference

> Derived from USB captures (`lbp3000-windowsxp.pcapng`, `lbp3000-windowsxp-4pages-outofpaper-after-2-then-reprint.pcapng`, `lbp3000-windowsxp-hardtocompress.pcapng`, `lbp3000-windowsxp-hardtocompress2.pcapng`, `lbp3000-windowsxp-hardtocompress4.pcapng`) and reverse-engineering notes in `info/`. Linux CAPT driver behaviour is documented separately from Windows XP driver behaviour, derived from `lbp3000.pcap` / `parsed/lbp3000.txt`.
> All byte offsets are **1-based** (byte 1 = first byte of payload) unless otherwise stated.
> All multi-byte integers are **little-endian**.

---

## 1. Packet Framing

Every command and reply shares the same 4-byte header:

```
Offset  Size  Description
------  ----  -----------
0       2     Command code (uint16_t LE)
2       2     Total packet size including header (uint16_t LE)
4       …     Payload (0 or more bytes)
```

**Example** – command `0xD0A1` with no payload:

```
A1 D0 04 00
```

- Replies reuse the same command code as the request.
- If the driver expects a reply it **must** read it before issuing the next command; failure causes a printer deadlock.
- Replies longer than 6 bytes arrive as **two packets**: one 6-byte packet followed by the remainder.
- Some printer firmware encodes the size field in BCD rather than binary (firmware bug — handle both).

---

## 2. Command Reference

### 2.1 `0xA1A1` — `CAPT_GetPrinterInfo`

**Direction:** host → printer (request has no payload), printer replies.

**Response (52 bytes observed):**

```
Bytes  Field        Notes
-----  -----        -----
1-2    Unk          00 0b
3-4    DevID        Device model ID (LE uint16). LBP3000 observed: 0x2a30 = 10800
5-6    Unk          01 01
7-8    Blk          Max single Hi-SCoA block size for IC_VIDEO_DATA (LE uint16).
                    Observed: 0xfff0 = 65520 bytes
9-10   Buf          Max buffer count (LE uint16). Observed: 0x0040 = 64.
                    Actual RAM = Buf << 15 = 2 097 152 bytes (2 MB)
11     PFlags       Printer flags. Observed: 0x04
12     NumC         Number of colour planes (0 = mono). Observed: 0x00
13-16  Unk
…      (52 bytes total; driver checks size > 55 to guard optional fields)
```

**Observed LBP3000 response:**

```
000b 302a 0101 f0ff 4000 0400 4000 0100
4803 0000 6f08 0000 e40d 0000 0000 0000
fa02 0000 f604 0000 283c 3232 5802 5802
1503 0200
```

**Driver usage:** Called once at startup. `Blk` is used to size each `IC_VIDEO_DATA` transfer; `Buf` determines how many blocks can be pre-queued.

---

### 2.2 `0xA0A8` — `CAPT_GetExtendedStatus`

**Direction:** host → printer (no payload), printer replies 52 bytes.

**Response layout:**

```
Bytes  Field    Notes
-----  -----    -----
1      Bas      Basic status flags (same as GetBasicStatus byte 1):
                  0x01  RCF_PRINTER_FREE    – printer unit is idle/free
                  0x02  RCF_NOTREADY        – not ready
                  0x04  CMD_BUSY            – processing command / printing
                  0x08  IM_DATA_BUSY        – data buffer full
                  0x10  RCF_OFFLINE         – printer offline
                  0x40  UNIT_FREE
                  0x80  ERROR_BIT
2      Bas1     Secondary flags:
                  0x01  Extended status changed – call GetExtendedStatus again
                  0x02  needGetInputStatus    – paper tray state changed, call GetInputStatus
3      Bas2     Unk
4      Unk
5-6    Buf      IC_VIDEO_DATA FIFO free space.
                  Formula: (Buf & 0xF) × (GetPrinterInfo_Buf << 15) / 15
                  Max value: 0x000f (15 slots). Observed constant 0x000f during normal printing.
7      Unk
8      (unused)
9      Aux      Auxiliary engine status:
                  0x02  RCF_PRINTER_BUSY
                  0x04  RCF_PAPER_DELIVERY  – paper is being physically fed/delivered
                  0x80  RCF_SAFE_TIMER      – motor running
10     Cnt      Engine communication flags:
                  0x01  RCF_OVERRUN
                  0x02  RCF_UNDERRUN
                  0x04  RCF_MISSING_EOP
                  0x08  RCF_INVALID_DATA
                  0x10  RCF_ENGINE_COMM_ERROR
                  0x20  RCF_ENGINE_RESET_INPROGRESS
                  0x40  RCF_PRINT_REJECTED
11     Pap      bPaperAvailableBits. Indicates whether the printer can currently feed paper.
                  NOTE: LBP3000 has NO paper-tray sensor. Paper-out is detected only when
                  the engine fails to pick paper. This field reflects the printer's internal
                  readiness state, not a physical sensor reading.
                  if (Pap & (0x80 >> slotIndex)) == 0 → engine reports paper-out condition
                  0x80 = engine ready / paper available (normal and post-button-press state)
                  0x00 = engine in paper-out error state
12     Cnt2     bControllerStatus2
13-14  Eng      Engine ready status flags:
                  0x0002  RCF_SERVICE_CALL
                  0x0004  RCF_CLEANING
                  0x0008  Illegal connection (check 0xA2A7)
                  0x0100  RCF_JAM (when IllegalConnectionStatus set)
                  0x2000  RCF_NO_CARTRIDGE
                  0x4000  RCF_DOOR_OPEN
15-16  Start    Page number currently being decoded/queued (uint16)
17-18  Printing Page number currently printing (uint16)
19-20  Shipped  Page number being ejected (uint16)
21-22  Printed  Page number fully completed (uint16)
23     (special) If 0x3e → alternate MediaTable used
25     LED      Internal status code (see §5)
27     Cov      (used only on DevID 2230 – not LBP3000)
37-40  HostErr  dwHostDetectError flags written by SetLEDStatus:
                  0x00000004  PCF_SETCLEANINGPAGE
                  0x00010000  no paper (int_status 9)
                  0x00008000  CNDataXferError (int_status 11)
                  0x00080000  CNChangePaperSize (int_status 12)
                  0x01000000  CNPaused (int_status 4)
```

See §4 for the full page-counter progression table.

**Driver polling strategy observed:**
1. Poll `GetBasicStatus` rapidly (no sleep between calls).
2. When `GetBasicStatus` byte 1 bit `0x08` is set (`IM_DATA_BUSY`), stop sending video data.
3. When `GetBasicStatus` byte 1 changes from previous value → call `GetExtendedStatus`.
4. When `GetExtendedStatus` Bas1 `& 0x02` → call `GetInputStatus`.
5. Continue polling until `Printed == totalPages`.

---

### 2.3 `0xA0A1` — `CAPT_GetInputStatus`

**Direction:** host → printer (no payload), printer replies 18 bytes.

**Response layout:**

```
Bytes  Field    Notes
-----  -----    -----
1      ErrBits  Error flags; non-zero means error
2      SlotCfg  Upper nibble (>> 4) = number of input slots − 1.
                  0x10 → 1, meaning 2 slots described.
                  If (byte2 & 0xF0) != 0, parse slot records following.
3      Flags    Per-slot flags (used in paper selection for DevID 2230)
4      Unk1
5      Unk2
6      PaperID  Paper size ID (matches tPaperSizeTbl second column):
                  0x02 = A4, 0x01 = A3, 0x03 = A5, 0x07 = B5 …
7-8    Width    Paper width in pixels at 300 dpi (uint16)
9-10   Height   Paper height in pixels at 300 dpi (uint16)
11-12  Unk3
13     Unk4
14     Type
15     Flags2
(repeat per slot)
```

> **LBP3000 hardware note:** This printer has **no paper-tray sensor**. Paper-out is detected only when the engine physically fails to pick a sheet. After the user inserts paper and presses the **physical go button** on the printer, the printer updates its state and sets `Bas1 & 0x02` to notify the driver to call `GetInputStatus`.

**Observed response during normal operation (engine ready):**

```
00 10 d0 00 00 02 b0 09 b3 0d 96 00 fd 01 00 00 00 00
```

- Byte 3 = `0xd0`
- PaperID = `0x02` = A4
- Width = `0x09b0` = 2480 pixels (8.267" × 300 dpi ✓)
- Height = `0x0db3` = 3507 pixels (11.69" × 300 dpi ✓)

**Observed response during paper-out error state:**

```
00 10 80 00 00 00 00 00 00 00 00 00 96 00 fd 00 00 00
```

- Byte 3 = `0x80`; PaperID and Width/Height fields are 0
- This reflects the engine error condition, NOT a tray sensor reading

**Observed response after user inserts paper and presses go button:**

```
00 10 c0 00 00 02 b0 09 b3 0d 96 00 fd 01 00 00 00 00
```

- Byte 3 = `0xc0`; paper size fields valid again — engine is ready to resume
- This state change is triggered by the **button press**, not by paper detection

**When to call:** On startup, and whenever `GetExtendedStatus` Bas1 bit `0x02` is set.

---

### 2.4 `0xE0A0` — `CAPT_GetBasicStatus`

**Direction:** host → printer (no payload), printer replies 8 bytes.

**Response layout:**

```
Bytes  Field    Notes
-----  -----    -----
1      Status   Flag mask:
                  0x01  RCF_PRINTER_FREE
                  0x02  RCF_NOTREADY
                  0x04  CMD_BUSY
                  0x08  IM_DATA_BUSY   – do not send more video data, not used on LBP3000
                  0x10  RCF_OFFLINE
                  0x40  UNIT_FREE
                  0x80  ERROR_BIT
2      Status2  0x01 = ExtendedStatus changed; 0x02 = InputStatus changed
3-4    Unk      (00 00 observed)
5-6    BufLevel FIFO free-slot count (same formula as GetExtendedStatus bytes 5-6)
7-8    Unk      (00 00 observed)
```

**Observed values during 4-page job:**

| Value (byte 1) | Meaning |
|----------------|---------|
| `0x31` | Offline + free |
| `0x30` | Offline, no free flag |
| `0x00` | Online, free, idle |
| `0x04` | CMD_BUSY (printing) |
| `0x05` | CMD_BUSY + PRINTER_FREE (transition) |
| `0x01` | PRINTER_FREE (job finishing) |

**Usage:** Called as tight polling loop. If byte 1 value changes, call `GetExtendedStatus` next.

**Out-of-paper detection values:**

| Value (byte 1) | Meaning |
|----------------|---------|
| `0x16` / `0x1e` | RCF_NOTREADY + RCF_OFFLINE — printer stopped (out of paper or error) |
| `0x12` | RCF_NOTREADY + RCF_OFFLINE (transitional, after error acknowledged) |
| `0x10` | RCF_OFFLINE only (clearing error state) |

---

### 2.5 `0xA2A0` — `CAPT_ReserveUnit`

**Direction:** host → printer.

**Request (8 bytes, all zeros):**

```
00 00 00 00 00 00 00 00
```

**Response (4 bytes):**

```
Bytes  Field   Notes
-----  -----   -----
1      Error   0 = success
2      Unk
3-4    JobID   Job identifier (uint16). Used in all subsequent SetJobInfo2 and ReleaseUnit calls.
```

**Observed:** `00 00 01 00` → JobID = 1.

**Usage:** Must be called at the start of every print job before sending any page data. Acquires exclusive access to the print engine.

---

### 2.6 `0xE0A9` — `CAPT_ReleaseUnit`

**Direction:** host → printer.

**Request (2 bytes):**

```
Bytes  Field   Notes
-----  -----   -----
1-2    JobID   Job ID from ReserveUnit (uint16 LE)
```

**Response (2 bytes):**

```
Bytes  Field   Notes
-----  -----   -----
1      Error   0 = success
2      Unk
```

**Observed request:** `01 00` (JobID = 1).

**Usage:** Called at end of job after the last `SetJobInfo2(jobflag=6)`. After this the driver continues polling until `Printed == totalPages`.

---

### 2.7 `0xE1A1` — `CAPT_SetJobInfo2`

**Direction:** host → printer (variable length).

**Request layout (minimum 72 bytes of fixed header + optional UTF-16 strings):**

```
Bytes   Field        Notes
-----   -----        -----
1-2     (zero)       Hard-coded 0
3-4     (zero)       Hard-coded 0
5-8     (zero)       Hard-coded 0
9-10    HostLen      UTF-16 byte length of hostname string (uint16)
11-12   UserLen      UTF-16 byte length of username string (uint16)
13-14   JobNameLen   UTF-16 byte length of job name string (uint16)
15-16   (zero)       Hard-coded 0
17      JobFlag      Job lifecycle marker:
                       1 = job start
                       2 = job continuation (not first page, not last)
                       3 = job end (Linux driver)
                       6 = job end (Windows driver; use this)
                       4 = job abort (unconfirmed)
18      NumberUp     Pages per sheet (N-up). Observed: 1
19-20   JobID        Job ID from ReserveUnit (uint16)
21-22   TZOffset     UTC offset in minutes relative to local time.
                       +300 = UTC−5, −180 = UTC+3 (Moscow)
23-24   TZOffset2    Same timezone value repeated
25-26   Year         Job timestamp year (uint16)
27      Month        Job timestamp month
28      Day          Job timestamp day
29      Hour         Job timestamp hour
30      Minute       Job timestamp minute
31      Second       Job timestamp second
32-72   (zero)       Hard-coded 0
73+     Strings      UTF-16LE: hostname, username, job name (concatenated, no separators)
                     Omit if corresponding length field is 0
```

**Response (2 bytes):**

```
Bytes  Field   Notes
-----  -----   -----
1      Error   0 = success
2      Unk
```

**Observed call sequence in 4-page job:**

1. **Job start** (`JobFlag=1`): sent after `ReserveUnit`, before first page.
2. **Job continuation** (`JobFlag=2`): sent mid-job when more pages remain (observed between page 3 and 4).
3. **Job end** (`JobFlag=6`): sent after last page's `StartPrint`, before `ReleaseUnit`.

**Observed payload (JobFlag=1, JobID=1, UTC+3):**

```
00000000 00000000 1e00 1600 1e00 0000  ← HostLen=30,UserLen=22,JobNameLen=30
01 01 01 00 10ff 10ff                  ← JobFlag=1, NUp=1, JobID=1, TZ=-256?
ea07 05 11 0c 11 03 00…               ← year=0x07ea=2026, month=5, day=17, hr=12, min=17, sec=3
…(zeros)…
57 00 49 00 4e 00 …                   ← UTF-16LE hostname "WINXPVM-42AB88B"
…                                     ← UTF-16LE username "winxpvmuser"
…                                     ← UTF-16LE job name "document-a4.pdf"
```

> **Linux driver notes:**
> - The Linux driver sends a fixed 76-byte packet (72-byte payload). Although `HostLen`, `UserLen`, and `JobNameLen` fields are populated with non-zero lengths (e.g. 28, 18, 32 bytes), no string data follows because the fixed packet size leaves no room. This is a known bug — the variable-length string section is effectively dead code.
> - Linux sends **`TZOffset` = 0** (UTC, i.e. no offset). Windows sends the real local-UTC delta.
> - Linux uses **`JobFlag = 3`** for the final `SetJobInfo2` (job end). Windows uses `JobFlag = 6`. Both values appear to work.
> - Linux sends `SetJobInfo2(flag=2)` as a **periodic heartbeat** throughout printing — it is paired with `SetLEDStatus(0x17)` and repeated roughly every ~10–15 polling cycles regardless of page-count milestones. This is fundamentally different from the Windows driver, which only sends `flag=2` once mid-job (when `Printed` first becomes ≥ 1).

---

### 2.8 `0xE0A6` — `CAPT_GoOffline`

**Direction:** host → printer.

**Request (2 bytes):**

| Driver  | Payload | Notes |
|---------|---------|-------|
| Windows | `00 00` | All zeros |
| Linux   | `01 00` | JobID (uint16 LE) as payload — sends the current job's ID |

**Response (2 bytes):** `00 00`

**Usage:** Transitions the printer to the offline state. Called:
1. During job initialization, **before** the clear sequence and `GoOnline` (Windows only — see §3.1).
2. During error recovery (e.g. out of paper): called after notifying the user via `SetLEDStatus`, then the driver polls `GetBasicStatus` until paper is detected via `GetInputStatus`, then calls `GoOnline` to resume.
3. **Linux job termination**: called after `SetJobInfo2(flag=3)` + `ClearError` + `DiscardData`, using the current JobID as payload. `ReleaseUnit` follows immediately.

**Observed effect:** `GetBasicStatus` byte 1 transitions from `0x00` to `0x10` (RCF_OFFLINE set).

---

### 2.9 `0xE0A3` — `CAPT_ClearMisPrint`

**Direction:** host → printer (no payload). Reply: `00 00`.

**Usage:** Called during job initialization sequence to clear any previous misprint state. Also called as part of the out-of-paper recovery sequence.

---

### 2.10 `0xE0A2` — `CAPT_ClearError`

**Direction:** host → printer (no payload). Reply: `00 00`.

**Usage:** Called during job initialization to clear any pending error flags. Also called as part of the out-of-paper recovery sequence.

---

### 2.11 `0xE0A4` — `CAPT_DiscardData`

**Direction:** host → printer (no payload). Reply: `00 00`.

**Usage:** Called during job initialization to flush any leftover buffered print data. Also called as part of the out-of-paper recovery sequence.

---

### 2.12 `0xE0A5` — `CAPT_GoOnline`

**Direction:** host → printer.

**Request:**

| Driver  | Length | Payload |
|---------|--------|---------|
| Windows | 16 bytes | `ee db ea ad 00 00 00 00 00 00 00 00 00 00 00 00` |
| Linux   | 8 bytes  | `ee db ea ad 00 00 00 00` |

The first 4 bytes (`ee db ea ad`) are a fixed magic value. Windows pads with 8 additional zero bytes; the Linux driver sends only the first 8 bytes (magic + 4 zeros).

**Response:** `00 00` (error flag + padding).

**Usage:** Called after `ClearMisPrint` / `ClearError` / `DiscardData` to take the printer from offline to online state. Called twice during out-of-paper recovery: once immediately after the error is cleared (which resets page counters to 0), and once after paper is added back and the second clear sequence completes.

**Observed effect:** `GetBasicStatus` byte 1 transitions:
- Before: `0x10` (offline, `RCF_OFFLINE` set)
- After: `0x00` (online, all clear)

**Important:** Every `GoOnline` call causes `GetExtendedStatus` page counters (`Start`, `Printing`, `Shipped`, `Printed`) to **reset to 0**.

---

### 2.13 `0xD0A9` — `CAPT_MultiCommand` (container)

**Direction:** host → printer (no reply).

Contains multiple `0xD0xx` sub-commands concatenated without additional framing — each sub-command uses the standard 4-byte header. No reply is expected for any `0xD0xx` command.

**Observed usage:** Sent as a single USB transfer containing:
1. `D0A0` — `CAPT_IC_BEGIN_PAGE`
2. `D0A4` — `CAPT_IC_BLACK_PLANE`
3. `D0A1` — `CAPT_IC_BEGIN_DATA`
4. `D0A2` — `CAPT_IC_END_PAGE`

**Role in the page protocol:** The `D0A9` block is a **page announcement** — it declares to the printer that a new page's raster data stream is about to follow, and carries all parameters for that page. It does NOT start the raster data itself; `IC_VIDEO_DATA` chunks that follow are attributed to this declared page until an `IC_BLACK_END` is received. Because `D0A9` is only a declaration (no data), it can be sent for page N+1 while page N's data is still being transmitted — the printer queues the descriptor. The `IC_VIDEO_DATA` for page N+1 must not start until page N's `IC_BLACK_END` has been sent.

---

### 2.14 `0xD0A0` — `CAPT_IC_BEGIN_PAGE`

**Direction:** host → printer (no reply). Payload: 40 bytes for LBP3000 (CNTblModel=1); 34 bytes for older models without the final 6 bytes (bytes 35-40 absent).

**Linux vs Windows differences in D0A0 payload:**

| Byte | Windows | Linux | Notes |
|------|---------|-------|-------|
| 1-2  | non-zero (PageSeq) | `00 00` | Linux always hardcodes 0 |
| 6    | `00`    | `01`  | Unknown field; Linux sets to 1 |
| 21   | `00`    | `01`  | Unknown field; Linux sets to 1 |
| 29-30 (ImgHeight) | `78 1a` = 6776 | `80 1a` = 6784 | Linux computes 8 lines more |

Issued once per page inside the `D0A9` multi-command before raster data.

**Payload layout (1-indexed):**

```
Bytes   Field        Notes
-----   -----        -----
1-2     PageSeq      Page sequence number assigned by the host driver per page content.
                     NOT a globally incrementing counter — Windows driver reuses the same
                     value when reprinting the same page after an error recovery.
                     Observed values: 0x0010 (pages 1–2 of a job), 0x0009, 0x000e, 0x000b
                     NOTE: Linux captfilter always hardcodes this field to 0x0000; the
                     non-zero values are Windows driver behaviour.
3-4     ModelConst   Model-specific constant. LBP3000 observed: 0x2a30
5       PaperSzByte  Paper size byte (paper size table index for the paper ID):
                       0x02 = A4, derived from tPaperSizeTbl lookup by paper_id
6       Unk6         Windows: 0x00. Linux: 0x01. Meaning unknown.
7       PaperSrc     Paper source/input slot. On LBP3000 (CNTblModel=1) this is
                     inputslot_to_papersource(inputslot); 0x00 = auto-feed (default).
                     On older models without CNTblModel this field is always 0.
                     Observed 0x00 for all LBP3000 captures (auto-feed).
8       (zero)
9-12    TonerDensity 4 bytes; for mono LBP3000 all 4 bytes equal.
                     Bits 5-2 = density value (0-7 for LBP3000). Default: 0x1f.
                     (0x1f = bits: 00011111, density = 7, bits 1,0 set to 1)
13      PaperType    Media type mode (result of special_mode_for_papertype()):
                       0x00 = Plain
                       0x20 = Envelope
                       0x24 = Transparency
14      ResFlag      Resolution indicator. 0x11 (17 decimal) = 600 dpi, 0x00 = 300 dpi.
                     LBP3000 always uses 600 dpi, so this is always 0x11.
15      Fixed_04     0x04 for LBP3000 (CNTblModel=1), 0x03 for older models
16      Fixed_00     0x00 for LBP3000, 0x01 for older models
17      Fixed_01     0x01 (always)
18      Fixed_01b    0x01 (always)
19      SuperSmooth  CNSuperSmooth — smoothing mode. Observed 0x02 for LBP3000.
                     Configurable; not a hardcoded constant.
20      TonerSave    CNTonerSaving: 0 = off, 1 = on
21      Unk21        Windows: 0x00. Linux: 0x01. Meaning unknown.
22      (zero/model) 0x00 for LBP3000 (CNTblModel=1). On older models may carry
                     a different field value.
23-24   MarginH      Image margin height in pixels at 600 dpi (uint16).
                     Derived from papertable_strange_dims p3 field:
                     MarginH = 600 × p3 / 2540. For A4: p3=510 → 120 px.
25-26   MarginW      Image margin width in pixels at 600 dpi (uint16).
                     Derived from papertable_strange_dims p1 field:
                     MarginW = 600 × p1 / 2540. For A4: p1=410 → 96 px.
27-28   LineSize     Image line width in bytes (= Hi-SCoA LINESIZE param) (uint16).
                     Computed as ceil(printable_width_px / 8), 32-pixel aligned.
29-30   ImgHeight    Image height in raster lines (uint16).
                     Windows observed: 6776 (0x1a78). Linux observed: 6784 (0x1a80).
                     Difference of 8 lines; likely due to slightly different margin
                     rounding in the Linux captfilter vs Windows driver.
31-32   PaperW       Physical paper width in pixels at 600 dpi (uint16).
                     May be swapped with PaperH when landscape/rotation is active.
33-34   PaperH       Physical paper height in pixels at 600 dpi (uint16).
                     May be swapped with PaperW when landscape/rotation is active.
35-36   FixFlags     CNBackPaperPrint_CNFixingMode_flags (uint16 LE).
                     = (CNBackPaperPrint | CNFixingMode). Controls duplex backside
                     print and fuser fixing mode for special media. For plain paper
                     with no duplex on LBP3000: 0x0000. Non-zero for duplex or
                     special media types. Only present for CNTblModel=1 (LBP3000+).
37      MediaType    mediatype_from_tbl — media type code from mediatype_from_papertype().
                     This field drives the fuser temperature on the engine side:
                       0x01 = Plain / Plain L
                       0x02 = Thick H
                       0x13 = Transparency
                       0x1c = Envelope
                     Only present for CNTblModel=1 (LBP3000+).
38-40   (zero)       Only present for CNTblModel=1 (LBP3000+).
```

**Observed values for A4 plain paper (LBP3000):**

```
Field         Value     Decimal / Notes
ModelConst    2a 30     0x2a30 (LBP3000)
PaperSzByte   02        A4 (tPaperSizeTbl index)
PaperSrc      00        Auto-feed
TonerDensity  1f1f1f1f  Default density (all 4 channels equal on mono)
PaperType     00        Plain (special_mode_for_papertype result)
ResFlag       11        0x11 = 17 decimal = 600 dpi
Fixed_04      04        LBP3000 (CNTblModel=1)
Fixed_00      00        LBP3000 (CNTblModel=1)
Fixed_01      01        always
Fixed_01b     01        always
SuperSmooth   02        CNSuperSmooth smoothing mode
TonerSave     00        Off
MarginH       78 00     120 pixels (from papertable_strange_dims p3=510: 600×510/2540=120)
MarginW       60 00     96 pixels  (from papertable_strange_dims p1=410: 600×410/2540=96)
LineSize      50 02     592 bytes/line
ImgHeight     78 1a     6776 lines (Windows); 80 1a = 6784 lines (Linux)
PaperW        60 13     4960 px (8.267" at 600 dpi ≈ A4 width)
PaperH        66 1b     7014 px (11.69" at 600 dpi ≈ A4 height)
FixFlags      00 00     Plain paper, no duplex (CNBackPaperPrint | CNFixingMode = 0)
MediaType     01        Plain / Plain L fuser mode
(zero)        00 00 00
```

---

### 2.15 `0xD0A4` — `CAPT_IC_BLACK_PLANE`

**Direction:** host → printer (no reply). Payload: 8 bytes.

Specifies Hi-SCoA compression parameters for the black (mono) plane. Sent inside the `D0A9` multi-command.

**Payload layout (0-indexed per SPECS):**

```
Offset  Field  Notes
------  -----  -----
0       L3     int8_t, positive. Back-reference offset 3 in Hi-SCoA. LBP3000: 0x01
1       L5     int8_t, positive. Back-reference offset 5 in Hi-SCoA. LBP3000: 0x04
2       Flag   Unknown flag. Always 0x01 observed.
3       BPP    Bits per pixel / plane count. 0x01 = mono. (0x08 suggested for color)
4       L0     int8_t, always 0x00
5       L2     int8_t, negative (signed). LBP3000: 0xf9 = −7
6-7     L4     int16_t LE. LBP3000: 0x0080 = 128
```

**Observed for LBP3000:**

```
01 04 01 01 00 f9 80 00
```

Typical Hi-SCoA compression parameters used with LBP3000:
- L0=0, L2=−7, L3=1, L4=128, L5=4
- LINESIZE=592 (from `D0A0` LineSize field)

---

### 2.16 `0xD0A1` — `CAPT_IC_BEGIN_DATA`

**Direction:** host → printer (no payload, no reply).

Marks that raster data (`IC_VIDEO_DATA`) for the announced page will follow. Part of the `D0A9` page announcement block, sent after `D0A4`.

---

### 2.17 `0xD0A2` — `CAPT_IC_END_PAGE`

**Direction:** host → printer (no payload, no reply).

Closes the `D0A9` page announcement block. Sent after `D0A1`. After this, `IC_VIDEO_DATA` (`C0A0`) chunks for the announced page begin.

---

### 2.18 `0xC0A0` — `CAPT_IC_VIDEO_DATA`

**Direction:** host → printer (no reply).

Carries Hi-SCoA compressed raster data. Multiple `C0A0` transfers are used per page.

**Max chunk size:** `GetPrinterInfo.Blk` bytes of payload (LBP3000: 65520 bytes; observed largest chunk: 65284 bytes payload).

**Flow control via `BufLevel`:**

The primary back-pressure mechanism is `GetBasicStatus` bytes 5-6 (`BufLevel`), not the `IM_DATA_BUSY` bit.

| BufLevel | Meaning |
|----------|---------|
| `0x000f` | 15 free slots — full speed ahead (4–5 chunks per burst) |
| `0x0008`–`0x000e` | Partial free — driver reduces burst size |
| `0x0001`–`0x0007` | Nearly full — driver sends 1 chunk, then polls |
| `0x0000` | **Buffer completely full** — driver must poll until BufLevel > 0 |

BufLevel formula (from `info/my.md`): `(BufLevel & 0xF) × (GetPrinterInfo_Buf << 15) / 15`

**`IM_DATA_BUSY` note:** Although the `GetBasicStatus` byte 1 bit `0x08` (`IM_DATA_BUSY`) is documented as a stop-send signal, it was **never observed to be set** on LBP3000 in any capture, including during heavy streaming of ~2 MB pages. The driver relies entirely on `BufLevel` for back-pressure.

**Linux driver chunk sizes and polling pattern:**

The Linux CAPT driver sends dramatically smaller `IC_VIDEO_DATA` chunks than the Windows driver:

| Driver  | Typical chunk size | Max observed | Polling rate |
|---------|--------------------|--------------|--------------|
| Windows | ~4000–65284 bytes  | 65284 bytes  | Every ~5 chunks |
| Linux   | 228–11444 bytes    | ~11444 bytes | After every single chunk |

Because Linux sends such small chunks and polls `GetExtendedStatus` after each one, the `BufLevel` counter **never drops below `0x000f`** (maximum) in the Linux capture. The Linux driver effectively never exercises the BufLevel flow-control path. `BufLevel`-driven throttling and streaming mode are Windows-driver behaviours not observed in the Linux driver for this printer.

**Observed page sizes — Windows XP driver, standard 4-page A4 job (`lbp3000-windowsxp`):**

| Page | Total bytes | Chunk count |
|------|-------------|-------------|
| 1    | 451 772     | 7 (65284×6 + 60068) |
| 2    | 460 528     | 7 (65284×7 + 3540)  |
| 3    | 263 028     | 5 (65284×4 + 1892)  |
| 4    | 369 704     | 6 (65284×5 + 43284) |

**Observed page sizes — Linux CAPT driver, 4-page A4 job (`lbp3000.pcap`):**

| Page | Total bytes | Notes |
|------|-------------|-------|
| 1    | 541 328     | Many small chunks (228–11308 bytes); BufLevel always 0x000f |
| 2    | 562 888     | Same pattern |
| 3    | 326 264     | Same pattern |
| 4    | 480 808     | Same pattern |

Linux page sizes are notably larger than Windows for the same 4-page A4 job. This is partly explained by the 8-line difference in `ImgHeight` (6784 vs 6776) and potentially also by differences in the Hi-SCoA compression parameters or the raster image content provided by `captfilter`.

**Observed page sizes (hard-to-compress jobs):**

| Capture | Page | Total bytes | Notes |
|---------|------|-------------|-------|
| hardtocompress  | 1 | 2 045 520 | Exceeds 2 MB buffer; streaming required |
| hardtocompress  | 2 | 2 054 336 | Same |
| hardtocompress  | 3 | 1 344 676 | Fits in buffer; normal non-streaming mode |
| hardtocompress2 | 1 | 1 901 688 | Just below 2 MB; normal mode (IC_BLACK_END → StartPrint) |
| hardtocompress2 | 2 | 1 385 160 | Well below 2 MB; oscillating BufLevel 0↔1 during send but still normal mode |
| hardtocompress2 | 3 | 2 151 412 | Exceeds 2 MB; streaming mode (StartPrint fires mid-stream at BufLevel=0) |
| hardtocompress4 | 1 | 2 240 364 | Exceeds 2 MB buffer; streaming required |
| hardtocompress4 | 2 | 2 240 364 | Same |
| hardtocompress4 | 3 | 2 240 364 | Same |

---

### 2.18a Large-Page Streaming Mode

When a page's compressed data approaches or exceeds the printer's 2 MB buffer, the driver **streams data while the printer is physically printing** that page. This mode is confirmed by the `lbp3000-windowsxp-hardtocompress.txt`, `lbp3000-windowsxp-hardtocompress2.txt`, and `lbp3000-windowsxp-hardtocompress4.txt` captures.

**The actual streaming trigger is `BufLevel` dropping to 0 while chunks remain**, not the page data size per se. If all chunks are sent before `BufLevel` reaches 0, the printer prints in normal mode. If `BufLevel` reaches 0 before the last chunk is sent, streaming mode engages.

**Critical ordering distinction — normal vs. streaming:**

| Mode | Condition | Order |
|------|-----------|-------|
| Normal | All chunks sent before BufLevel reaches 0 | last chunk → `IC_BLACK_END` → `StartPrint(N)` |
| Streaming | BufLevel=0 reached while chunks still remain | `StartPrint(N)` at BufLevel=0 → poll BufLevel≥1 → remaining chunk(s) → `IC_BLACK_END` |

> **Important:** Even pages approaching 2 MB (e.g. 1.9 MB) may use normal mode if the BufLevel never reaches 0 before the last chunk is sent. Pages slightly over 2 MB will use streaming mode. The threshold is not a fixed byte count.

**Normal mode sequence (BufLevel never hits 0 during data send):**

```
1. Send D0A9 {D0A0, D0A4, D0A1, D0A2}  ← page N parameters

2. [Burst phase] Send IC_VIDEO_DATA chunks for page N:
     poll GetBasicStatus every ~5 chunks
     reduce burst size as BufLevel drops (continue sending)
     BufLevel never reaches 0 before last chunk is sent

3. After the LAST IC_VIDEO_DATA chunk: BufLevel is ≥ 1
     Send IC_BLACK_END

4. Send StartPrint(N)
```

**Streaming mode sequence (BufLevel hits 0 while chunks remain):**

```
1. Send D0A9 {D0A0, D0A4, D0A1, D0A2}  ← page N parameters

2. [Burst phase] Send IC_VIDEO_DATA chunks for page N:
     poll GetBasicStatus every ~5 chunks initially
     as BufLevel drops, send one chunk per BufLevel≥1 poll

3. When BufLevel first reaches 0 and chunks still remain:
     Send StartPrint(N)               ← fired immediately at BufLevel=0

4. [Drain wait] Poll GetBasicStatus repeatedly until BufLevel ≥ 1
     BufLevel=0 here is normal; engine is consuming the buffer
     observed: ~15–382 polls before BufLevel rises to 1

5. Send the remaining IC_VIDEO_DATA chunk(s)
     (may be just one partial/small final chunk)

6. Send IC_BLACK_END                  ← sent after remaining chunks, with BufLevel ≥ 1
```

**Observed `BufLevel` progression — hardtocompress capture, page 1 (~2.05 MB, streaming mode):**

```
Phase                         BufLevel    Action
--------------------------------------------------
Initial burst (4 chunks)      0x0f        poll → Start=1; page 2 D0A9 sent immediately
Next burst (4 chunks)         0x0f        —
Next burst (5 chunks)         0x0f        —
Next burst (5 chunks)         0x0e        —
Next burst (5 chunks)         0x09        —
Next burst (5 chunks)         0x04        —
Last batch (3×65284 + 21716)  0x04 → 0x00 StartPrint(1) issued after last chunk
Poll ×382                     0x00        waiting for engine to consume buffer
BufLevel rises                0x01        IC_BLACK_END sent
```

In this capture the last chunk happened to be sent when BufLevel was still 0x04, dropping to 0x00 shortly after — effectively the whole page was sent before StartPrint.

**Observed `BufLevel` progression — hardtocompress2 capture, page 3 (~2.15 MB, streaming mode):**

```
Phase                                    BufLevel    Action
----------------------------------------------------------
Burst (5 chunks)                         0x0f        Start=3 seen after D0A9
Burst (5 chunks)                         0x0e        —
Burst (5 chunks)                         0x0b        —
Burst (5 chunks)                         0x07        —
Burst (5 chunks)                         0x04        —
1 chunk (BufLevel≥1 wait per chunk)      0x01        —
1 chunk (BufLevel≥1 wait per chunk)      0x00 → 0x01 —
1 chunk                                  0x00        StartPrint(3) issued at BufLevel=0
Poll ×17                                 0x00        —
ExtendedStatus poll                      0x00        Start=3, Printing=2 still
Poll ×72                                 0x00        —
ExtendedStatus poll                      0x00        Printing=3
Poll ×15 until BufLevel rises            0x01        —
Last chunk (62,324 bytes)                0x01→0x00   remaining data sent
Poll (BufLevel≥1 confirmed)              0x01        IC_BLACK_END sent
```

**Observed `BufLevel` progression — hardtocompress2 capture, page 1 (~1.9 MB, normal mode):**

```
Phase                              BufLevel    Action
------------------------------------------------------
Initial burst (5 chunks)           0x0f        poll → Start=1; page 2 D0A9 sent
Burst (5 chunks)                   0x0f        —
Burst (5 chunks)                   0x0f        —
Burst (5 chunks)                   0x0c        —
Burst (5 chunks)                   0x07        —
Burst (4 chunks + 1 small 8452)    0x02        BufLevel never reached 0
Poll                               0x02        IC_BLACK_END sent (BufLevel=2)
                                               StartPrint(1) sent
```

**Next-page `D0A9` timing:** The `D0A9` multi-command for page N+1 can be sent **as soon as `GetExtendedStatus.Start == N`** (page N was accepted by the decoder). In both `hardtocompress` and `hardtocompress2` captures, page 2's `D0A9` is sent after the first status poll showing `Start=1`, while only ~261 KB of page 1's data has been transmitted. However, page N+1's `IC_VIDEO_DATA` chunks must **NOT** start until page N's `IC_BLACK_END` has been sent.

**Note on `Start` counter timing after D0A9:** `Start` does NOT increment to N+1 immediately when page N+1's `D0A9` is sent during streaming. The decoder must first finish accepting page N before it increments. In the `hardtocompress2` capture, page 2's `D0A9` is sent while `Start=1`, but `Start` only becomes `2` after ~382 polls at BufLevel=0 (when `Printing=1` is also seen). In non-streaming mode, `Start` increments shortly after `D0A9` is sent.

**Page counter behaviour during streaming (hardtocompress capture, all large pages):**

| Phase | Start | Printing | Shipped | Printed | BufLevel |
|-------|-------|----------|---------|---------|----------|
| Page 1 D0A9 sent | 1 | 0 | 0 | 0 | 0x0f |
| Page 2 D0A9 sent (4 chunks into page 1) | 1 | 0 | 0 | 0 | 0x0f |
| Page 1 last chunk + StartPrint(1) | 1 | 0 | 0 | 0 | 0x04→0x00 |
| Polling (engine consuming buffer) | 1 | 0 | 0 | 0 | 0x00 |
| BufLevel rises; IC_BLACK_END page 1 | 2 | 1 | 0 | 0 | 0x01 |
| Page 2 streaming begins (1 chunk/poll) | 2 | 1 | 0 | 0 | 0x00/0x01 |
| Page 2 last chunk + StartPrint(2) | 2 | 1 | 1 | 0 | 0x00 |
| Polling; BufLevel rises | 2 | 2 | 1 | 0 | 0x01 |
| IC_BLACK_END page 2 | 2 | 2 | 1 | 0 | 0x01 |
| Page 3 (fits in buffer) — normal mode | 3 | 2 | 1 | 1 | rising |
| Page 3 last chunk + IC_BLACK_END | 3 | 2 | 1 | 1 | 0x02 |
| StartPrint(3) (after IC_BLACK_END) | 3 | 2→3 | 1 | 1 | 0x03 |
| Post-job drain | 3 | 3 | 2→3 | 1→3 | 0x0f |

**Page counter behaviour during streaming (hardtocompress2 capture, mixed-size pages):**

| Phase | Start | Printing | Shipped | Printed | BufLevel |
|-------|-------|----------|---------|---------|----------|
| After GoOnline — page counters reset | 0 | 0 | 0 | 0 | 0x0f |
| Page 1 D0A9 sent | 1 | 0 | 0 | 0 | 0x0f |
| Page 2 D0A9 sent (5 chunks into page 1) | 1 | 0 | 0 | 0 | 0x0f |
| Page 1 IC_BLACK_END + StartPrint(1) (BufLevel=2) | 1 | 0 | 0 | 0 | 0x02 |
| First chunk page 2; BufLevel | 1 | 0 | 0 | 0 | 0x01 |
| Second chunk page 2; then 382 polls | 1 | 0 | 0 | 0 | 0x00 |
| BufLevel=0, ExtendedStatus; Start rises | 2 | 1 | 0 | 0 | 0x00 |
| Page 2 sending (1 chunk/poll) | 2 | 1 | 0 | 0 | 0x00/0x01 |
| Page 2 IC_BLACK_END + StartPrint(2) (BufLevel=1) | 2 | 1 | 0 | 0 | 0x01 |
| BufLevel rising after page 2 start | 2 | 1 | 1 | 0 | rising 0x02→0x0a |
| Page 3 D0A9 sent (after SetJobInfo2 flag=2) | 3 | 2 | 1 | 1 | 0x0f |
| Page 3 streaming: StartPrint(3) at BufLevel=0 | 3 | 2 | 2 | 1 | 0x00 |
| Polling drain (engine consuming) | 3 | 3 | 2 | 1 | 0x00 |
| BufLevel rises; last chunk sent; IC_BLACK_END | 3 | 3 | 2 | 1 | 0x01 |
| Post-job drain | 3 | 3 | 3 | 1→3 | rising |

**`BufLevel` = 0 in `GetExtendedStatus` during streaming:** This is the normal full-buffer state. It does NOT indicate an error. In streaming mode, the driver issues `StartPrint` at this point (if chunks remain) and then polls until `BufLevel` rises before continuing.

**Page 2 streaming after IC_BLACK_END for page 1 (hardtocompress/hardtocompress2):** After `IC_BLACK_END` for page 1, page 2 `IC_VIDEO_DATA` is sent one chunk per poll (since BufLevel alternates 0→1→0 during streaming). This is the expected throttled sending pattern while the engine is actively consuming.

**SetJobInfo2(flag=2) timing in hardtocompress2:** `SetJobInfo2(flag=2)` (mid-job continuation) is sent when `GetExtendedStatus.Printed` first reaches 1 — i.e., after page 1 completes printing. Observed: triggered between page 2's `StartPrint` and page 3's `D0A9` when `Printed=1` and `Printing=2`.

---

### 2.19 `0xC0A4` — `CAPT_IC_BLACK_END`

**Direction:** host → printer (no payload, no reply).

**Page stream separator.** Marks the end of the current page's raster data stream. Any `IC_VIDEO_DATA` chunk received by the printer **after** this command belongs to the next page (announced by a subsequent `D0A9`). Must be sent after the last `C0A0` chunk for the current page, and before the first `C0A0` chunk of the next page.

In streaming mode, `IC_BLACK_END` is sent slightly after the last chunk (once `BufLevel` rises to ≥ 1 following `StartPrint`). In normal mode it is sent immediately after the last chunk, before `StartPrint`. See §2.18a and §2.20 for ordering details.

---

### 2.20 `0xE0A7` — `CAPT_StartPrint`

**Direction:** host → printer.

**Request (2 bytes):**

```
Bytes  Field   Notes
-----  -----   -----
1-2    PageNum Page number to start printing (uint16 LE). Should match GetExtendedStatus.Start
```

**Response:** `00 00`.

**Usage:** Signals the engine to begin the physical printing of a page.

- **Normal mode** (BufLevel does not reach 0 before last chunk): called after `IC_BLACK_END` (`C0A4`).
- **Streaming mode** (BufLevel reaches 0 while chunks remain): called immediately when BufLevel first hits 0 (before the remaining chunk(s) are sent). After `StartPrint`, the driver waits for BufLevel ≥ 1, sends any remaining data, then sends `IC_BLACK_END`.

See §2.18a for the complete streaming sequence.

**Observed calls:**
- `StartPrint(1)` – page 1
- `StartPrint(2)` – page 2
- `StartPrint(3)` – page 3
- `StartPrint(4)` – page 4

---

### 2.21 `0xE1A2` — `CAPT_SetLEDStatus`

**Direction:** host → printer.

**Request (12 bytes):**

```
Bytes  Field     Notes
-----  -----     -----
1      IntStatus Internal driver status code sent to printer.
                 Windows driver does NOT use this field (sends 0).
                 Linux driver sends its internal error_code.
                 0x16 = PrinterReadyToPrint
2      (zero)
3      HSCode    Handshake/signal code (see table below)
4      PaperSize Paper size from tPaperSizeTbl (second column value)
5      Flag      0 (Linux) or 1 (Windows)
6      MediaSrc  Media source slot (always 0 for LBP3000)
7-8    (zero)
9-12   ErrFlags  Error flags written back; appear in GetExtendedStatus HostErr (bytes 37-40)
```

**HS Code table:**

| int_status | Meaning              | hs_code |
|------------|----------------------|---------|
| 1          | PrinterReadyToPrint  | 0       |
| 4          | CNPaused             | 5       |
| 9          | No paper             | 1       |
| 11         | CNDataXferError      | 4       |
| 12         | CNChangePaperSize    | 2       |
| 14         | InputMediaSupplyEmpty| (cassette open) 7 |

**HostErr flag values:**

| int_status | Flag value   |
|------------|-------------|
| 1 (Ready)  | 0x00000000  |
| 4 (Paused) | 0x01000000  |
| 9 (NoPaper)| 0x00010000  |
| 11 (XferErr)| 0x00008000 |
| 12 (WrongSize)| 0x00080000 |
| 13 (Cleaning)| 0x00000004 |
| 20 (IllegalConn)| 0x00000008 |

**Response:** `00 00`.

---

### 2.22 `0xA0A0` — `CAPT_NOP` / status probe

**Direction:** host → printer (no payload). Reply: likely empty `00 00 04 00`.

Purpose unclear; possibly a NOP or alternate status check. Not observed in LBP3000 capture.

---

### 2.23 `0xA1A0` — `CAPT_GetIEEE1284ID`

**Direction:** host → printer (no payload). Response is raw IEEE-1284 device ID string — **not** the standard CAPT reply format.

---

## 3. Print Job Lifecycle

### 3.1 Normal Job (no errors)

#### 3.1.1 Windows XP driver

The complete sequence observed in `lbp3000-windowsxp.txt` for a 4-page A4 job:

```
[Startup / Idle polling loop]
GetPrinterInfo          ← get Blk/Buf parameters once
GetExtendedStatus       ← initial state
GetInputStatus          ← (triggered by Bas1 & 0x02)
  … repeat GetExtendedStatus + GetInputStatus until stable …

[Job initialization]
ReserveUnit             → JobID (increments across jobs; first job = 1)
SetJobInfo2(flag=1)     ← job start, sends hostname/username/jobname
GetBasicStatus
GetExtendedStatus
GetInputStatus
GoOffline               ← only if printer is NOT already offline (Bas & 0x10 == 0)
GetBasicStatus          ← confirm offline (byte1 = 0x10)
ClearMisPrint
ClearError
DiscardData
GetBasicStatus
GetExtendedStatus       ← confirm offline, LED state, page counters
GoOnline(ee db ea ad… × 16 bytes)  ← bring printer online
GetBasicStatus          ← confirm online (byte1 → 0x00)
GetExtendedStatus       ← page counters reset to 0,0,0,0

[Page 1 — data send loop]
GetBasicStatus          ← confirm buffer available (BufLevel > 0)
D0A9 { D0A0, D0A4, D0A1, D0A2 }   ← page parameters
IC_VIDEO_DATA ×N        ← raster chunks; interleave GetBasicStatus every ~5 chunks;
                           throttle when BufLevel low (see §2.18)
IC_BLACK_END
StartPrint(1)

[Page 2 — data send loop]
GetBasicStatus          ← poll; proceed when BufLevel > 0
D0A9 { D0A0, D0A4, D0A1, D0A2 }
IC_VIDEO_DATA ×N
IC_BLACK_END
  … wait for GetExtendedStatus.Printing == 2 before StartPrint …
StartPrint(2)

[Pages 3 and 4 — repeat page loop pattern]
  … SetJobInfo2(flag=2) once when GetExtendedStatus.Printed >= 1 (mid-job) …

[Job termination]
SetJobInfo2(flag=6)     ← Windows job-end flag
ReleaseUnit

[Post-job drain loop]
  … alternate GetInputStatus + GetExtendedStatus …
  … until GetExtendedStatus.Printed == totalPages …
```

**Note on `GoOffline` during initialization:** `GoOffline` is only needed when the printer is online at the start of the session (Bas byte 1 bit `0x10` is clear). If the printer is already in the offline/uninitialized state (`Bas & 0x10` set), the `ClearMisPrint` / `ClearError` / `DiscardData` sequence may be sent directly without `GoOffline`. Observed in `lbp3000-windowsxp-hardtocompress4.txt`: printer starts at `Bas=0x30` (already offline), so `GoOffline` is skipped.

---

#### 3.1.2 Linux CAPT driver

The complete sequence observed in `parsed/lbp3000.txt` for a 4-page A4 job. The Linux driver behaviour differs significantly from Windows in initialization order, polling density, chunk sizes, and job termination.

```
[Startup — printer is already offline (Bas=0x31 → RCF_PRINTER_FREE | RCF_OFFLINE)]
GetPrinterInfo          ← twice at startup
GetExtendedStatus       ← twice; printer offline (Bas=0x31)

[Job initialization — NO GoOffline needed (printer already offline)]
ReserveUnit             → JobID=1
DiscardData             ← order differs from Windows: DiscardData first
ClearMisPrint
ClearError
GoOnline(ee db ea ad 00 00 00 00)  ← 8-byte payload (not 16 bytes like Windows)
GetPrinterInfo          ← re-read after GoOnline
GetExtendedStatus       ← Bas=0x00 (online), page counters 0,0,0,0
SetLEDStatus(0x16, …)   ← byte1=0x16=PrinterReadyToPrint error_code; sent BEFORE SetJobInfo2
GetInputStatus
GetPrinterInfo          ← again
GetExtendedStatus
SetLEDStatus(0x17, …)   ← byte1=0x17=CNPrinting error_code; second SetLEDStatus
SetJobInfo2(flag=1)     ← job start (no hostname/username/jobname strings despite non-zero lengths)

[Page 1 — data send loop]
GetBasicStatus
GetExtendedStatus       ← wait for Start=1 (page descriptor accepted)
D0A9 { D0A0, D0A4, D0A1, D0A2 }   ← page 1 parameters (PageSeq=0x0000 hardcoded)
IC_VIDEO_DATA (228 bytes)
GetBasicStatus
GetExtendedStatus       ← poll after every single chunk; BufLevel stays 0x000f always
IC_VIDEO_DATA (228 bytes)
GetBasicStatus
GetExtendedStatus
  … repeat: one chunk, then GetBasicStatus + GetExtendedStatus, every time …
  [SetJobInfo2(flag=2) + SetLEDStatus(0x17) heartbeat every ~10–15 poll cycles]
IC_BLACK_END
StartPrint(1)           ← normal mode (BufLevel never dropped)

[Between pages: heartbeat continues]
SetJobInfo2(flag=2) + SetLEDStatus(0x17)   ← repeated again after StartPrint

[Page 2 — D0A9 sent after StartPrint(1)]
D0A9 { D0A0, D0A4, D0A1, D0A2 }   ← page 2 parameters
  … same chunk-by-chunk pattern with interleaved heartbeats …
IC_BLACK_END
StartPrint(2)

[Pages 3 and 4]
  ← D0A9 for page 3 sent after GetInputStatus call triggered by Bas1 change (Printed=1 visible)
  ← D0A9 for page 4 sent after Printed=2 is seen in GetExtendedStatus
  … same pattern …
IC_BLACK_END
StartPrint(3)
  …
IC_BLACK_END
StartPrint(4)

[Job termination — Linux sequence differs from Windows]
  … heartbeat SetJobInfo2(flag=2) + SetLEDStatus(0x17) continues while Printed < 4 …
  … when Printed==4 detected:
SetJobInfo2(flag=2) + SetLEDStatus(0x17)   ← final heartbeat
SetJobInfo2(flag=3)   ← job end (Linux uses flag=3; Windows uses flag=6)
GetExtendedStatus
ClearError
DiscardData
GetExtendedStatus
GoOffline(01 00)      ← payload is JobID (0x0001 LE), not zeros like Windows
ReleaseUnit(01 00)    ← JobID

[Post-job idle polling]
GetExtendedStatus
GetPrinterInfo
GetInputStatus
GetExtendedStatus
GetPrinterInfo
GetExtendedStatus     ← Bas=0x11, LED transitions 0x57→0x56 (job→idle)
```

**Key Linux vs Windows differences summarised:**

| Aspect | Windows XP driver | Linux CAPT driver |
|--------|-------------------|-------------------|
| Init clear order | `ClearMisPrint` → `ClearError` → `DiscardData` | `DiscardData` → `ClearMisPrint` → `ClearError` |
| GoOnline payload | 16 bytes | 8 bytes |
| SetLEDStatus timing | After job init | Before `SetJobInfo2(flag=1)`, during startup |
| SetJobInfo2(flag=2) trigger | Once, when `Printed` ≥ 1 | Repeated heartbeat (~every 10–15 polls) throughout job |
| SetJobInfo2 job-end flag | `6` | `3` |
| GoOffline payload | `00 00` | `01 00` (JobID) |
| Job end sequence | `flag=6` → `ReleaseUnit` | `flag=3` → `ClearError` → `DiscardData` → `GoOffline(JobID)` → `ReleaseUnit` |
| Chunk sizes | Up to 65284 bytes | 208–~11444 bytes (typically 228 bytes) |
| Polling rate | Every ~5 chunks | After every single chunk |
| BufLevel observed | Drops to 0 on large pages | Always 0x000f (buffer never fills) |
| D0A0 byte 6 | `0x00` | `0x01` |
| D0A0 byte 21 | `0x00` | `0x01` |
| D0A0 PageSeq (bytes 1-2) | Non-zero (content hash) | `0x0000` (always) |
| D0A0 ImgHeight (A4) | 6776 (0x1a78) | 6784 (0x1a80) |
| TZOffset in SetJobInfo2 | Real UTC offset (minutes) | 0 (UTC) |
| Hostname/user/jobname | Correctly appended | Set to non-zero lengths but strings absent (bug) |
| Next-page D0A9 timing | Can be sent while page N data is still streaming | Sent only after `StartPrint(N)` (or after Printed counter advances) |
| Post-job drain | Alternates `GetInputStatus` + `GetExtendedStatus` | A few `GetExtendedStatus` + `GetPrinterInfo` + `GetInputStatus` cycles then stops |

### 3.2 Out-of-Paper Recovery

Observed in `lbp3000-windowsxp-4pages-outofpaper-after-2-then-reprint.pcapng`: 4-page job, paper runs out after page 2 is printed (during page 3 data queuing). Pages 3 and 4 are reprinted after paper is added.

> **LBP3000 hardware note:** This printer has **no paper-tray sensor**. Paper-out is detected only when the engine fails to physically pick a sheet mid-print. The printer then lights its LED indicator. Recovery requires the user to insert paper **and press the physical go button** on the printer. Only after the button press does the printer set `Bas1 & 0x02` to notify the driver. The driver does NOT detect paper presence — it waits for the printer/user to signal readiness.

**Out-of-paper detection:**

While sending page data, the driver polls `GetBasicStatus`. Out-of-paper is detected when:

```
GetBasicStatus → byte1 = 0x16 (RCF_NOTREADY | RCF_OFFLINE), byte2 = 0x8b
GetExtendedStatus:
  Bas  = 0x16  → RCF_NOTREADY (0x02) + RCF_OFFLINE (0x10)
  Bas1 = 0x8a  → needGetInputStatus (0x02) set
  Cnt  = 0x40  → RCF_PRINT_REJECTED
  Pap  = 0x00  → engine in paper-out error state
GetInputStatus → byte3 = 0x80, PaperID/Width/Height = 0 (error state)
```

**Out-of-paper error handling sequence:**

```
[Error detected]
GetInputStatus          ← triggered by Bas1 & 0x02; byte3 = 0x80 confirms paper-out error
GetBasicStatus          ← observe transition through 0x16 → 0x12 → 0x12
GetExtendedStatus       ← Bas=0x12, Cnt=0x40 (PRINT_REJECTED), Pap=0x00
GetBasicStatus
ClearMisPrint           ← clear print error
ClearError
DiscardData             ← discard partially-sent page data
GetBasicStatus
GetExtendedStatus       ← Bas=0x10 (offline), Cnt=0x00 (error cleared), Pap=0x00
GoOnline                ← pulse online to reset page counters
GetBasicStatus          ← byte1 → 0x00
GetExtendedStatus       ← Start=0, Printing=0, Shipped=0, Printed=0 (RESET)
GetBasicStatus
SetLEDStatus(NoPaper)   ← notify printer to show out-of-paper status to user
                           byte3=0x01 (HSCode=1=NoPaper), byte4=PaperSizeID,
                           byte5=0x01, bytes9-12=0x00010000 (NoPaper HostErr flag)
GetBasicStatus
GetExtendedStatus       ← HostErr byte 37-40 now reflects 0x00010000

[Wait for user — go offline and poll]
GoOffline
GetBasicStatus          ← byte1 = 0x10 (offline)
  … poll GetBasicStatus repeatedly (may be 85+ iterations) …
  … user inserts paper and presses go button on printer …
  … printer sets Bas1 & 0x02 (needGetInputStatus) after button press …
GetInputStatus          ← byte3 changes from 0x80 → 0xc0 (engine ready again)
                           PaperID/Width/Height fields become valid
SetLEDStatus(0)         ← clear NoPaper status (all 12 payload bytes = 0x00)
ClearMisPrint
ClearError
DiscardData
GetBasicStatus
GetExtendedStatus       ← Pap=0x80 (engine ready), Bas=0x10 (still offline)
GoOnline                ← bring printer back online
GetBasicStatus          ← byte1 → 0x00
GetExtendedStatus       ← Start=0, Printing=0, Shipped=0, Printed=0

[Resume printing — reprint unprinted pages]
  … same page data send loop as normal job …
  … Windows driver uses the same PageSeq values in D0A0 as the original attempt …
StartPrint(N)           ← for each reprinted page

[Job termination — same as normal]
SetJobInfo2(flag=6)
ReleaseUnit

[Post-job drain]
  … alternate GetInputStatus + GetExtendedStatus …
  … until Printed == totalPages …
```

**Key observations:**
- The driver does NOT detect paper presence directly — it waits for the **user to press the go button** on the printer, which changes `Bas1 & 0x02` to trigger a `GetInputStatus` call.
- Page counters (Start/Printing/Shipped/Printed) are **reset to 0** after the recovery `GoOnline`. The driver must count pages printed in the previous attempt separately to know which pages to reprint.
- `SetLEDStatus` with `HostErr = 0x00010000` notifies the printer/UI of the no-paper condition. This value appears in `GetExtendedStatus` bytes 37-40 (`HostErr`).
- `SetLEDStatus` with all-zero payload is called after the button press to clear the no-paper condition.
- The recovery uses a **second** `ClearMisPrint` + `ClearError` + `DiscardData` + `GoOnline` sequence after the button is pressed.
- The Windows driver (confirmed) reprints pages that were not successfully printed; it reuses the same `D0A0 PageSeq` field values for the same page content.

---

## 4. `GetExtendedStatus` Page-Counter Behaviour

The `Start`, `Printing`, `Shipped`, `Printed` fields (bytes 15-22) track the current page pipeline:

**Standard (small pages, buffer not exhausted, normal mode):**

| Phase | Start | Printing | Shipped | Printed |
|-------|-------|----------|---------|---------|
| Idle / job not started | 0 | 0 | 0 | 0 |
| Page 1 data queued, engine not yet engaged | 1 | 0 | 0 | 0 |
| Page 1 in engine, page 2 data queued | 2 | 1 | 0 | 0 |
| Page 1 being ejected | 2 | 1 | 1 | 0 |
| Page 1 done, page 2 printing | 2 | 2 | 1 | 1 |
| Page 2 being ejected | 3 | 2 | 2 | 1 |
| Page 2 done, page 3 being decoded | 3 | 2 | 2 | 2 |
| All done (4-page job) | 4 | 4 | 4 | 4 |

**Large-page streaming (BufLevel hits 0 while chunks remain):**

See §2.18a for the complete tables and explanation. Summary:

| Phase | Start | Printing | BufLevel |
|-------|-------|----------|----------|
| Page N D0A9 sent | N | N−1 | 0x0f |
| BufLevel=0 reached — StartPrint(N) issued | N | N−1 | 0x00 |
| Poll drain (engine consuming) | N | N−1→N | 0x00 |
| BufLevel rises ≥ 1 — remaining chunk(s) sent | N | N | 0x01 |
| IC_BLACK_END page N | N+1* | N | ≥0x01 |

\* `Start` increments to N+1 only after the engine's decoder has consumed page N, which happens around the time `Printing` increments — NOT immediately when page N+1's `D0A9` is sent.

**`Start` counter semantics:** `Start` increments when the printer's decoder pipeline actually accepts a page's `D0A9` descriptor. In streaming mode, this can lag significantly behind when `D0A9` was sent (the descriptor queues but the decoder is busy). In non-streaming mode, `Start` increments shortly after `D0A9` is sent.

After an out-of-paper recovery `GoOnline`, all four counters reset to **0** regardless of how many pages were printed before the error.

> **LBP3000 note:** Because this printer has no paper sensor, the `Pap` field (byte 11) reflects engine readiness state rather than physical paper detection. `Pap=0x80` = engine ready to feed; `Pap=0x00` = engine in paper-out error condition.

---

## 5. Paper Size Table (`tPaperSizeTbl`)

**Column semantics:**

- **`paper_id`** — the internal paper size identifier used throughout the driver:
  in the paper-name → ID lookup table, in `papersize_res600_cntblmodel1` (physical
  pixel dimensions), and in `papertable_strange_dims` (margin source values).
  This is **not** the value written directly into `D0A0` byte 5.

- **`PaperSzByte`** (`D0A0` byte 5 / `SetLEDStatus` byte 4 / `GetInputStatus` byte 6) —
  derived from `paper_id` via a linear scan of `tPaperSizeTbl` (`struc_8057020`
  pairs at `.data:08057020`): the driver walks the table until it finds a matching
  `paper_id` in the first field, then takes the second field as `PaperSzByte`.

| `paper_id` | `PaperSzByte` | Name             |
|------------|---------------|------------------|
| 0x08       | 1             | A3               |
| 0x09       | 2             | A4               |
| 0x0B       | 3             | A5               |
| 0x0C       | 6             | B4               |
| 0x0D       | 7             | B5               |
| 0x07       | 10            | Executive        |
| 0x05       | 12            | Legal            |
| 0x03       | 11            | Ledger           |
| 0x01       | 13            | Letter           |
| 0x2B       | 14            | Postcard         |
| 0x52       | 15            | Double Postcard  |
| 0x1C       | 21            | Envelope C5      |
| 0x14       | 22            | Envelope #10 (Com10) |
| 0x25       | 23            | Envelope Monarch |
| 0x1B       | 24            | Envelope DL      |
| 0x22       | 25            | Envelope B5      |
| 0x5B       | 26            | Envelope Y4 (jenv_you4) |
| 0x47       | 27            | Envelope K2 (jenv_kaku2) |
| 0x1F       | 28            | Envelope Y2 (jenv_you2) |
| 0x106      | 28            | Envelope Y2 (alt ID) |
| 0x10CC     | 55            | 4×1 Postcard (4x_postcard) |
| 0x023A     | 64            | Index Card       |

---

## 6. LED / Internal Status Codes

Seen in `GetExtendedStatus` byte 25 (`LED`). Absolute values are internal signalling — changes matter more than the values themselves.

| Observed value | State          |
|----------------|----------------|
| `0x56`         | Idle / standby |
| `0x57`         | Job active     |

Full status table (from `captmon2` binary, `error_code` column):

| int_status | Meaning                     | error_code |
|------------|----------------------------|------------|
| 0          | CNCheckingStatus           | 0x00       |
| 1          | PrinterReadyToPrint        | 0x16       |
| 2 / 3      | CNPrinting                 | 0x17       |
| 4          | CNPaused                   | 0x17       |
| 5          | CNPrinterPortBusy          | 0x00       |
| 6          | CNPrinterOffline           | 0x00       |
| 7          | CoverOpen                  | 0x0D       |
| 8 / 16     | Jam                        | 0x0D       |
| 9 / 14     | InputMediaSupplyEmpty      | 0x35       |
| 10         | MarkerTonerCartridgeMissing| 0x0D       |
| 11         | CNDataXferError            | 0x0D       |
| 12 / 15    | CNChangePaperSize          | 0x35       |
| 13         | CNCleaning                 | 0x35       |
| 17         | CoverOpen (tone unit)      | 0x0D       |
| 18         | CoverOpen (top)            | 0x0D       |
| 19         | CoverOpen (duplex)         | 0x0D       |
| 20         | InputMediaSupplyEmpty (cassette open) | 0x35 |
| 21         | IllegalConnection (duplex) | 0x0D       |
| 22         | IllegalConnection (cassette)| 0x0D      |
| 0x8001     | CNPrinterCommError         | 0x00       |
| 0x8002     | CNInvalidData              | 0x0D       |
| 0x8003     | CNNoMemory                 | 0x00       |
| 0x8004     | CNServiceCall              | 0x19       |
| 0x8005     | CNCleaning                 | 0x17       |
| 0x8006     | CNUnknown                  | 0x00       |

---

## 7. LBP3000-Specific Constants

| Parameter     | Value        | Notes                          |
|---------------|--------------|--------------------------------|
| DevID         | `0x2a30`     | From `GetPrinterInfo` bytes 3-4 |
| CNTblModel    | 1            | Internal model selector used throughout driver |
| Blk           | 65520 (0xfff0) | Max IC_VIDEO_DATA chunk bytes |
| Buf           | 64 (0x40)    | Max pre-queued buffer count; RAM = 2 MB |
| D0A0 ModelConst | `30 2a`    | Bytes 3-4 of `IC_BEGIN_PAGE` (= DevID 0x2a30 LE) |
| D0A0 ResFlag  | `0x11` (17)  | 600 dpi indicator in byte 14 of IC_BEGIN_PAGE |
| D0A0 size     | 44 bytes total (40 payload) | CNTblModel=1 adds 6 bytes vs older 34-byte format |
| Hi-SCoA L2    | −7 (0xf9)    |                                |
| Hi-SCoA L3    | 1            |                                |
| Hi-SCoA L4    | 128 (0x0080) |                                |
| Hi-SCoA L5    | 4            |                                |
| LINESIZE (A4) | 592 (0x0250) | Bytes per raster line          |
| GoOnline magic | `ee db ea ad` | First 4 bytes of GoOnline payload (Windows: 16 bytes; Linux: 8 bytes) |
| Toner density default | `0x1f` per channel | Bits 5-2 = density value 7; same in Windows and Linux |
| A4 papertable_strange_dims | p1=410, p2=600, p3=510, p4=510 | Margin source values (in 1/1000 inch). MarginW=600×410/2540≈96px, MarginH=600×510/2540≈120px |
| A4 paper pixel dims | width=0x1360 (4960), height=0x1B66 (7014) | From papersize_res600_cntblmodel1, paper_id=9 (0x09) |
| A4 ImgHeight (Windows) | 6776 (0x1a78) | From lbp3000-windowsxp captures |
| A4 ImgHeight (Linux) | 6784 (0x1a80) | From lbp3000.pcap / parsed/lbp3000.txt; 8 lines more than Windows |

### Paper size table for LBP3000 (CNTblModel=1, 600 dpi — `papersize_res600_cntblmodel1`)

Paper names are taken from `off_8057BC0` (the paper-name → `paper_id` lookup table).
Note: A3 (paper_id=8) and B4 (paper_id=0x0C) are **not present** in this table —
those sizes are not supported by this printer model.
`width_bands`/`height_bands` are in points (1/72 inch); `width_pixels`/`height_pixels`
are at 600 dpi.

| paper_id | Name             | width_bands  | height_bands | width_pixels  | height_pixels  | paper_flag |
|----------|------------------|--------------|--------------|---------------|----------------|------------|
| 0x01     | Letter           | 0x0264 (612) | 0x0318 (792) | 0x13EC (5100) | 0x19C8 (6600)  | 0 |
| 0x05     | Legal            | 0x0264 (612) | 0x03F0 (1008)| 0x13EC (5100) | 0x20D0 (8400)  | 0 |
| 0x07     | Executive        | 0x020A (522) | 0x02F4 (756) | 0x10FE (4350) | 0x189C (6300)  | 0 |
| 0x09     | A4               | 0x0253 (595) | 0x034A (842) | 0x1360 (4960) | 0x1B66 (7014)  | 0 |
| 0x0B     | A5               | 0x01A4 (420) | 0x0253 (595) | 0x0DA8 (3496) | 0x1360 (4960)  | 0 |
| 0x0D     | B5               | 0x0204 (516) | 0x02D9 (729) | 0x10CA (4298) | 0x17B6 (6070)  | 0 |
| 0x2B     | Postcard         | 0x011B (283) | 0x01A4 (420) | 0x093A (2362) | 0x0DA8 (3496)  | 0 |
| 0x52     | Dbl Postcard     | 0x01A4 (420) | 0x0237 (567) | 0x0DA8 (3496) | 0x1274 (4724)  | 0 |
| 0x10CC   | 4x1 Postcard     | 0x0237 (567) | 0x0347 (839) | 0x1274 (4724) | 0x1B66 (7014)  | 0 |
| 0x5B     | Env Y4 (jenv_you4)| 0x0128 (296)| 0x029A (666) | 0x09B0 (2480) | 0x15AE (5550)  | 0 |
| 0x1F     | Env Y2 (jenv_you2)| 0x0143 (323)| 0x01CB (459) | 0x0A84 (2692) | 0x0EF2 (3826)  | 0 |
| 0x106    | Env Y2 (alt ID)  | 0x0143 (323) | 0x01CB (459) | 0x0A84 (2692) | 0x0EF2 (3826)  | 0 |
| 0x14     | Com10 (Env #10)  | 0x0129 (297) | 0x02AC (684) | 0x09AE (2478) | 0x1644 (5700)  | 0 |
| 0x1C     | Envelope C5      | 0x01CB (459) | 0x0289 (649) | 0x0EF2 (3826) | 0x1520 (5408)  | 0 |
| 0x1B     | Envelope DL      | 0x0138 (312) | 0x0270 (624) | 0x0A26 (2598) | 0x144C (5196)  | 0 |
| 0x25     | Monarch          | 0x0117 (279) | 0x021C (540) | 0x0918 (2328) | 0x1194 (4500)  | 0 |
| 0x22     | Envelope B5      | 0x01F3 (499) | 0x02C5 (709) | 0x1026 (4134) | 0x1712 (5906)  | 0 |
| 0x11F8   | Index 3×5        | 0x00D8 (216) | 0x0168 (360) | 0x0708 (1800) | 0x0BB8 (3000)  | 0 |

*Note: `paper_id` is the internal size identifier from `off_8057BC0` / `papertable_strange_dims`.
It maps to `PaperSzByte` (used in `D0A0` byte 5) via `tPaperSizeTbl`.*

---

## 8. Internal Driver Tables (from decompiled Canon CAPT Linux driver binary)

> **Source note:** The decompiled code in `info/decompiled-snippets.txt` is from a 32-bit
> Linux ELF binary (code addresses ~0x0804xxxx, data addresses ~0x0805-0x0808xxxx).
> The specific binary name is not stated in the snippets. Based on the function content
> (page setup, paper dimension tables, compression parameters for IC_BEGIN_PAGE),
> it is most likely `captfilter` (the CAPT page-rendering filter) or `ccpd` (the CAPT
> daemon), both part of the Canon proprietary CAPT driver suite for Linux.

### 8.1 Paper Source Table (`tPaperSourceTbl`)

Used by `inputslot_to_papersource()` to translate the CUPS/PostScript input slot
number into the paper source byte written into `D0A0` byte 7. Only applied when
`CNTblModel` != 0 (LBP3000 and later); older models always write 0.

| inputslot | papersource (D0A0 byte 7) |
|-----------|--------------------------|
| 7         | 0x00 (auto)              |
| 4         | 0x00 (auto)              |
| 1         | 0x01                     |
| 3         | 0x02                     |
| 2         | 0x03 (alt)               |
| 2         | 0x02 (alt)               |
| 0x108     | 0x04                     |
| (default) | 0x00                     |

LBP3000 in all observed captures uses auto-feed (papersource = 0x00).

### 8.2 Margin / Print-Area Dimension Tables (`papertable_strange_dims`)

The driver keeps per-model tables (`papertable_strange_dims_model1` for CNTblModel=1)
storing four margin source values `p1, p2, p3, p4` per paper ID, in 1/1000-inch
units (milliinches). Conversion to pixels at 600 dpi: `px = 600 × milliinches / 2540`.

```
paper_dim1 = 600 × p1 / 2540   used in height margin calc (top/left)
paper_dim2 = 600 × p2 / 2540   used in height margin calc (bottom/right)
paper_dim3 = 600 × p3 / 2540   used in width margin calc  (top/left)
paper_dim4 = 600 × p4 / 2540   used in width margin calc  (bottom/right)
```

The bind-edge shift (`CNBindEdgeShift`) optionally adds
`bindedgeshift = CNBindEdgeShift * resolution / 2540` to `paper_dim1` or
`paper_dim3` depending on `BindEdge` (1-4) and even/odd page parity.

**LBP3000 (CNTblModel=1) papertable_strange_dims key entries:**

| paper_id       | p1   | p2   | p3   | p4   | Notes                              |
|----------------|------|------|------|------|------------------------------------|
| 0x01 Letter    | 410  | 600  | 510  | 510  |                                    |
| 0x05 Legal     | 410  | 600  | 510  | 510  |                                    |
| 0x07 Executive | 410  | 600  | 510  | 510  |                                    |
| 0x09 A4        | 410  | 600  | 510  | 510  | MarginW=96px, MarginH=120px @600dpi |
| 0x0B A5        | 410  | 600  | 510  | 510  |                                    |
| 0x0D B5        | 410  | 600  | 510  | 510  |                                    |
| 0x2B Postcard  | 510  | 510  | 510  | 510  | equal margins all sides            |
| 0x52 Dbl Post. | 510  | 510  | 510  | 510  |                                    |
| 0x10CC 4x1     | 410  | 600  | 510  | 510  |                                    |
| 0x5B Env Y4    | 1000 | 1000 | 1000 | 740  | large envelope margins             |
| 0x1F Env Y2    | 1000 | 1000 | 1000 | 740  |                                    |
| 0x106          | 1000 | 1000 | 1000 | 740  |                                    |
| 0x14 Env #10   | 1000 | 1000 | 1000 | 1000 |                                    |
| 0x1C Env C5    | 1000 | 1000 | 1000 | 1000 |                                    |
| 0x1B Env DL    | 1000 | 1000 | 1000 | 1000 |                                    |
| 0x25 Env Mon.  | 1000 | 1000 | 1000 | 1000 |                                    |
| 0x11F8 custom  | 1000 | 1000 | 1000 | 1000 |                                    |


### 8.3 IC_BEGIN_PAGE (D0A0) Line-Size / Printable-Area Computation

`LineSize` (bytes 27-28) and `ImgHeight` (bytes 29-30) are computed from the
physical paper pixel dimensions minus the margin values, then aligned upward to
the next 32-pixel (4-byte) boundary:

```
width_printable  = paper_width_px  - paper_dim3 - paper_dim4 - bind_w_left - bind_w_right
height_printable = paper_height_px - paper_dim1 - paper_dim2 - bind_h_top  - bind_h_bottom

# Align up to 32-pixel boundary:
if (width_printable  & 0x1F) != 0: width_aligned  = (width_printable  | 0x1F) + 1
else:                               width_aligned  = width_printable
if (height_printable & 0x1F) != 0: height_aligned = (height_printable | 0x1F) + 1
else:                               height_aligned = height_printable

LineSize  = width_aligned  / 8   (bytes per raster line, written to D0A0 bytes 27-28)
ImgHeight = height_aligned       (raster lines, written to D0A0 bytes 29-30)
```

For A4 plain paper (paper_id=0x09, pixel dims 4960×7014), no bind-edge shift, at 600 dpi:
- `paper_dim3 = paper_dim4 = 600 * 510 / 2540 = 120 px`
- `width_printable = 4960 - 120 - 120 = 4720` → not 32-aligned → `4736`, LineSize=592
- `paper_dim1 = 600 * 410 / 2540 = 96 px`, `paper_dim2 = 600 * 600 / 2540 = 141 px`
- `height_printable = 7014 - 96 - 141 = 6777` → aligned to next 32-px boundary → 6784, but Windows observed 6776

*Note: the observed `ImgHeight=6776` is larger than the strict margin calculation
would give; the actual clipping may use fewer margin bits, or margins only apply
to the image area coordinate origin (`MarginH`/`MarginW`) and not to the height itself.*

---

## 9. Key Implementation Notes for Driver Authors

1. **Read `GetPrinterInfo` once** at startup to obtain `Blk` and `Buf`; size all subsequent `IC_VIDEO_DATA` transfers to ≤ `Blk` bytes.

2. **Tight GetBasicStatus polling** is expected and normal. The printer does not signal via interrupt; the driver must poll.

3. **Back-pressure via `BufLevel`**: `GetBasicStatus` bytes 5-6 carry the FIFO free-slot count (0–15). Reduce burst size as `BufLevel` drops; when `BufLevel == 0` — if chunks remain, issue `StartPrint(N)` first, then poll until BufLevel rises to ≥ 1, then send remaining chunk(s), then `IC_BLACK_END`. On LBP3000, `IM_DATA_BUSY` (byte 1 bit `0x08`) was **never observed to be set** — do not rely on it as the primary flow-control signal. `BufLevel == 0` during streaming is normal; wait for it to rise before continuing data transfer.

4. **`GoOffline` is conditional**: only send `GoOffline` if the printer is currently online (`Bas byte 1 bit 0x10` is clear). If the printer is already in the offline/uninitialized state (`Bas & 0x10` set), proceed directly to `ClearMisPrint` → `ClearError` → `DiscardData` → `GoOnline`.

5. **SetJobInfo2 job-end flag:** Windows uses `flag=6`; Linux uses `flag=3`. Both appear to work on LBP3000. The `flag=6` value is the Windows-canonical end-of-job marker; `flag=3` is the Linux driver's choice.

6. **Page numbering** in `StartPrint` must match `GetExtendedStatus.Start`. Wait for `Start` to increment before sending the next page's `D0A9` multi-command block.

7. **Post-job drain**: after `ReleaseUnit`, alternate `GetInputStatus` + `GetExtendedStatus` (do not rely on `GetBasicStatus` alone in this phase). Continue until `Printed == totalPages`.

8. **Hi-SCoA parameters** for LBP3000 mono: L0=0, L2=−7, L3=1, L4=128, L5=4. LINESIZE derived from image width field in `D0A0`.

9. **GetInputStatus** must be called whenever `GetExtendedStatus` Bas1 bit `0x02` is set; this typically happens when paper tray state changes (paper loaded or empty). Check byte 3 of the response: `0xd0`/`0xc0` = paper present, `0x80` = no paper. In the Linux capture this is also triggered mid-job (e.g. when `Printed` advances) — the Linux driver calls `GetInputStatus` whenever `Bas1 & 0x02` is set, which the printer asserts periodically during printing.

10. **Reply split**: replies > 6 bytes arrive in two USB transfers. The driver must issue two bulk-in reads for such responses.

11. **Out-of-paper detection**: triggered when `GetBasicStatus` byte 1 shows `RCF_NOTREADY | RCF_OFFLINE` (0x12 or 0x16), confirmed by `GetExtendedStatus` having `Cnt & 0x40` (`RCF_PRINT_REJECTED`) and `Pap == 0x00`. LBP3000 has **no tray sensor** — paper-out is discovered only when the engine fails to pick a sheet.

12. **Out-of-paper recovery**: the driver cannot detect paper insertion — it must poll and wait for the user to insert paper and press the **physical go button** on the printer. The button press causes the printer to set `Bas1 & 0x02`, prompting a `GetInputStatus` call. Only then does the driver see `GetInputStatus` byte 3 change from `0x80` → `0xc0` (engine ready). Recovery sequence: `ClearMisPrint` + `ClearError` + `DiscardData` → `GoOnline` (resets counters) → `SetLEDStatus(NoPaper)` → `GoOffline` → poll until button pressed → `SetLEDStatus(0)` → `ClearMisPrint` + `ClearError` + `DiscardData` → `GoOnline` → resume printing.

13. **JobID persistence**: the printer assigns incrementing job IDs via `ReserveUnit` — they do not reset to 1 for each new job. The driver must use whatever ID was returned.

14. **Page counters reset** to 0 after every `GoOnline` call during error recovery. The driver must track independently how many pages were successfully printed before the error to know which pages to reprint.

15. **Large-page streaming**: when `BufLevel` reaches 0 while page N data remains to be sent, issue `StartPrint(N)` immediately (at BufLevel=0), then poll until BufLevel rises to ≥ 1, then send remaining IC_VIDEO_DATA chunk(s), then send `IC_BLACK_END`. The streaming trigger is BufLevel=0 with pending data — **not** a fixed page size threshold. A 1.9 MB page may use normal mode; a 2.15 MB page may use streaming mode, depending on exactly when BufLevel reaches 0. Do not start page N+1 `IC_VIDEO_DATA` until page N's `IC_BLACK_END` is sent. See §2.18a for the full streaming sequences and counter behaviour.

16. **`SetJobInfo2(flag=2)` timing**: In the Windows driver, the mid-job continuation marker is sent once when `GetExtendedStatus.Printed` first becomes ≥ 1. In the Linux driver, `flag=2` is sent as a repeated heartbeat throughout the entire job (every ~10–15 polling cycles, paired with `SetLEDStatus(0x17)`), independent of page completion state.

17. **Linux `GoOffline` payload carries JobID**: the Linux driver passes the current `JobID` (uint16 LE) as the 2-byte payload to `GoOffline` when terminating a job. The Windows driver always sends `00 00`. Both are accepted by the printer.

18. **Linux init clear order**: Linux sends `DiscardData` → `ClearMisPrint` → `ClearError` (before `GoOnline`), the reverse of the Windows order (`ClearMisPrint` → `ClearError` → `DiscardData`). Both work. The Linux driver also skips `GoOffline` in initialisation because the printer is already offline at startup.

19. **Linux GoOnline payload is 8 bytes**: Linux sends only `ee db ea ad 00 00 00 00` (8 bytes). The Windows driver sends 16 bytes. The printer accepts both.

20. **Linux chunk sizes**: The Linux driver sends very small `IC_VIDEO_DATA` chunks (as small as 208 bytes, typical 228 bytes) and polls `GetExtendedStatus` after every single chunk. This is far more conservative than the Windows driver (up to 65284-byte chunks, polling every ~5 chunks). As a result, the buffer never fills in the Linux capture (`BufLevel` is always `0x000f`). The streaming mode (§2.18a) is therefore not observed in Linux captures for the LBP3000.
