# SM8975 LP6 implementation status

Status: implementation contract for `agent/qc-sm8975-fail-mapping` and its merge to `master`.

## Public behavior

- Supported mapping option: `--dram-map sm8975-lp6`
- Default: no DRAM mapping (`none`)
- Removed option: `--dram-map lpddr-v1`
- Direct use of the removed option fails instead of silently applying an old mapping.

## Mapping source of truth

The implementation mirrors the current `stpcoder/lpddr6-packet-mapper` QC SM8975 path:

1. system physical address normalization
2. CH / CS / SC / BK / ROW / MAT / COL topology equations
3. stressapptest 64-bit fail split into lower/upper 32-bit mapper rows
4. `expected XOR read` mismatch-bit identification
5. LPDDR6 normal-mode 32-byte packet assignment
6. ordered DQ/BL pairs
7. HEX lookup per exact DQ/BL pair

There is no independently verified SM8975 BG equation in the source mapper, so the public SM8975 LP6 result does not invent a BG value.

## Output contract

The original stressapptest fail prefix remains parseable:

```text
at <virtual>(0x<physical>:...): read:0x..., reread:0x..., expected:0x...
```

With `--dram-map sm8975-lp6`, the former generic topology block is replaced by:

```text
map:sm8975-lp6,lp6:[
  {addr:0x...,norm:0x...,ch:0x...,cs:0x...,sc:0x...,bk:0x...,
   row:0x...,mat:...,col:0x...,dq:[...],bl:[...],hex:[...]}
]
```

If both 32-bit halves of one stressapptest 64-bit word fail, both mapper rows are emitted in order.

## Regression vectors

- `0x80000000` -> normalized `0x000000000`, CH0/CS0/SC0/BK0/ROW0/MAT0/COL0
- `0x800000020` -> normalized `0x000000020`
- `0x90A5EDC3C` -> normalized `0x10A5EDC3C`, CH0/CS1/SC1/BK15/ROW0x4297/MAT12/COL0x3D
- mismatch data bits 5, 6, 11 -> `(DQ,BL,HEX)` = `(1,1,0)`, `(1,2,0)`, `(2,3,1)`

## Validation gates

The repository CI must verify:

- Linux host build
- standalone SM8975 topology/MAT/DQ-BL-HEX vectors
- `--dram-map sm8975-lp6` is accepted
- removed `--dram-map lpddr-v1` is rejected
- injected memory failure produces `map:sm8975-lp6,lp6:` output
- Android ARM64 build and ABI checks
- documentation build
