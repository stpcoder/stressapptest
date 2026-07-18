# 메모리에 쓰는 데이터 pattern

pattern은 메모리에 실제로 기록하는 데이터입니다. stressapptest는 bit 배열이 서로 다른 pattern을 사용하여 특정 bit 고정, 인접 bit 간섭, 잘못된 주소 접근과 같은 오류가 드러나도록 합니다.

## Pattern을 사용하는 이유

stressapptest는 bit transition과 반복 구조가 서로 다른 pattern family를 사용한다. 각 family는 다음 오류 형태를 검출하기 위한 데이터 조건을 제공한다.

- 특정 bit가 0 또는 1로 고정되는 stuck-at 성향
- 인접 bit coupling
- alternating data에서 증가하는 switching
- 주소/경로 misrouting으로 다른 block의 pattern이 나타나는 현상
- read/write 과정에서 일부 bit 또는 burst가 바뀌는 현상

pattern은 CPU virtual address에 기록되는 데이터 word를 정의한다. physical DQ, CA, bank 및 row transition은 cache write-back, DMC address mapping, PHY swizzle 및 LPDDR protocol 처리 결과로 결정된다.

<sub><em>Pattern family: 동일한 생성 규칙을 공유하는 기본 데이터 배열의 집합입니다.</em></sub><br>
<sub><em>Bit transition: 데이터 bit가 0에서 1 또는 1에서 0으로 변경되는 동작입니다.</em></sub><br>
<sub><em>DQ: LPDDR device와 controller 사이에서 데이터를 전송하는 physical signal입니다.</em></sub><br>
<sub><em>CA: LPDDR command와 address 정보를 전송하는 physical signal입니다.</em></sub>

## Pattern variant 만드는 방법

각 pattern family는 다음 조합을 만든다.

```text
원본 / bitwise inverted
× 32 / 64 / 128 / 256 logical width
```

15 family × 2 polarity × 4 width = 120 object다. weight 0 variant는 선택되지 않는다.

> **파일:** `src/pattern.cc` · **함수:** `PatternList::Initialize()` · **기준:** `73b9df2`

```cpp
patterns_.resize(pattern_array_size * 8);
for (int i = 0; i < pattern_array_size; i++) {
  patterns_[patterncount++].Initialize(pattern_array[i], 32, false,
                                       pattern_array[i].weight[0]);
  patterns_[patterncount++].Initialize(pattern_array[i], 64, false,
                                       pattern_array[i].weight[1]);
  patterns_[patterncount++].Initialize(pattern_array[i], 128, false,
                                       pattern_array[i].weight[2]);
  patterns_[patterncount++].Initialize(pattern_array[i], 256, false,
                                       pattern_array[i].weight[3]);
  // 같은 네 width를 invert=true로 다시 생성한다.
}
```

**해석:** 각 family는 8개 object로 확장됩니다. 네 width의 원본과 네 width의 bitwise inverse가 생성되며 각 object에 독립적인 선택 weight와 4 KiB expected checksum이 저장됩니다.

### Width가 뜻하는 범위

`Pattern::pattern(offset)`은 `offset >> busshift`로 32-bit pattern word 반복 수를 바꾼다.

| 이름 suffix | 같은 32-bit word 반복 |
|---|---:|
| 32 | 1회 |
| 64 | 2회 |
| 128 | 4회 |
| 256 | 8회 |

이 width는 pattern 함수에서 동일한 32-bit word를 반복하는 논리적 범위다. LPDDR physical channel width, burst width 및 DQ width는 별도의 hardware 속성으로 관리된다. DMC interleave, cache-line assembly, endian 및 bus packing은 이후 hardware 경로에서 적용된다.

<sub><em>Logical width: 동일한 32-bit pattern word가 반복되는 byte 배열의 범위입니다.</em></sub><br>
<sub><em>Burst width: 하나의 DRAM read/write command가 전송하는 데이터 구성을 나타내며 device width와 burst length로 결정됩니다.</em></sub>

## 사용하는 pattern과 선택 확률

총 selection weight는 inverted variant까지 포함해 160이다.

| Family | 대표 32-bit 값/구조 | width weight 32/64/128/256 | 전체 확률 |
|---|---|---:|---:|
| `walkingOnes` | 한 개의 1 bit가 LSB→MSB→LSB 이동 | 1/1/2/1 | 6.25% |
| `walkingInvOnes` | walking one과 그 inverse를 교대 | 2/2/5/5 | 17.50% |
| `walkingZeros` | 한 개의 0 bit가 이동 | 1/1/2/1 | 6.25% |
| `OneZero` | `00000000`, `ffffffff` 교대 | 5/5/15/5 | 37.50% |
| `JustZero` | `00000000` 반복 | 2/0/0/0 | 2.50% |
| `JustOne` | `ffffffff` 반복 | 2/0/0/0 | 2.50% |
| `JustFive` | `55555555` 반복 | 2/0/0/0 | 2.50% |
| `JustA` | `aaaaaaaa` 반복 | 2/0/0/0 | 2.50% |
| `FiveA` | `55555555`, `aaaaaaaa` | 1/1/1/1 | 5.00% |
| `FiveA8` | `5aa5a55a`, `a55a5aa5` 조합 | 1/1/1/1 | 5.00% |
| `Long8b10b` | `16161616` 반복 | 2/0/0/0 | 2.50% |
| `Short8b10b` | `b5b5b5b5` 반복 | 2/0/0/0 | 2.50% |
| `Checker8b10b` | `b5b5b5b5`, `4a4a4a4a` | 1/0/0/1 | 2.50% |
| `Five7` | `55555557`, `55575555` | 0/2/0/0 | 2.50% |
| `Zero2fd` | `00020002`, `fffdfffd` | 0/2/0/0 | 2.50% |

각 확률에는 원본과 inverted variant가 모두 포함된다. `OneZero` family의 전체 확률은 37.5%이며, 원본/반전 및 width는 각 variant weight에 따라 선택된다.

<sub><em>Inverted variant: 원본 pattern의 각 bit에 bitwise NOT을 적용한 데이터 배열입니다.</em></sub><br>
<sub><em>Selection weight: 전체 weight 합에서 특정 pattern variant가 선택될 상대 비율입니다.</em></sub>

> **파일:** `src/pattern.cc` · **함수:** `PatternList::GetRandomPattern()` · **기준:** `73b9df2`

```cpp
int target = random();
unsigned int i = 0;
target = (target % weightcount_) + 1;

do {
  target -= patterns_[i].weight();
  if (target <= 0)
    break;
  i++;
} while (i < size_);

if (i < size_) {
  return &patterns_[i];
}
```

**해석:** random 값은 전체 weight 합의 범위로 변환됩니다. 각 pattern weight를 차감하다가 0 이하가 되는 object가 선택됩니다. weight 10은 weight 5보다 이 함수에서 선택될 확률이 두 배입니다.

## Width별 pattern 분포

| logical width | 확률 |
|---|---:|
| 32 | 30.00% |
| 64 | 18.75% |
| 128 | 32.50% |
| 256 | 18.75% |

OneZero128의 weight가 크므로 전체 선택에서 alternating all-zero/all-one 계열의 128-bit 반복이 특히 자주 나타난다.

## Block에 pattern을 지정하는 시점

초기 FillThread가 empty block을 얻을 때 `GetRandomPattern()`을 한 번 호출한다. 선택된 하나의 Pattern variant가 1 MiB block 전체에 반복된다.

```text
1 MiB block A → OneZero128
1 MiB block B → walkingInvOnes~256
1 MiB block C → JustFive32
```

CopyThread는 source bytes와 함께 `pattern` metadata를 destination으로 전달한다. Copy 후 destination은 같은 expected pattern을 가진다.

## Expected checksum

각 Pattern variant는 초기화할 때 첫 4 KiB에 해당하는 modified-Adler checksum을 미리 계산한다 (`src/pattern.cc:246`). Pattern이 주기적으로 반복되기 때문에 1 MiB 안의 각 4 KiB slice에 같은 expected checksum을 적용할 수 있다.

Tag mode에서는 cache line 첫 8 B가 주소 tag로 바뀌므로 주소-aware checksum 함수를 사용한다.

## `--tag_mode`

Tag mode는 각 64 B cache line의 첫 8 B에 해당 virtual address에서 만든 tag를 저장한다 (`src/worker.cc:490`). 나머지 word는 pattern을 유지한다.

목적:

- 다른 주소의 cache line이 잘못 전달된 경우
- address alias/misrouting 성격의 corruption
- data pattern 자체는 맞지만 wrong-location data가 들어온 경우

를 구분하는 데 도움을 준다.

Tag mode는 cacheable memory access를 유지하면서 address-derived tag를 데이터에 포함한다. disk/network DMA option과 함께 지정하면 compatibility 검사에서 초기화가 실패한다.

<sub><em>Address tag: cache line의 현재 virtual address에서 계산하여 해당 line의 첫 8 B에 저장하는 식별값입니다.</em></sub>

## Pattern 검증 범위

Pattern API는 CPU가 load/store하는 byte/word 데이터 배열을 정의한다. 특정 LPDDR DQ bit의 pin-level transition은 다음 hardware 요소를 모두 적용한 결과로 결정된다.

- cache-line allocation/writeback
- NoC packetization
- DMC data swizzle/interleave
- channel/rank/bank 선택
- LPDDR burst organization
- DBI, ECC, encryption 등 SoC/device 기능

따라서 pattern의 pin-level 의미가 필요하면 vendor DMC configuration과 PHY mapping을 결합해야 한다.
