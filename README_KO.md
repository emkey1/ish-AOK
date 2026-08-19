# iSH-AOK

> **번역 안내:** 이 문서는 [README.md](README.md)의 이전 개정판(2026-08-14)을 옮긴 것으로,
> 549 릴리스에서 추가된 네이티브 프로그램(SmallCLUE, bash, zsh)에 관한 내용이 빠져
> 있습니다. 최신 내용은 README.md를 참고하세요.
>
> 특히 라이선스: `git submodule update --init --recursive`는 `deps/bash`를 포함하므로
> 기본 빌드는 GPLv3 바이너리가 됩니다. 배포할 계획이라면 README.md의
> "Native bash and licensing" 절을 먼저 읽으십시오.

iSH-AOK는 [ish-app/ish](https://github.com/ish-app/ish)의 포크로, 이 트리에서의 일상적인 개발을 위한 자체 제품, 툴링, 플랫폼 변경 사항을 포함하고 있습니다.

Testflight: https://testflight.apple.com/join/X1flyiqE

이 포크는 단순한 리브랜딩이 아닙니다. 포크 전용 동작, 번들된 루트 파일시스템, 진단 작업, File Provider 통합, 그리고 네 가지 게스트 아키텍처 지원을 포함하고 있습니다. 업스트림 iSH를 원한다면 `ish-app/ish`를 사용하세요. 이 저장소에서 작업 중이라면, 이 README가 참고해야 할 문서입니다.

## 이 포크가 추가한 것

- 포크 전용 앱 아이덴티티:
  - 제품명 `iSH-AOK`
  - 번들 루트 `app.ish.iSH-AOK`
- **네 가지 게스트 아키텍처**, 모두 JIT 기반: `i386`, `amd64`(x86_64), `arm64`(aarch64), `riscv64`.
- 앱 빌드에 번들된 루트 파일시스템(Alpine 3.23.3과 Devuan 6, 각각 i386, x86_64, aarch64용), 그리고 riscv64를 포함한 추가 다운로드 이미지.
- iOS를 통해 게스트 파일을 노출하는 File Provider 지원.
- 선택적 가속기: 자주 쓰이는 libc 루틴의 네이티브 대체, 암호화 및 pixman 오프로드.
- 이 포크 전용의 추가 진단 및 운영 관련 변경 사항.

## 게스트 아키텍처

네 가지 게스트 모두 지원되며 gadget JIT를 통해 실행됩니다. 어느 것도 네이티브로
실행되지 않습니다. ARM 호스트에서 `arm64` 게스트 명령어 하나는 `riscv64`의 경우와
똑같이 gadget 디스패치 하나입니다. 호스트와 같은 ISA 계열이라는 점은 각 gadget의
본체를 저렴하게 만들 뿐, 공짜로 만들지는 않습니다.

| 게스트 | 상태 |
|---|---|
| `i386` | 최초의 게스트, JIT 전용 |
| `amd64` | 지원됨, JIT |
| `arm64` | 지원됨, JIT |
| `riscv64` | 지원됨, JIT |

게스트별 회귀 테스트 스위트는 네 아키텍처 모두에서 통과합니다. 인터프리터는
레거시이며 제거될 예정이므로, 새 작업은 JIT를 대상으로 해야 합니다.

관련 파일:

- [jit/gen.c](jit/gen.c) 모든 게스트의 명령어 변환
- [jit/jit.c](jit/jit.c) 블록 캐시와 디스패치
- [kernel/calls.c](kernel/calls.c) ABI별 시스템 콜 테이블
- [docs/amd64_port_plan.md](docs/amd64_port_plan.md)
- [docs/aarch64_guest_plan.md](docs/aarch64_guest_plan.md)

## 성능

엔진은 gadget 디스패치당 약 6.8 ns로 디스패치 바운드 상태이며, 따라서 비용은 게스트
명령어 수에 비례합니다. 측정 방법과 수치는
[docs/perf_benchmarks_2026_08.md](docs/perf_benchmarks_2026_08.md)에 있습니다.

명령어 퓨전과 리턴 캐시는 A/B 측정을 위해 게스트별로 런타임에 전환할 수 있습니다.

```sh
cat /proc/ish/arm64_jit_fuse          # 계열마다 "이름 on|off" 한 줄씩
echo retcache=0 > /proc/ish/arm64_jit_fuse
echo all=1 > /proc/ish/riscv64_jit_fuse
```

`i386`, `amd64`, `arm64`, `riscv64`용 노드가 존재합니다. 이 비트들은 변환 시점에
소비되므로 변경은 새로 컴파일되는 블록에만 영향을 줍니다. 측정할 때는 각 실행을
별도의 프로세스로 수행하세요. [tests/manual/jit_fuse_ab.sh](tests/manual/jit_fuse_ab.sh)는
교차 실행 A/B를 자동화하며 종료 시 마스크를 원래대로 복원합니다.

## 선택적 가속기

셋 다 **기본적으로 꺼져 있으며** 명시적으로 켜야 합니다.

| 기능 | CLI | 동작 |
|---|---|---|
| HLE | `ISH_HLE=1` | 자주 쓰이는 libc 루틴(`memcpy`, `strlen`, `memcmp` 등)을 네이티브 코드로 대체 |
| 암호화 | `ISH_CRYPTO_ACCEL=1` | AES-GCM 및 ChaCha20-Poly1305 오프로드 |
| Pixman | `ISH_PIX_ACCEL=1` | pixman 합성 오프로드 |

이 중 HLE의 영향이 가장 큽니다. 대체 대상 루틴이 대부분을 차지하는 루프에서, 게스트는
네이티브 대비 약 250배 느린 상태에서 약 1.4배 수준으로 개선됩니다. 게스트 명령어마다
디스패치를 하는 대신 네이티브 호출 한 번 안에서 작업이 이루어지기 때문입니다. 이는
순수한 빠른 경로입니다. 인식되지 않는 libc는 매칭되지 않고 일반 변환으로 넘어갑니다.
`ISH_HLE_STATS=1`은 함수별 호출 횟수를 출력합니다.

## 저장소 구조

- `app/`: iOS 앱, UI, 루트 선택, 진단, File Provider 통합.
- `emu/`: 게스트 CPU 상태, 메모리, TLB, FPU/벡터 지원.
- `kernel/`: 시스템 콜 변환, 프로세스 모델, exec, 시그널, 메모리 관리.
- `fs/`: 파일시스템 계층, fakefs, procfs, tmpfs, 마운트.
- `jit/`: gadget JIT와 게스트별 변환기.
- `tests/`: 엔드투엔드 테스트와 게스트 측 회귀 스위트.
- `tools/`: 개발자 도구 및 호스트 측 헬퍼.

## 클론

이 저장소는 서브모듈을 사용합니다.

```bash
git clone --recurse-submodules git@github.com:emkey1/ish-AOK.git
cd ish-AOK
```

서브모듈 없이 이미 클론했다면:

```bash
git submodule update --init --recursive
```

## 빌드 요구 사항

로컬 개발에는 보통 다음이 필요합니다.

- Xcode
- Python 3
- Meson
- Ninja
- Clang/LLVM 툴체인
- sqlite3
- libarchive

macOS에서의 일반적인 설정:

```bash
brew install meson ninja llvm libarchive
```

`sqlite3`는 대개 이미 설치되어 있습니다.

Apple Silicon에서는 빌드가 `llvm`, `libarchive`, `unicorn`을 `/usr/local`보다
`/opt/homebrew`에서 먼저 찾는다는 점에 유의하세요. 예전 Intel용 Homebrew가 남아
있어도 그쪽의 x86_64 사본은 사용되지 않습니다.

## iOS 앱 빌드

[iSH-AOK.xcodeproj](iSH-AOK.xcodeproj)를 Xcode에서 열고 `iSH` 스킴을 빌드하세요.

포크 전용 설정 중 중요한 것:

- 번들 ID는 [app/iSH.xcconfig](app/iSH.xcconfig)에서 결정됩니다.
- `ROOT_BUNDLE_IDENTIFIER`의 기본값은 `app.ish.iSH-AOK`입니다.
- 프로젝트는 포크 전용 디버그 구성인 `Debug-ApplePleaseFixFB19282108`을 사용합니다.

기기용 커맨드라인 빌드:

```bash
xcodebuild \
  -project iSH-AOK.xcodeproj \
  -scheme iSH \
  -configuration Debug-ApplePleaseFixFB19282108 \
  -destination 'generic/platform=iOS' \
  -allowProvisioningUpdates build
```

iOS 빌드 스크립트는 저장소 루트에서 루트 파일시스템 아카이브를 앱 번들로
복사합니다. 하나라도 없으면 해당 번들 루트는 동작하지 않습니다.

## 네이티브 CLI / 에뮬레이터 빌드

에뮬레이터 쪽 작업에는 Meson 빌드가 Xcode 전체 실행보다 훨씬 빠릅니다.

```bash
meson setup build --buildtype=debugoptimized
ninja -C build
```

반드시 `--buildtype=debugoptimized`를 사용하세요. Meson의 기본값은 `debug`(`-O0`)이며,
`-O0` 에뮬레이터는 단지 느린 것이 아니라 그 위에서 얻은 측정값 자체를 무의미하게
만듭니다. 그런 빌드에서는 게스트의 `uname -v`에 `" unoptimized"`가 표시됩니다.

게스트 실행:

```bash
./build/ish -f build/alpine /bin/login -f root
```

루트 파일시스템 tarball로 파일시스템 생성:

```bash
./build/tools/fakefsify alpine-minirootfs-*.tar.gz alpine
```

## 회귀 테스트

호스트 측 테스트:

```bash
meson test -C build
```

`float80`은 `long double`이 x87 80비트 형식이 아닌 호스트에서는 건너뜁니다. Apple
Silicon이 여기에 해당하며, 그런 호스트에는 비교할 기준값 자체가 없기 때문입니다.
x86_64 호스트에서는 전부 실행됩니다.

게스트 측 스위트가 주된 회귀 게이트입니다. [tests/manual/](tests/manual)에 있으며
게스트 안에서는 `/AOK/tests`에 읽기 전용으로 제공됩니다. 시그널, futex, 프로세스
라이프사이클, 파일시스템 계층, JIT, 아키텍처별 명령어 동작을 다루는 약 120개의
프로그램으로 구성되어 있습니다. 각 프로그램은 실패 시 0이 아닌 값으로 종료하며 `-v`를
지원합니다.

게스트 안에서:

```sh
sh /AOK/tests/setup-regressions.sh --install-deps --run   # 전체 빌드 후 실행
sh /AOK/tests/setup-regressions.sh --only fs_conformance,futex_core --run
```

테스트를 추가하려면 소스를 `tests/manual/`에 넣고
[fs/aok-tests.manifest](fs/aok-tests.manifest)에 등록해야 합니다. 이 매니페스트가
`/AOK/tests`로 게시하는 역할을 합니다. 또한 빌드 및 실행되도록
[tests/manual/setup-regressions.sh](tests/manual/setup-regressions.sh)에도 추가하세요.
매니페스트에서 빠진 테스트는 기기에서 아무 말 없이 사라집니다.

## 루트 파일시스템 다루기

앱에 번들된 것: Alpine 3.23.3과 Devuan 6(excalibur), 각각 `i386`, `x86_64`,
`aarch64`용. `riscv64`와 Arch를 포함한 추가 이미지는 앱 안에서 내려받을 수 있으며,
카탈로그는 [deps/rootfs-manifest](deps/rootfs-manifest)에 있습니다.

루트 선택 UI와 메타데이터 처리는 다음에 있습니다.

- [app/Roots.m](app/Roots.m)
- [app/RootsTableViewController.m](app/RootsTableViewController.m)

참고:

- 앱은 가져온 루트마다 게스트 ABI를 기록합니다.
- 설치된 모든 루트는 부팅된 게스트에서 `/AOK/roots/<이름>`에 읽기·쓰기로 노출되므로,
  다른 아키텍처의 userland로 chroot할 수 있습니다.
- 관리되는 루트에 대해 File Provider 도메인이 동기화됩니다.

## 로깅과 진단

로깅은 [app/iSH.xcconfig](app/iSH.xcconfig)의 `ISH_LOG`로 제어하며, CLI 빌드에서는
`meson configure -Dlog=...`을 사용합니다.

```xcconfig
ISH_LOG = verbose strace
```

자주 쓰는 채널: `strace`(시스템 콜 인자와 반환값, 가장 유용함), `verbose`,
`instr`(모든 명령어, 매우 느림).

로거 기본값은 iPhone과 시뮬레이터에서 `nslog`, macOS에서 `dprintf`입니다.

## File Provider

이 포크는 게스트 파일을 시스템 파일 API를 통해 노출하기 위한 iOS File Provider
확장을 포함합니다.

- [app/FileProvider/FileProviderExtension.m](app/FileProvider/FileProviderExtension.m)
- [app/FileProvider/FileProviderEnumerator.m](app/FileProvider/FileProviderEnumerator.m)
- [app/FileProvider/FileProviderItem.m](app/FileProvider/FileProviderItem.m)

이는 포크 전용 기능이며 여기서 유지보수하는 제품 영역의 일부입니다.

## 릴리스 자동화

[tools/release-aok.sh](tools/release-aok.sh)는 아카이브 및 익스포트 흐름을 감쌉니다.

```bash
./tools/release-aok.sh preflight
./tools/release-aok.sh archive
./tools/release-aok.sh export latest /tmp/iSH-AOK-export
./tools/release-aok.sh upload-fastlane      # TestFlight 전체 자동화
```

`upload-fastlane`은 기존 `fastlane upload_build` 레인을 사용하며
Ruby/Bundler/Fastlane 환경과 서명 및 인증 시크릿이 필요합니다.

릴리스 자체는 `CURRENT_PROJECT_VERSION`을 올리고,
`docs/release-notes-since-iSH-AOK_<N>.md`와 `docs/release-summary-iSH-AOK_<N>.md`를
추가한 뒤, 해당 커밋에 `builds/iSH-AOK_<N>` 태그를 붙여 만듭니다. 태그 이름은 그 자체로
동작에 관여합니다. `.github/workflows/build-release-ipa.yml`이 `builds/iSH-AOK_*`에서
트리거되므로, 다르게 이름 붙인 태그는 릴리스 빌드를 만들지 않습니다.

## 브랜치

- `working`이 기본 브랜치이자 활성 통합 브랜치입니다. 버그 수정, 기능 작업, 릴리스
  후보가 여기로 들어옵니다.
- `amd64`, `aarch64`, `riscv`는 원래 게스트별 초기 구현 브랜치였습니다. 그 작업은
  `working`에 병합되었고, `working`이 네 게스트를 모두 빌드합니다.

## 업스트림과의 관계

iSH-AOK는 업스트림 iSH를 기반으로 하지만 의도적으로 분기되어 있습니다.

즉:

- 업스트림 README의 지침은 이 포크에서 불완전하거나 틀릴 수 있습니다
- 브랜치 이름과 빌드 구성이 다릅니다
- 번들 루트와 운영 동작은 이 포크 고유의 것입니다
- 여기의 amd64, arm64, riscv64 게스트가 업스트림에도 있다고 가정하면 안 됩니다

`upstream` 리모트가 있는 클론에서 `gh` CLI를 쓸 때는 `--repo emkey1/ish-AOK`를
전달하세요. 그렇지 않으면 `gh`는 `ish-app/ish`로 해석되어 이 포크가 아니라 업스트림의
워크플로, 릴리스, 태그에 대해 답합니다.

## 감사의 말

ARM64 게스트 작업은 [OpenMinis/ish-arm64](https://github.com/OpenMinis/ish-arm64)에서
동기를 얻었고 일부는 그로부터 가져왔습니다. 이는 동일한 기능을 독자적으로 추가한
`ish-app/ish`의 GPLv3 포크입니다. 파일 단위 출처 표기는
[docs/CREDITS-aarch64.md](docs/CREDITS-aarch64.md)를 참고하세요.

## 라이선스

다음을 참고하세요.

- [LICENSE.md](LICENSE.md)
- [LICENSE.IOS](LICENSE.IOS)
