# BicMag

BicMag은 NTIS 국가 R&D 공고를 로컬에 캐시하고, 다운로드한 PDF 첨부파일의 내용을 로컬에서 검색하는 크로스 플랫폼 애플리케이션입니다.

## 디렉터리 구조

```text
.
├── bicmag/                  # PDF 모듈 헤더와 구현
│   ├── pdf.h
│   └── pdf.cpp              # PoDoFo를 감싸는 C++ 경계
├── tests/                   # GLib 테스트
│   └── test-pdf.c
├── meson.build
├── LICENSE
└── README.md
```

공개 함수와 파일 이름은 GLib 스타일의 소문자 스네이크 케이스와 `bicmag_` 접두사를 사용합니다. C++ 라이브러리 타입과 예외는 `bicmag/` 경계를 넘지 않으며, C API는 `GError`와 GLib 소유권 규칙을 사용합니다.

## 빌드

PoDoFo 1.1 이상과 GLib 개발 패키지가 필요합니다.

```sh
meson setup build
meson compile -C build
meson test -C build --print-errorlogs
```
