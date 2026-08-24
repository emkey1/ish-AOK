# iSH-AOK

> **번역 안내:** 이 문서는 [README.md](README.md)의 번역본입니다. 내용이 어긋나는
> 경우 영어판 README.md가 정본입니다.

iSH-AOK는 [ish-app/ish](https://github.com/ish-app/ish)의 포크로, 이 트리에서의 일상적인 개발을 위한 자체 제품, 툴링, 플랫폼 변경 사항을 포함하고 있습니다.

Testflight: https://testflight.apple.com/join/X1flyiqE

이 포크는 단순한 리브랜딩이 아닙니다. 포크 전용 동작, 번들된 루트 파일시스템, 진단 작업, File Provider 통합, 그리고 네 가지 게스트 아키텍처 지원을 포함하고 있습니다. 업스트림 iSH를 원한다면 `ish-app/ish`를 사용하세요. 이 저장소에서 작업 중이라면, 이 README가 참고해야 할 문서입니다.

## 이 포크가 추가한 것

- 포크 전용 앱 아이덴티티:
  - 제품명 `iSH-AOK`
  - 번들 루트 `app.ish.iSH-AOK`
- **네 가지 게스트 아키텍처**, 모두 JIT 기반: `i386`, `amd64`(x86_64), `arm64`(aarch64), `riscv64`.
- **네이티브 프로그램**: bash, zsh, 그리고 OpenSSH(`ssh`, `scp`, `sftp`, `ssh-keygen`, `ssh-copy-id`)와 Nextvi 편집기를 품고 있는 SmallCLUE 의 busybox 스타일 도구 모음이 호스트 코드로 앱에 컴파일되어 들어가며, 게스트의 `execve` 에서 `/AOK/native/<이름>` 을 통해 디스패치됩니다. 이들은 게스트 바이너리가 아니라 게스트 태스크 스레드 위에서 도는 호스트 함수이므로, 명령어 단위로 변환되지 않고 전속력으로 실행됩니다.
- `/AOK`, 읽기 전용 인앱 파일시스템(`/AOK/docs`, `/AOK/tools`, `/AOK/tests`, `/AOK/native`). `fs/aok-*.manifest` 와 `tools/gen-aokfs.py` 를 통해 빌드 시점에 `opt/AOK/` 에서 만들어 넣습니다.
- 앱 빌드에 번들된 루트 파일시스템(Alpine 3.23.3과 Devuan 6, `aarch64` 전용), 그리고 `i386`, `x86_64`, `riscv64` 용 다운로드 이미지.
- iOS를 통해 게스트 파일을 노출하는 File Provider 지원.
- **Apple 단축어(Shortcuts) 액션** (iOS 16+): 앱을 열지 않고도 네이티브 zsh로 게스트에서 명령을 실행하고 그 출력을 단축어로 돌려주는 "Run Command" 액션과, Siri 문구가 지원되는 "Open iSH-AOK" 대상들. `/AOK/docs/shortcuts.md` 참고.
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

게스트별 회귀 테스트 스위트는 기기에서 네 아키텍처 모두 통과합니다. 인터프리터는
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
| HLE | `ISH_HLE=1` | 자주 쓰이는 libc 루틴(`memcpy`, `strlen`, `memcmp` 등)을 네이티브 코드로 대체 — **arm64와 riscv64 게스트 전용** |
| 암호화 | `ISH_CRYPTO_ACCEL=1` | AES-GCM 및 ChaCha20-Poly1305 오프로드 |
| Pixman | `ISH_PIX_ACCEL=1` | pixman 합성 오프로드 |

HLE의 영향이 가장 크지만, arm64와 riscv64 게스트에 한정됩니다. `jit/jit.c`가 이 둘에만
게이트를 걸어 두었기 때문에 i386이나 amd64 게스트는 이 경로를 아예 타지 않으며,
그곳에서는 `ISH_HLE=1`을 줘도 조용히 아무 일도 일어나지 않습니다. 같은 빌드에서 끈
상태와 비교해 memcpy/memset/memcmp/strlen 루프로 측정한 값은 256 B에서 1.23배,
4 KB에서 3.16배, 64 KB에서 7.17배, 1 MB에서 6.68배입니다
([docs/performance-optimizations-2026-07.md](docs/performance-optimizations-2026-07.md)).
게스트 명령어마다 디스패치를 하는 대신 네이티브 호출 한 번 안에서 작업이 이루어지므로,
데이터 이동이 많은 코드에는 도움이 되고 프로그램 자신의 산술 연산이 지배적인 경우에는
중립적입니다. 이는 순수한 빠른 경로입니다. 인식되지 않는 libc는 매칭되지 않고 일반
변환으로 넘어갑니다. `ISH_HLE_STATS=1`은 함수별 호출 횟수를 출력합니다.

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

`--recursive` 는 `deps/bash` 를 포함하므로 기본 빌드가 GPLv3 빌드가 된다는 점에
유의하세요. 결과물을 배포할 생각이라면
[네이티브 bash와 라이선스](#네이티브-bash와-라이선스) 를 읽어 보십시오.

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

## 네이티브 프로그램

네이티브 프로그램은 앱 안에 컴파일되어 들어간 호스트 코드입니다. `/AOK/native`
아래의 경로를 `execve` 하면 게스트 이미지를 적재하는 대신 iSH-AOK 내부의 함수로
디스패치되며, 호출한 쪽은 그 차이를 알 수 없습니다. `/AOK/native` 에는
레지스트리(`kernel/native.c`)에 등록된 프로그램마다 항목이 하나씩 있고 —
`smallclue`, `motepad`, `hx`, `rust-probe`, `bash`, `zsh`, `zsh-multio` —
나머지는 모두 그중 하나를 가리키는 심볼릭 링크입니다. busybox 와 똑같이 링크
이름이 애플릿을 고릅니다:

| 프로그램 | 설명 |
|---|---|
| `/AOK/native/smallclue` | busybox 스타일 멀티콜 도구 모음, `argv[0]` 으로 애플릿 선택 |
| `ssh`, `scp`, `sftp`, `ssh-keygen`, `ssh-copy-id` | OpenSSH, SmallCLUE 의 애플릿 (OpenSSL 없이 빌드) |
| `vi` | Nextvi 편집기, SmallCLUE 의 애플릿 |
| `/AOK/native/motepad` | 모드가 없는 터미널 텍스트 편집기, Workspace 의 MotePad 애플릿에 대응 |
| `/AOK/native/hx` | [helix](https://helix-editor.com), 구문 강조를 지원하는 모달 편집기. MPL-2.0 이라 bash 처럼 빌드 스위치(`-Dnative_helix`)가 있으며, 문법 파일은 `/AOK/native/libs` 에 있습니다 |
| `/AOK/native/rust-probe` | `hx` 가 딛고 있는 Rust-온-shim 경로를 검증하는 프로브. 직접 쓸 일은 없습니다 |
| `/AOK/native/bash` | [네이티브 bash와 라이선스](#네이티브-bash와-라이선스) 참고 |
| `/AOK/native/zsh` | [네이티브 zsh](#네이티브-zsh) 참고 |

`/AOK/tools/native-links.sh` 는 애플릿을 `PATH` 에 올리는 심볼릭 링크 묶음을
만들고, `--shell bash|zsh|/path` 로 로그인 셸을 바꿉니다. `--remove` 는 둘 다
되돌립니다. 앱 안의 문서는 `/AOK/docs/native-programs.md`(무엇인가)와
`/AOK/docs/native-setup.md`(설정 방법)에 있고, 원본은
[opt/AOK/docs/](opt/AOK/docs) 아래에 있습니다.

어려운 부분은 속도가 아니라, 네이티브 프로그램이 자신이 실행되고 있는 iPhone 이
아니라 *게스트*에 대해 답해야 한다는 점입니다. 환경 변수, 신원, 파일시스템,
`/etc/hosts` 와 `/etc/resolv.conf`, terminfo, 로캘, rc 파일 위치가 모두 시스템
헤더보다 먼저 컴파일되는 shim(`kernel/native_libc.c`)을 통해 루트 파일시스템으로
라우팅됩니다. 기준이 되는 질문은 "이 함수는 순수한가?"가 아니라 "이 함수의 답이
호스트와 게스트에서 달라질 수 있는가?"입니다. `tools/check-native-libc.py` 가 그
게이트입니다. 빌드된 오브젝트를 훑어, 명시적 허용 목록에 없는데 네이티브
프로그램이 참조하는 호스트 libc 심볼을 전부 보고합니다. 빌드에 엮여 있지 않고
의도적으로 직접 실행하도록 되어 있습니다.

## 네이티브 bash와 라이선스

> 라이선스에 관한 내용은 영어판 README 의
> [Native bash and licensing](README.md#native-bash-and-licensing) 이 정본입니다.
> 아래는 이해를 돕기 위한 번역입니다.

bash 는 네이티브 프로그램으로 앱에 컴파일되어 들어갑니다. 이득은 fork 가 아니라
해석(interpretation)에 있습니다. 산술 루프는 에뮬레이트되는 셸보다 약 16배 빠르고,
서브셸과 명령 치환은 거의 같은 수준입니다. 네이티브 프로그램은 `fork` 를 할 수
없어 자기 자신을 다시 띄우기 때문입니다. 수치와 측정 방법은
[docs/bash_native_plan.md](docs/bash_native_plan.md) 에 있습니다. 동시에 이는
바이너리에 GPLv3 코드를 넣는 일이기도 합니다. bash 자체, 함께 들어가는 readline,
그리고 GNU termcap 입니다.

이 점은 App Store 배포에서 중요합니다. iSH-AOK 역시 GPLv3 이지만,
[LICENSE.IOS](LICENSE.IOS) 는 *이 프로젝트의* 저작권자들이 GPL 과 Apple 약관 사이의
충돌에 대해 권리를 행사하지 않겠다는 약속입니다. 이것이 bash 의 저작권을 보유한
FSF 를 구속할 수는 없으며, FSF 는 실제로 GPL 소프트웨어를 App Store 에서 두 번
내리게 한 적이 있습니다 — 2010년 [GNU
Go](https://www.theregister.com/2010/05/27/gnu_go_fsf_apple_itunes/) 와 2011년
[VLC](https://www.fsf.org/blogs/licensing/vlc-enforcement) 로, 스토어의 이용
규칙이 [GPL 6조](https://www.fsf.org/blogs/licensing/more-about-the-app-store-gpl-enforcement)
가 금지하는 "추가적인 제한"을 부과한다는 근거였습니다. FSF 는 이 해석이 v3 뿐
아니라 모든 GPL 버전에 적용된다고 밝히고 있습니다.

그래서 빌드 옵션입니다:

```bash
meson setup build .                          # auto: deps/bash 가 있으면 켜짐
meson setup build . -Dnative_bash=disabled   # 바이너리에 서드파티 GPL 없음
meson setup build . -Dnative_bash=enabled    # deps/bash 가 없으면 실패
```

configure 는 어느 쪽이 선택되었는지 `Licensing` 항목 아래에 출력합니다. 짐작하지
말고 확인하십시오:

```
Licensing
  native bash: no -- no third-party GPL in the binary
```

`disabled` 는 bash, readline, termcap 을 아카이브에서 완전히 뺍니다 — 오브젝트 0개,
`ar t` 로 확인했습니다. 그래도 사용자는 bash 를 쓸 수 있습니다. 게스트 루트
파일시스템의 에뮬레이트되는 `/bin/bash` 이며, 이는 Devuan 이나 Alpine 의 다른 모든
GPL 도구와 동일한 단순 병합(mere aggregation) 입장입니다.

**`kernel/native.c` 의 애플릿 테이블 항목을 지우는 것만으로는 부족합니다.**
`meson.build` 가 이 아카이브들을 `link_whole` 로 묶기 때문에, 아무것도 참조하지
않아도 오브젝트는 그대로 들어갑니다 — 레지스트리 항목을 지운 상태에서 bash
오브젝트 144개와 readline 오브젝트 35개가 남는 것을 측정했습니다. 빌드 옵션만이
이를 제거합니다.

바이너리의 나머지에는 서드파티 GPL 이 없습니다. SmallCLUE 는 MIT, OpenSSH 와
libarchive 는 BSD, liblzma 는 퍼블릭 도메인이며, `deps/linux` 는 이 타깃에
컴파일되지 않습니다.

## 네이티브 zsh

zsh 는 세 번째 네이티브 프로그램으로 컴파일되어 들어가며 `/AOK/native/zsh` 로
접근합니다. **기본값이 켜짐**이고, `-Dnative_zsh=disabled` 로 뺄 수 있습니다.
bash 와 달리 라이선스 문제는 없습니다. zsh 의 라이선스는 허용적이고, 컴파일되는 C
코드 중 GPL 인 것은 없습니다.

제대로 동작하는 셸입니다. 줄 편집기인 ZLE 가 동작합니다 — 프롬프트, 에코, 편집,
히스토리 키, 줄바꿈, 완전한 터미널 협상까지. 되지 않던 `fork` 도 됩니다. 네이티브
프로그램은 프로세스가 아니라 게스트 태스크 스레드 위의 C 함수이므로 `fork` 로 주소
공간을 복사할 수 없습니다. 그래서 zsh 는 자신의 상태를 스크립트로 직렬화한 뒤 자기
자신을 다시 띄웁니다. bash 에서 먼저 검증한 설계입니다
(`deps/zsh/Src/aok_fork.c`, `deps/bash/aok_fork.c`). 명령 치환, 파이프라인,
서브셸, 백그라운드 작업이 모두 이 경로를 지납니다:

```
% echo $(echo A); echo B | tr B C; (echo D); sleep 0.1 & wait; echo E
A
C
D
E
```

MULTIOS 리다이렉션은 동반 네이티브 프로그램인 `zsh-multio` 를 씁니다. 디스크립터를
셸이 아닌 무언가가 붙들고 있어야 하기 때문입니다.

`/AOK/tools/native-links.sh --shell zsh` 로 로그인 셸로 지정할 수 있습니다.

119개의 차등 테스트가 게스트의 `/AOK/tests/native_zsh_fork_state.sh` 에 들어
있습니다. 기대값은 그럴듯해 보이는 것이 아니라 실제 zsh 가 출력하는 것에서
가져왔으며, 그중 116개가 통과합니다. 실패하는 둘은 **프로세스 치환** — `<(...)` 과
`>(...)` — 이고, 이는 셸이 아니라 루트 파일시스템의 속성입니다. `/dev/fd` 가
필요한데 Alpine 이미지에는 없어서 그곳에서는 에뮬레이트되는 `/bin/bash` 에서도
똑같이 실패하고, `/dev/fd` 가 `/proc/self/fd` 심볼릭 링크인 Devuan 에서는 두 셸
모두 동작합니다. *셸 자체의* 알려진 결함 두 가지는
[docs/release-notes-since-iSH-AOK_549.md](docs/release-notes-since-iSH-AOK_549.md)
의 *Known gaps* 에 적혀 있습니다. 패턴이 처음 쓰일 때 컴파일되어 파스 트리에
캐시되는데 그때 어떤 옵션이 켜져 있었는지는 어디에도 기록되지 않아, 다시 띄워진
자식이 부모와 다른 옵션으로 컴파일할 수 있다는 것, 그리고 multio 아래의
`pipestatus` 가 zsh 의 `0 0` 대신 `1 0` 을 보고한다는 것입니다.

`deps/zsh` 트리는 [emkey1/zsh](https://github.com/emkey1/zsh) 의 `ish-aok`
브랜치를 서브모듈로 둔 것입니다. zsh 의 *생성된* 소스 — `config.h`,
`Src/signames.c`, 모듈별 `.mdh`/`.epro`/`.pro` — 를 업스트림의 `.gitignore` 를
거슬러 커밋해 두었습니다. 이 빌드는 zsh 를 meson 으로 컴파일하고 zsh 자신의 `make`
는 전혀 돌리지 않기 때문입니다. 그래서 체크아웃하면 configure 단계 없이 빌드됩니다:

```bash
git submodule update --init deps/zsh
```

termcap 전용으로, 모든 모듈을 정적 링크하도록 구성되어 있습니다. 둘 다 강제입니다.
iOS SDK 는 curses `.tbd` 스텁은 주면서 `curses.h`/`term.h` 는 주지 않고, 네이티브
프로그램은 `dlopen` 을 할 수 없습니다 — `--disable-dynamic` 만으로는 `zsh/regex`
가 조용히 `link=no` 로 매핑되어 `[[ =~ ]]` 가 런타임에 실패합니다.

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

예외인 스위트가 셋 있습니다. `native_zsh_fork_state.sh`(119개), `native_bash_fork_state.sh`(20개),
`native_stdio_redirect.sh` 는 C 가 아니라 셸 스크립트여서 `setup-regressions.sh` 가
빌드하지도 나열하지도 않습니다. 이들은 매니페스트를 통해 배포되어 `/AOK/tests` 에서
직접 실행되며, 각각 대응하는 네이티브 프로그램이 있어야 합니다.

## 루트 파일시스템 다루기

앱에 번들된 것: Alpine 3.23.3과 Devuan 6(excalibur), `aarch64` 전용. Xcode 의
"Download Root" 단계가 이 두 아카이브를 설치하고 Resources 에서 i386 과 x86_64
아카이브를 지우므로, 무언가를 내려받기 전까지는 이 둘만 존재합니다. 같은 두 배포판의
`i386`, `x86_64`, `riscv64` 판과 Arch 는 앱 안에서 내려받을 수 있으며, 카탈로그는
[deps/rootfs-manifest](deps/rootfs-manifest)에 있습니다.

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
