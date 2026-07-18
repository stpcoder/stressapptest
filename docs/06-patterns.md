# 데이터 pattern 전체 분석

## Pattern을 쓰는 이유

고정된 random bytes만 사용하는 대신 bit transition과 반복 구조가 다른 pattern을 섞으면 다음과 같은 오류 형태를 관찰하기 쉽다.

- 특정 bit가 0 또는 1로 고정되는 stuck-at 성향
- 인접 bit coupling
- alternating data에서 증가하는 switching
- 주소/경로 misrouting으로 다른 block의 pattern이 나타나는 현상
- read/write 과정에서 일부 bit 또는 burst가 바뀌는 현상

다만 userspace virtual access이므로 pattern 이름만으로 특정 physical DQ, CA, bank 또는 row transition을 직접 지정하는 것은 아니다.

## Variant 생성

각 pattern family는 다음 조합을 만든다.

```text
원본 / bitwise inverted
× 32 / 64 / 128 / 256 logical width
```

15 family × 2 polarity × 4 width = 120 object다. weight 0 variant는 선택되지 않는다.

### Width의 정확한 의미

`Pattern::pattern(offset)`은 `offset >> busshift`로 32-bit pattern word 반복 수를 바꾼다.

| 이름 suffix | 같은 32-bit word 반복 |
|---|---:|
| 32 | 1회 |
| 64 | 2회 |
| 128 | 4회 |
| 256 | 8회 |

이는 LPDDR physical channel width나 burst width가 아니다. DMC interleave, cache-line assembly, endian, bus packing을 거치므로 suffix를 실제 DQ width로 해석하면 안 된다.

## 15개 family와 선택 확률

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

각 확률에는 원본과 inverted가 모두 포함된다. 예를 들어 `OneZero` family 전체가 37.5%이고, 그 안에서 원본/반전 및 width가 각 weight에 따라 선택된다.

## Width별 전체 분포

| logical width | 확률 |
|---|---:|
| 32 | 30.00% |
| 64 | 18.75% |
| 128 | 32.50% |
| 256 | 18.75% |

OneZero128의 weight가 크므로 전체 선택에서 alternating all-zero/all-one 계열의 128-bit 반복이 특히 자주 나타난다.

## Block에 pattern이 지정되는 시점

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

Tag mode는 cache bypass 기능이 아니다. 또한 disk/network DMA와 호환되지 않아 이 option들과 동시에 사용하면 초기화가 실패한다.

## Pattern이 보장하지 않는 것

사용자가 `walkingOnes`를 선택해 특정 LPDDR DQ bit를 직접 순회시키는 API는 없다. Pattern은 CPU가 보는 byte/word 데이터다. 실제 pin-level 자극은 다음 요소를 거친 결과다.

- cache-line allocation/writeback
- NoC packetization
- DMC data swizzle/interleave
- channel/rank/bank 선택
- LPDDR burst organization
- DBI, ECC, encryption 등 SoC/device 기능

따라서 pattern의 pin-level 의미가 필요하면 vendor DMC configuration과 PHY mapping을 결합해야 한다.
