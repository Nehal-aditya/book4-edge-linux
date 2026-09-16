# EC2.sys command table and the charging command namespace

Findings from `EC2.sys` / `EmuEc.sys`, the Windows EC drivers.

## 1. Command descriptor table (EC2.sys)

`.data` VA `0x14000d5b0` (file offset `0xb7b0`), 4 bytes per entry, index = `cmd - 0x80`.

Dispatcher at `0x140005170`:

    ldrb w22, [x8]        ; w22 = command byte, from a caller-supplied buffer
    cmp  w22, #0x80
    b.cc 0x1400053d0      ; < 0x80 takes a different path entirely
    sub  x8, x22, #0x80
    lsl  x26, x8, #2      ; 4-byte entries
    ldrb w20, [x26, x9+1] ; byte1
    ldrb w25, [x26, x9+2] ; byte2 -> bound of the 0xF480 payload loop at 0x1400052a8

byte0 echoes the command id; byte1/byte2 are payload lengths.

Validated against the two commands we independently proved work over I2C:

| cmd | b1 | b2 | known behaviour |
|---|---|---|---|
| 0x88 | 1 | 1 | read EC space: send offset, get value |
| 0x89 | 2 | 0 | write EC space: send offset+value, no reply |

0x86 and 0x87 are both `0 0`: they carry no payload at all. Every
`raw_excmd 86 …` / `87 …` attempt was structurally impossible, which explains the
inertness without needing any other theory.

Commands that do carry data: 0x8D (2/16), 0x83 (0/9), 0xA6 (7/4), 0xB5 (5/5),
0x8C (2/2), 0xD0 (2/2), 0xE9 (2/3), 0xEF (5/5), 0xAF (2/1), 0x81 (3/0), 0x90 (2/0),
0x9F (2/0), 0xA3 (2/1), 0xFC (0/1).

## 2. The charging commands are a different namespace (EmuEc.sys)

`mboxLiCACheckChargingCurrent` (~`0x14002f800`) uses small logical ids, not 0x80+:

| name | id |
|---|---|
| `CMD_CHG_CUR`   | 0x14 |
| `CMD_CHG_STATE` | 0x20 |
| `CMD_INP_CUR`   | 0x3f |

Write path, setting charge current to 1808 mA:

    mov w0, #0x14     ; CMD_CHG_CUR
    mov w1, #0x710    ; 1808
    mov w2, #0x2      ; length
    bl  0x14002dc90   ; -> 0x140007750(bank=2, cmd, buf, len)

`0x140007750` dispatches through a handle table at `0x140054580`, slot `0x19`,
an object call, not raw I2C.

`EmuEc.sys` imports `ACPI.SYS`, not `EC2.sys`. It reaches the EC through ACPI
operation regions; `EC2.sys` is the region handler that turns a region access into
an I2C mailbox command. `EC2.sys` registers address-space handlers at `0x140003150`
onward: one region id read from a device property (handler `0x140001e20`), then
0xA1 (`0x140002020`) and 0xA2 (`0x140002150`).

## 3. BMOP layout (DSDT, region 0x9E, DWordAcc)

| off | field | | off | field |
|---|---|---|---|---|
| 0x00 | SOC  | | 0x20 | CHGC charge current |
| 0x04 | FCCP | | 0x24 | STPC charge limit |
| 0x08 | CHST charge status | | 0x28 | GADC |
| 0x0C | RMCP | | 0x2C | TTEM |
| 0x10 | VOLT | | 0x30 | TTCH |
| 0x14 | BATP | | 0x34 | CYCL |
| 0x18 | SRP0 | | 0x38 | BTP |
| 0x1C | STYP | | 0x3C | RSV1 |

`STPC` at 0x24 corroborates the offset independently established earlier, so the
layout is trustworthy.

## 4. Where this leaves the charging bug

Named hypothesis, replacing the earlier vague one: the EC wedges with input/charge
current at zero when the adapter is attached while running. `CHGC` (BMOP 0x20) and
`CMD_INP_CUR` are the fields Windows drives.

The gap is reaching region 0x9E from Linux. We have 0xA1 (EC RAM) via 0x88/0x89;
the 0x80+ opcode pair for 0x9E is not yet identified.

Safe next probe (reads only, cannot damage anything): sweep candidate read
opcodes and check BMOP 0x00 against the real battery percentage and 0x34 against the
real cycle count. Those two are hard falsification criteria: a wrong mapping fails
them, as an earlier guess already did.
