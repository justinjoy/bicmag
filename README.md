# BicMag

BicMag은 NTIS 국가 R&D 공고를 로컬에 캐시하고, 다운로드한 PDF 첨부파일의 내용을 로컬에서 검색하는 크로스 플랫폼 애플리케이션입니다.

## 디렉터리 구조

```text
.
├── bicmag/                  # PDF 모듈 헤더와 구현
│   ├── pdf.h
│   ├── pdf.cpp              # PoDoFo를 감싸는 C++ 경계
│   ├── notice.c/.h          # 공고 날짜 규칙
│   ├── cache.c/.h           # SQLite/FTS5 로컬 캐시
│   └── ntis.c/.h            # libsoup 기반 HTTP 세션과 NTIS 요청
├── tests/                   # GLib 테스트
│   └── test-pdf.c
├── meson.build
├── LICENSE
└── README.md
```

공개 함수와 파일 이름은 GLib 스타일의 소문자 스네이크 케이스와 `bicmag_` 접두사를 사용합니다. C++ 라이브러리 타입과 예외는 `bicmag/` 경계를 넘지 않으며, C API는 `GError`와 GLib 소유권 규칙을 사용합니다.

## 빌드

PoDoFo 1.1 이상, GLib 및 libsoup 3 개발 패키지가 필요합니다.

```sh
meson setup build
meson compile -C build
meson test -C build --print-errorlogs
```

## 첨부파일 본문 검색

PDF는 PoDoFo로, HWPX는 시스템의 `bsdtar`로 본문을 추출합니다. HWP 본문 검색에는
`hwp5html`이 필요합니다. BicMag은 `PATH` 외에도
`$XDG_DATA_HOME/bicmag/hwp-tools/bin/hwp5html`을 자동으로 확인합니다. 예를 들어
사용자 전용 가상환경은 다음처럼 준비할 수 있습니다.

```sh
python -m venv ~/.local/share/bicmag/hwp-tools
~/.local/share/bicmag/hwp-tools/bin/pip install pyhwp six
```

HWP 변환 경로를 직접 지정하려면 `BICMAG_HWP5HTML` 환경 변수를 사용합니다. 변환에
실패하거나 제한시간을 초과한 첨부파일은 파일명만 색인하고 나머지 동기화는 계속합니다.
검색은 FTS 결과에 유니코드 부분문자열 검색을 합쳐 한글 복합어의 일부도 찾습니다.

Enable the repository pre-commit hook once after cloning:

```sh
git config core.hooksPath .githooks
```

The hook formats staged C and C++ sources with Uncrustify 0.83.0 or newer before each commit.
