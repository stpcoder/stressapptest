# GitBook 화면 색상 설정

이 문서의 목표 화면은 다음과 같습니다.

- 왼쪽 메뉴: 하늘색 배경
- 본문: 흰색 배경
- 현재 선택한 메뉴: 선으로 명확하게 표시
- 기본 표시 방식: 밝은 화면

GitBook의 색상은 저장소의 `.gitbook.yaml`이 아니라 GitBook 사이트 관리 화면에서 설정합니다. `.gitbook.yaml`은 문서 위치와 목차 파일을 지정하며 화면 색상은 지정하지 않습니다.

## 적용할 값

| 설정 항목 | 값 |
|---|---|
| Default mode | `Light` |
| Primary color | `#0EA5E9` |
| Tint color | 흰색 또는 기본 neutral |
| Sidebar background style | `Filled` |
| Sidebar list style | `Line` |

`#0EA5E9`는 본문 링크와 현재 메뉴 표시에도 사용되는 하늘색입니다. `Filled`를 선택하면 왼쪽 메뉴의 배경에 theme 색상이 적용됩니다. GitBook은 배경색에 따라 메뉴 글자색의 명암을 자동으로 조정합니다.

## GitBook 관리 화면에서 적용하는 순서

1. GitBook에서 `stressapptest-mobile-arm64-docs` 사이트를 엽니다.
2. `Customization`을 선택합니다.
3. `General` 또는 `Themes and colors` 영역을 엽니다.
4. 기본 표시 방식을 `Light`로 설정합니다.
5. `Primary color`에 `#0EA5E9`를 입력합니다.
6. 본문이 흰색으로 유지되도록 `Tint color`는 흰색 또는 기본 neutral을 선택합니다.
7. `Sidebar styles`에서 `Background style`을 `Filled`로 설정합니다.
8. `List style`을 `Line`으로 설정합니다.
9. 미리보기에서 왼쪽 메뉴는 하늘색, 본문은 흰색으로 표시되는지 확인합니다.
10. `Save`를 눌러 공개 사이트에 반영합니다.

## 저장소에서 CSS를 추가하지 않는 이유

현재 GitBook 사이트는 사용자 CSS, HTML, JavaScript 삽입을 지원하지 않습니다. 저장소에 CSS 파일이나 지원되지 않는 `.gitbook.yaml` 항목을 추가해도 공개 사이트의 메뉴 색상은 바뀌지 않습니다. 따라서 위 설정은 GitBook의 공식 `Customization` 기능으로 적용해야 합니다.

GitBook 공식 문서:

- [Icons, colors, and themes](https://gitbook.com/docs/publishing-documentation/customization/icons-colors-and-themes)
- [Site customization](https://gitbook.com/docs/publishing-documentation/customization)
