# SM8975 LPDDR6 주소·패킷 매핑

이 문서는 `stressapptest`의 `--dram-map sm8975-lp6` 옵션이 Qualcomm SM8975 LPDDR6 장비에서 memory fail을 어떻게 DRAM 좌표와 DQ/BL/HEX 위치로 변환하는지 설명합니다.

> **중요:** 과거의 `--dram-map lpddr-v1` 매핑은 제거했습니다. 그 식은 QC SM8975 LPDDR6 address routing과 일치하지 않았습니다. `lpddr-v1`은 더 이상 지원되는 프로필이 아니며 직접 지정하면 프로그램이 실패합니다.

구현 기준은 `stpcoder/lpddr6-packet-mapper`의 현재 `sm8975_decode.py`와 LPDDR6 normal packet assignment입니다. Stressapptest가 별도의 추정식을 유지하지 않고 같은 계산 계약을 사용하도록 구성합니다.

## 실행

```bash
adb shell '/data/local/tmp/stressapptest \
  -M 1024 -s 600 -m 4 -i 4 \
  -P OneZero256 \
  --dram-map sm8975-lp6'
```

`--dram-map`을 생략하거나 `none`을 지정하면 기존 memory mismatch 자체는 그대로 출력하지만 SM8975 LPDDR6 mapping block을 생성하지 않습니다.

## 전체 변환 흐름

```text
stressapptest virtual fail address
        ↓ /proc/self/pagemap
system physical address (PA)
        ↓ SM8975 QC base removal
36-bit normalized controller address A[35:0]
        ↓ SM8975 topology equations
CH / CS / SC / BK / ROW / MAT / COL

expected 64-bit + read 64-bit + PA
        ↓ lower/upper 32-bit mapper row로 분리
WR XOR RD mismatch bit
        ↓ 32-byte LPDDR6 normal packet 위치
ordered (DQ, BL) pair
        ↓ pair별 region lookup
HEX
```

여기서 `WR`은 stressapptest의 `expected`, `RD`는 최초 mismatch 검사에서 읽은 `read`에 대응합니다. `reread`는 오류 재확인 정보이며 DQ/BL 계산의 기준값으로 사용하지 않습니다.

## 1. Physical address 기준

Stressapptest는 오류가 포함된 64-bit word의 virtual address를 `/proc/self/pagemap`으로 변환하여 system physical address를 얻습니다. Android 보안 정책 때문에 PFN을 읽을 수 없으면 physical address가 0이 될 수 있으며, 이 경우 SM8975 mapping을 확정할 수 없습니다.

```text
VA → page table / pagemap → PA → SM8975 map
```

SM8975 식에는 virtual address를 직접 넣지 않습니다.

## 2. SM8975 address normalization

`lpddr6-packet-mapper`와 동일하게 두 QC address window를 하나의 36-bit controller address로 정규화합니다.

```text
raw PA < 0x100000000:
    A = PA - 0x80000000

raw PA >= 0x100000000:
    A = PA - 0x800000000

A = A & 0xFFFFFFFFF
```

예:

```text
0x80000000  → 0x000000000
0x80000020  → 0x000000020
0x800000000 → 0x000000000
0x800000020 → 0x000000020
```

이 base removal이 과거 `lpddr-v1`과의 가장 근본적인 차이 중 하나입니다. 과거 식은 system PA를 그대로 DRAM bit로 소비했기 때문에 QC memory-window base bit가 ROW/BG 등의 실제 routing bit처럼 섞일 수 있었습니다.

## 3. CH / CS / SC / BK / ROW / COL

아래 `A[n]`은 normalization 이후 36-bit controller address의 bit n입니다. `^`는 XOR입니다.

### CH

```text
CH0 = A8
CH1 = A9 ^ A11 ^ A13 ^ A15 ^ A17 ^ A19
CH  = (CH1 << 1) | CH0
```

### CS

```text
CS = A32 ^ A33
```

과거 `lpddr-v1`은 Rank를 무조건 0으로 출력했습니다. `sm8975-lp6`에서는 CS를 실제 식으로 계산합니다.

### SC

```text
SC = A10 ^ A11 ^ A12 ^ A13
```

### BK

SM8975 LPDDR6 mapping은 별도의 BG + 2-bit Bank 모델을 사용하지 않고 4-bit `BK`를 계산합니다.

```text
BK0 = A16 ^ A19 ^ A20 ^ A22 ^ A24 ^ A25 ^ A26 ^ A27 ^ A31
BK1 = A17 ^ A20 ^ A21 ^ A23 ^ A25 ^ A26 ^ A27 ^ A28 ^ A32
BK2 = A7  ^ A18 ^ A21 ^ A22 ^ A24 ^ A26 ^ A27 ^ A28 ^ A29
BK3 = A13 ^ A18 ^ A19 ^ A21 ^ A23 ^ A24 ^ A25 ^ A26 ^ A30

BK = (BK3 << 3) | (BK2 << 2) | (BK1 << 1) | BK0
```

따라서 `BK` 범위는 0~15입니다. 검증된 별도 BG 식이 없으므로 `sm8975-lp6` 결과에 임의의 BG를 만들지 않습니다.

### ROW

```text
ROW bit 0  = A18
...
ROW bit 14 = A32
```

즉 ROW는 15-bit 값입니다.

### COL

```text
COL = (A15 << 5)
    | (A14 << 4)
    | (A12 << 3)
    | (A11 << 2)
    | (A6  << 1)
    | A5
```

## 4. MAT

MAT는 최종 ROW에서 계산합니다.

```text
ROW 0x000-0x55F   → MAT 0
ROW 0x560-0xABF   → MAT 1
ROW 0xAC0-0x101F  → MAT 2
ROW 0x1020-0x153F → MAT 3
ROW 0x1540-0x1A9F → MAT 4
ROW 0x1AA0-0x1FFF → MAT 5
ROW 0x2000-0x255F → MAT 6
ROW 0x2560-0x2ABF → MAT 7
ROW 0x2AC0-0x301F → MAT 8
ROW 0x3020-0x353F → MAT 9
ROW 0x3540-0x3A9F → MAT 10
ROW 0x3AA0-0x3FFF → MAT 11
ROW 0x4000-0x455F → MAT 12
ROW 0x4560-0x4ABF → MAT 13
ROW 0x4AC0-0x501F → MAT 14
ROW 0x5020-0x553F → MAT 15
ROW 0x5540-0x5A9F → MAT 16
ROW 0x5AA0-0x5FFF → MAT 17
ROW 0x6000-0x655F → MAT 18
ROW 0x6560-0x6ABF → MAT 19
ROW 0x6AC0-0x701F → MAT 20
ROW 0x7020-0x753F → MAT 21
ROW 0x7540-0x7A9F → MAT 22
ROW 0x7AA0-0x7FFF → MAT 23
```

범위 밖 ROW는 consistency signal로 `-1`을 사용합니다.

## 5. 왜 64-bit fail을 32-bit 두 행으로 나누는가

Stressapptest는 상세 mismatch를 64-bit word 단위로 출력합니다.

```text
read:     64 bit
expected: 64 bit
```

반면 `lpddr6-packet-mapper`는 이 값을 다음처럼 처리합니다.

```text
lower 32 bit → address = word_base + 0
upper 32 bit → address = word_base + 4
```

각 32-bit 값에서 `expected XOR read`가 0이 아니면 하나의 mapper row가 됩니다.

이 분리를 유지하는 이유는 64-bit word가 32-byte packet 또는 COL 경계를 걸칠 수 있기 때문입니다. 두 half를 하나의 topology로 뭉치면 upper 32-bit의 정확한 ADDR/CH/CS/SC/BK/ROW/COL 결과를 잃을 수 있습니다.

따라서 `sm8975-lp6` fail log의 `lp6:[...]`에는 실제로 fail한 32-bit half마다 별도 block을 기록합니다.

## 6. LPDDR6 32-byte packet과 DQ / BL

LPDDR6 normal packet 기준:

```text
DQ lane = 12개 (DQ0..DQ11)
BL beat = 24개 (BL0..BL23)
physical grid = 12 × 24 = 288 slots
user data = 256 bits = 32 bytes
```

나머지 32 slot은 packet mode에 따라 FIXL/system metadata/DBI/link-protection 영역이 됩니다. 현재 stressapptest의 `sm8975-lp6`은 `lpddr6-packet-mapper`의 **normal mode user-data assignment**를 그대로 사용합니다.

한 32-bit mapper row에 대해:

```text
packet_base = ADDR - (ADDR % 32)
word_index  = (ADDR - packet_base) / 4

data_bit = word_index × 32 + mismatch_bit
```

그 `data_bit`를 mapper의 packet assignment 순서에 넣어 정확한 `(DQ, BL)` pair 하나를 얻습니다.

### pair 순서는 보존한다

DQ와 BL은 독립 set가 아닙니다.

예:

```text
pairs = [(1,1), (1,2), (2,3)]
DQ    = [1,1,2]
BL    = [1,2,3]
```

다음처럼 독립적으로 dedupe하지 않습니다.

```text
DQ = [1,2]
BL = [1,2,3]   # 잘못된 표현
```

각 index의 DQ와 BL이 하나의 물리 pair입니다.

## 7. HEX

HEX도 각 `(DQ, BL)` pair에서 하나씩 계산합니다.

```text
BL_group = BL // 4
DQ_group = DQ // 2
HEX      = table[BL_group][DQ_group]
```

```text
             DQ group
             0-1  2-3  4-5  6-7  8-9  10-11
BL 0-3        0    1    2    3    4     5
BL 4-7        6    7    8    9   10    11
BL 8-11      12   13    -   14   15     -
BL 12-15     16   17   18   19   20    21
BL 16-19     22   23   24   25   26    27
BL 20-23     28   29    -   30   31     -
```

`-` 위치는 normal packet에서 user-data slot이 아니므로 코드에서는 `-1`을 consistency signal로 사용합니다.

## 8. 실제 fail log 형식

기존 stressapptest의 핵심 로그 정보는 유지합니다.

```text
physical address
read
reread
expected
worker / phase
DDR frequency
```

`--dram-map sm8975-lp6`을 켜면 기존 topology 문자열 대신 다음 block이 들어갑니다.

```text
map:sm8975-lp6,lp6:[
  {addr:0x...,norm:0x...,ch:0x...,cs:0x...,sc:0x...,bk:0x...,
   row:0x...,mat:...,col:0x...,dq:[...],bl:[...],hex:[...]}
]
```

64-bit word의 lower와 upper 32-bit가 모두 fail이면 `lp6:[...]` 안에 두 block을 `;`로 구분하여 기록합니다.

예시 구조:

```text
Hardware Error: miscompare ... at <VA>(0x90A5EDC3C:DIMM Unknown):
read:0x..., reread:0x..., expected:0x....
...,
map:sm8975-lp6,lp6:[
 {addr:0x90A5EDC38,norm:0x10A5EDC38,ch:0x0,cs:0x1,sc:0x1,bk:0xF,
  row:0x4297,mat:12,col:0x3D,dq:[...],bl:[...],hex:[...]}
],
cur_mode:..., cur_freq:..., ddr_freq(...).
```

실제 값은 fail word와 address에 따라 달라집니다.

## 9. 대표 cross-check

과거 문서에서 사용했던 physical address:

```text
0x90A5EDC3C
```

SM8975 LP6 normalization:

```text
0x90A5EDC3C - 0x800000000 = 0x10A5EDC3C
```

현재 식의 topology:

```text
CH  = 0
CS  = 1
SC  = 1
BK  = 15 (0xF)
ROW = 0x4297
MAT = 12
COL = 0x3D
```

과거 `lpddr-v1`은 같은 주소를 `RK=0`, `BG=1`, `BANK=2`, `COL=0x29` 등으로 해석했습니다. 해당 결과는 QC SM8975 LPDDR6 mapping과 일치하지 않으므로 제거했습니다.

## 10. 소스 위치

```text
src/sm8975_mapping.h
  NormalizeSm8975Address()
  DecodeSm8975Address()
  Sm8975MatForRow()
  Sm8975DataBitToCoordinate()
  MapSm8975MismatchWord32()
  Sm8975HexRegion()

src/dram_address.h
  stressapptest topology adapter
  legacy lpddr-v1 equations removed

src/logger.cc
  64-bit fail → 32-bit mapper rows
  CH/CS/SC/BK/ROW/MAT/COL + DQ/BL/HEX fail-line formatting

src/main.cc
  public option: --dram-map sm8975-lp6
  removed option: --dram-map lpddr-v1

src/dram_address_test.cc
  normalization/topology/MAT/DQ-BL-HEX cross-check vectors
```

## 11. 분석할 때의 주의점

`sm8975-lp6`이 계산하는 것은 **주어진 system physical address와 LPDDR6 packet assignment에 대한 deterministic mapping**입니다. 이 값이 실제 장비와 일치하려면 다음 전제가 맞아야 합니다.

- 오류 로그의 PA가 실제 memory transaction의 system physical address와 대응함
- 대상이 QC SM8975이며 현재 측정한 LPDDR6 routing을 사용함
- boot/DMC 설정이 mapping을 바꾸지 않았음
- LPDDR6 packet interpretation이 mapper의 normal-mode data assignment와 일치함

주소 매핑 자체의 검증은 알려진 address → CH/CS/SC/BK/ROW/COL bench capture와 비교하고, DQ/BL은 known injected mismatch 또는 logic-analyzer capture와 비교합니다.
