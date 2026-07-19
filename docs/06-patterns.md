# 메모리에 기록하는 테스트 데이터

Pattern은 stressapptest가 메모리에 기록하는 테스트 데이터입니다. 서로 다른 bit 배열을 반복해서 기록하여 특정 bit가 한 값으로 고정되는 오류, 인접 bit의 간섭, 다른 주소의 데이터가 전달되는 오류를 검사합니다.

## Pattern을 사용하는 이유

stressapptest는 bit가 바뀌는 순서와 반복 구조가 서로 다른 pattern 묶음을 사용합니다. 각 pattern은 다음 오류를 발견하기 위한 데이터 조건을 만듭니다.

- 특정 bit가 0 또는 1로 고정되는 stuck-at 오류
- 인접 bit가 서로 영향을 주는 coupling 오류
- 0과 1이 교대로 기록될 때 발생하는 switching 오류
- 주소 또는 데이터 전달 경로가 잘못되어 다른 block의 pattern이 나타나는 오류
- 읽기·쓰기 과정에서 일부 bit 또는 burst가 바뀌는 오류

Pattern은 CPU가 virtual address에 기록할 데이터 word를 정의합니다. 실제 DQ·CA 신호와 bank·row의 변화는 cache write-back, DMC 주소 배치, PHY 배선, LPDDR protocol을 모두 거친 결과로 결정됩니다.

<sub><em>Pattern family: 동일한 생성 규칙을 공유하는 기본 데이터 배열의 집합입니다.</em></sub>
<sub><em>Bit transition: 데이터 bit가 0에서 1 또는 1에서 0으로 변경되는 동작입니다.</em></sub>
<sub><em>DQ: LPDDR device와 controller 사이에서 데이터를 전송하는 physical signal입니다.</em></sub>
<sub><em>CA: LPDDR command와 address 정보를 전송하는 physical signal입니다.</em></sub>

## Pattern을 만드는 방법

각 기본 pattern은 다음 조합으로 확장됩니다.

```text
원본 / 모든 bit를 반전한 값
× 32 / 64 / 128 / 256 bit 반복 범위
```

기본 pattern 15개에 원본·반전 2종과 반복 범위 4종을 적용하여 120개의 `Pattern` 객체를 만듭니다. 선택 비율이 0인 조합은 실제 시험에서 선택하지 않습니다.

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
  // 같은 네 반복 범위를 invert=true로 다시 생성한다.
}
```

**코드 설명:** 각 기본 pattern에서 반복 범위 4종의 원본과 반전 pattern 4종을 만듭니다. 각 `Pattern` 객체에는 선택 비율과 4 KiB 단위의 기대 checksum이 저장됩니다.

### Width가 나타내는 반복 범위

`Pattern::pattern(offset)`은 `offset >> busshift`로 32-bit pattern word 반복 수를 바꾼다.

| 이름 suffix | 같은 32-bit word 반복 |
|---|---:|
| 32 | 1회 |
| 64 | 2회 |
| 128 | 4회 |
| 256 | 8회 |

이 width는 pattern 함수에서 동일한 32-bit word를 반복하는 논리적 범위다. LPDDR physical channel width, burst width 및 DQ width는 별도의 hardware 속성으로 관리된다. DMC interleave, cache-line assembly, endian 및 bus packing은 이후 hardware 경로에서 적용된다.

<sub><em>Logical width: 동일한 32-bit pattern word가 반복되는 byte 배열의 범위입니다.</em></sub>
<sub><em>Burst width: 하나의 DRAM read/write command가 전송하는 데이터 구성을 나타내며 device width와 burst length로 결정됩니다.</em></sub>

## Pattern 종류와 선택 확률

반전 pattern까지 포함한 전체 선택 비율의 합은 160입니다.

| Pattern 이름 | 대표 32-bit 값 또는 구조 | 반복 범위별 선택 비율 32/64/128/256 | 전체 선택 확률 |
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

표의 확률에는 원본과 반전 pattern이 모두 포함됩니다. `OneZero`의 전체 선택 확률은 37.5%입니다. 원본·반전 여부와 반복 범위는 각 조합에 지정된 선택 비율에 따라 정해집니다.

<sub><em>Inverted variant: 원본 pattern의 각 bit에 bitwise NOT을 적용한 데이터 배열입니다.</em></sub>
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

**코드 설명:** 임의 값을 전체 선택 비율의 합인 160 안의 값으로 바꿉니다. 각 pattern의 선택 비율을 차례로 빼다가 0 이하가 되는 `Pattern` 객체를 선택합니다. 선택 비율이 10인 pattern은 5인 pattern보다 선택 확률이 두 배입니다.

## Width별 선택 비율

| 반복 범위 | 선택 확률 |
|---|---:|
| 32 | 30.00% |
| 64 | 18.75% |
| 128 | 32.50% |
| 256 | 18.75% |

`OneZero128`의 선택 비율이 크기 때문에, 0과 1을 교대로 구성한 128-bit 반복 pattern이 특히 자주 선택됩니다.

## Block에 Pattern을 기록하는 과정

초기 `FillThread`가 empty block을 가져올 때 `GetRandomPattern()`을 한 번 호출합니다. 선택한 하나의 `Pattern`을 1 MiB block 전체에 반복해서 기록합니다.

```text
1 MiB block A → OneZero128
1 MiB block B → walkingInvOnes~256
1 MiB block C → JustFive32
```

`CopyThread`는 원본 데이터를 대상 block에 복사하고 원본의 `pattern` 정보도 대상 block에 전달합니다. 따라서 복사가 끝난 대상 block은 원본과 같은 기대 pattern을 갖습니다.

## Pattern별 기대 checksum

각 `Pattern` 객체는 초기화할 때 4 KiB 데이터의 modified-Adler checksum을 미리 계산합니다 (`src/pattern.cc:246`). Pattern이 일정한 주기로 반복되므로 1 MiB block 안의 모든 4 KiB 검사 구간에 같은 기대 checksum을 적용할 수 있습니다.

Tag mode에서는 각 cache line의 첫 8 B에 주소 tag를 기록하므로, 현재 주소를 함께 반영하는 checksum 함수를 사용합니다.

## `--tag_mode`

Tag mode는 각 64 B cache line의 첫 8 B에 해당 virtual address로 만든 tag를 저장합니다 (`src/worker.cc:490`). 나머지 위치에는 선택한 pattern을 기록합니다.

목적:

- 다른 주소의 cache line이 잘못 전달된 경우
- 서로 다른 주소가 같은 위치를 가리키거나 주소 전달 경로가 잘못된 경우
- 데이터 pattern은 맞지만 다른 위치의 데이터가 들어온 경우

이러한 오류를 일반적인 data bit 오류와 구분하는 데 사용합니다.

Tag mode에서도 일반 cacheable memory를 사용합니다. 파일·네트워크 DMA 옵션과 함께 지정하면 호환성 검사에서 초기화가 실패합니다.

<sub><em>Address tag: cache line의 현재 virtual address에서 계산하여 해당 line의 첫 8 B에 저장하는 식별값입니다.</em></sub>

## Pattern이 만드는 데이터와 실제 LPDDR 신호의 관계

Pattern 코드는 CPU가 읽고 쓸 byte·word 배열을 정합니다. 특정 LPDDR DQ pin에서 0과 1이 어떻게 바뀌는지는 다음 hardware 동작을 모두 거친 뒤 결정됩니다.

- cache line 할당과 write-back
- NoC packet 구성
- DMC의 data swizzle과 주소 분산
- channel·rank·bank 선택
- LPDDR burst 구성
- DBI, ECC, encryption 등 SoC/device 기능

따라서 특정 DQ pin에 적용되는 실제 pattern을 계산하려면 SoC 제조사의 DMC 설정과 PHY 배선 정보를 함께 적용해야 합니다.
