# chan_mobile에 Bluetooth LE Audio 전송 계층 추가

Asterisk의 `chan_mobile`은 휴대폰을 클래식 Bluetooth HFP로 연결하기 때문에
오디오가 CVSD 8 kHz, 헤드셋 프로파일이 협상해주는 경우에도 mSBC 16 kHz에서
막힙니다. 여기서는 두 번째 전송 계층을 붙입니다. 휴대폰의 LE Audio 유니캐스트
스트림 — LC3, 32 kHz, 10 ms 프레임, 80 옥텟 — 이 Asterisk에 `slin32`로 들어오고,
통화 제어는 HFP AT 명령에서 GTBS로 넘어갑니다.

*English version: [README.md](README.md)*

Asterisk 22.9.0 기준입니다. 클래식 경로는 손대지 않았고 여전히 기본값이며, LE
전송은 기기별 opt-in입니다.

이 작업의 나머지 절반은 [bluez-leaudio-server-fixes][bluez]입니다. acceptor
역할의 BlueZ에 대한 패치 네 개인데, 이 모듈을 돌리다가 드러난 것들입니다. 그중
적어도 하나 — Unicast Server가 `Releasing`에서 빠져나오지 못하는 문제 — 는
적용하지 않으면 **두 번째 통화부터 아예 안 됩니다.** 이 모듈을 빌드하려고
읽고 계신다면 그쪽도 함께 보셔야 합니다.

    클래식 CVSD   8 kHz    지금의 chan_mobile
    클래식 mSBC  16 kHz
    LE LC3       32 kHz    여기서 추가되는 것

## 설계를 결정한 경계

`bluetoothd`가 LE 링크와 ASE 상태 기계와 CIS를 소유합니다. Asterisk는 아니고,
BAP를 다시 구현하지 않는 한 소유할 수도 없습니다. 그래서 이 모듈은 스트림을
직접 열지 않습니다. 별도의 통화 제어 프로세스가 스트림을 협상한 뒤 ISO 소켓을
유닉스 소켓으로 넘겨주고, `chan_mobile_leaudio.c`가 그 인계의 미디어 쪽입니다.

이 코드에서 어색해 보이는 부분은 전부 그 경계에서 나옵니다. 상대 프로세스가
재시작하는 것, 소켓이 발밑에서 교체되는 것, 코덱이 아직 은닉에 쓸 이력이 없는
상태에서 프레임이 도착하는 것을 모듈이 버텨내야 합니다.

## 무엇이 들어 있나

    patches/0001-...patch    22.9.0 대비 diff: addons/chan_mobile.c 와
                             addons/Makefile
    addons/                  Asterisk 트리에 넣을 신규 파일
      chan_mobile_leaudio.c  LC3 미디어 헬퍼: ISO 소켓, 인코딩·디코딩,
      chan_mobile_leaudio.h  수명 주기, 인계 계약
      chan_mobile_lecall.c   통화 제어 클라이언트 (GTBS 수신·응답·종료)
      chan_mobile_lecall.h
      chan_mobile_msbc.c     클래식 mSBC 경로, 대체 수단으로 유지
      chan_mobile_msbc.h
    tests/                   미디어 수명 주기 단독 실행 시험
    build/build-chan-mobile.sh

## 빌드

Asterisk 22.9.0 트리 최상단에서, `$REPO`가 이 저장소의 체크아웃을 가리키게
두고 실행합니다.

    patch -p1 < "$REPO"/patches/0001-chan_mobile-add-an-LE-Audio-unicast-transport.patch
    cp "$REPO"/addons/*.[ch] addons/

패치는 `chan_mobile.c`뿐 아니라 `addons/Makefile`도 건드립니다. 그 hunk가 없으면
신규 소스 파일들이 아예 컴파일되지 않고, **LE Audio가 빠진 `chan_mobile.so`가
에러 없이 조용히 나옵니다.**

BlueZ 개발 헤더, libsbc, [liblc3][lc3]가 필요합니다. `build/`에 저희가 쓰는
레시피가 들어 있고, SBC와 LC3를 정적으로 링크해서 모듈이 `libbluetooth.so.3`과
libc 외에는 런타임 의존성을 갖지 않습니다.

[lc3]: https://github.com/google/liblc3

## 설정

두 전송 계층 모두 `chan_mobile.conf`에서 기기별로 지정하고 기본값이 `classic`
이므로, 기존 설정은 이전과 똑같이 동작합니다.

    [phone]
    address=AA:BB:CC:DD:EE:FF
    port=0
    audiotransport=le-canary      ; 또는 classic (기본값)
    callcontrol=le-gtbs           ; 또는 classic (기본값)
    widebandspeech=yes            ; 클래식 mSBC, 위와 무관
    leaudiosocket=/run/asterisk-leaudio/leaudio.sock
    lecallsocket=/run/asterisk-leaudio/lecall.sock

`le-canary`는 이 전송 계층이 아직 운영에서 돌리던 실험이었을 때의 이름이고 그대로
남았습니다. "LE Audio 전송"으로 읽으시면 됩니다.

**두 줄 중 하나만 켜는 것이 피해야 할 실수입니다.** LE 미디어에 클래식 통화
제어를 붙이거나 그 반대로 하면, 전화는 받아지는데 소리가 안 나오는 기기가
됩니다.

모듈을 다시 올릴 때는 어댑터의 voice 설정을 먼저 넣어야 합니다
(`hciconfig <dev> voice 0x0060`). 그러지 않으면 `module load chan_mobile`이
실패합니다.

## handoff 재연결에는 상한이 있습니다

미디어 handoff 소켓을 accept한 직후 닫는 피어는 handoff 루프를 전속력 회전으로
바꿉니다. connect → 로그 → `POLLHUP` → 반복입니다. 그 루프의 `sleep(1)`은
connect *실패*만 막았기 때문에, 성공했다가 즉시 죽는 연결은 아무 대가 없이 곧장
다시 돌았습니다. 실제로 한 번은 2.9GB 로그의 첫 200MB에 같은 NOTICE 210만 줄이
쌓였습니다.

`MBL_LE_HANDOFF_MIN_LIFETIME`을 못 넘긴 연결은 churn으로 보고 다음 시도를
지연시킵니다. 지연은 `MBL_LE_HANDOFF_BACKOFF_MAX`까지 2배씩 늘고, 살아남은
연결은 지연을 0으로 되돌립니다. 한 번의 불안정이 정상 링크를 붙잡지 않게 하려는
것입니다. 지연은 1초 단위로 자며 매번 언로드 플래그를 다시 보므로, 지연 전체
길이만큼 모듈 언로드를 붙잡지 않습니다.

접속 NOTICE는 `MBL_LE_HANDOFF_LOG_INTERVAL`마다 한 줄로 제한하고
`reconnects_since_last_notice=N`을 함께 싣습니다. 조용히 억제하면 정작 알아야 할
재연결 폭주가 가려지기 때문입니다.

## 통화 중 다이얼: 톤은 코어가 만듭니다

GTBS에는 DTMF opcode가 없습니다. Accept, Terminate, Hold, Retrieve, Originate,
Join이 전부라 LE 경로에서는 휴대폰에 키를 누르라고 시킬 방법이 없습니다.
클래식 HFP는 `AT+VTS`가 있어 필요가 없었습니다. 그래서 LE에서는 키패드를 눌러도
아무 일도 일어나지 않았고, ARS를 쓸 수 없었습니다.

Asterisk가 이미 해결책을 갖고 있고, 드라이버가 요청해야 합니다.

    if (!tech->send_digit_begin)          return 0;   /* 아무 일도 없음 */
    if (!tech->send_digit_begin(chan, d)) return 0;   /* 드라이버가 처리 */
    ast_playtones_start(chan, 0, dtmf_tones[...], 0); /* 0 아니면 코어가 생성 */

`send_digit_begin`을 비워두면 첫 분기로 빠져 톤이 만들어지지 않습니다.
`mbl_digit_begin`은 GTBS 통화에서 0이 아닌 값을, 클래식에서 0을 반환합니다.
클래식은 `AT+VTS` 그대로입니다. `ast_senddigit_end`는 `send_digit_end`가 0이
아닌 값을 반환할 때 톤을 멈추는데, GTBS 분기는 이미 그렇게 하고 있었습니다.

톤은 8kHz로 생성되고 코어가 변환하므로, 여기서 LE 샘플레이트를 알 필요가
없습니다. 통화가 8kHz로 남지도 않습니다. `playtones_alloc`이 쓰기 포맷을
저장하고 `playtones_release`가 되돌리므로, 좁은 대역인 구간은 키를 누르는
순간뿐이고 그때 실리는 것도 음성이 아니라 697~1633Hz 톤입니다. 톤을 직접
만드는 쪽이 떠오르는 대안인데, 바로 그 복구를 놓치는 길입니다.

## 호출 제어 소켓의 발신자 정보

`lecall` 패킷은 128바이트 고정이고, 발신자 정보는 통화 상태 메시지에 선택
페이로드로 실립니다.

    [0]      uri 길이    0..CM_LE_CALL_URI_MAX  (32)
    [1..n]   uri
    [1+n]    이름 길이   0..CM_LE_CALL_NAME_MAX (44)
    [2+n..]  이름

`CM_LE_CALL_VERSION`은 2이고 검사는 엄격합니다. 다른 버전은 관대하게 해석하지
않고 `-EPROTO`로 거절하므로, 소켓 양쪽을 함께 교체해야 합니다. 협상은 없고,
없는 편이 낫습니다. 두 프로세스는 한 쌍으로 배포됩니다.

파서는 `0-9 + * # -` 밖의 문자가 든 URI와 인쇄 불가능한 바이트가 든 이름을
거절합니다. 발신자 정보는 결국 SIP 헤더에 들어가므로, 상대가 걸러줬으리라고
믿지 않습니다.

두 길이 모두 0일 수 있습니다. 저희 배포는 번호만 보내고 이름은 비웁니다.
소프트폰이 같은 주소록을 동기화하므로 번호에서 이름을 스스로 찾기 때문입니다.
필드를 남겨둔 것은 이름을 원하는 배포가 프로토콜을 바꾸지 않아도 되게 하려는
것입니다. 망이나 발신 서버가 표시를 제한한 번호는 모듈이 `Anonymous`로
표시합니다.

모듈은 발신자 정보의 유무만 기록하고 값은 절대 기록하지 않습니다.

## 시험

미디어 수명 주기 시험은 liblc3만 있으면 됩니다. Asterisk 트리도, 블루투스
하드웨어도, 휴대폰도 필요 없습니다.

    cc -O2 -Wall -Iaddons -I/path/to/lc3/include \
        tests/test_media_lifecycle.c addons/chan_mobile_leaudio.c \
        /path/to/liblc3.a -lm -o test_media_lifecycle
    ./test_media_lifecycle

아홉 가지 경우를 다루고, 전부 실제로 저희를 물었던 것들입니다. 시작 시점의 빈
SDU를 죽은 스트림으로 오인하는 문제, `POLLHUP`/`POLLERR`/`POLLNVAL` 분류, 통화
사이에 파일 서술자가 재사용되는 문제, 그리고 디코더에 이력이 없는데 은닉이
돌아가는 문제입니다.

마지막 것은 따로 적어둘 만합니다. 처음 넣은 수정이 조용히 틀렸기 때문입니다.
패킷 손실 은닉은 직전에 디코딩된 프레임에서 외삽하므로, 첫 실제 프레임이 오기
전에는 외삽할 대상이 없어서 잡음을 냅니다. 그런데 "이 스트림이 진행된 적이
있는가"로 막는 것도 듣지 않습니다. **전송 성공도 진행으로 집계되고, TX가 항상
먼저 도착하기 때문입니다.** 판정은 RX만 봐야 합니다.

## 동일한 바이너리 재현

`build/build-chan-mobile.sh`는 저희가 운영하는 모듈의 ABI 체크섬과 의존성
집합을 단정하므로, 다른 환경에서는 끝까지 가지 않고 멈춥니다. 동작하는 모듈만
필요하시면 `AST_BUILDOPT_SUM`과 `needed=` 단정을 지우시면 됩니다.

같은 바이트를 원하신다면, 트리를 `--without-pjproject`와
`--with-jansson-bundled`로 구성해야 하고, zlib은 **staging하지 말아야** 하며,
체크아웃이 같은 절대 경로에 있어야 합니다. `-ffile-prefix-map`으로 넘기는 것이
Asterisk 트리뿐이어서 의존성 경로가 디버그 정보에 남습니다.

## 한계

이것은 동작하는 브리지이고 범용 드라이버는 아닙니다. 안드로이드 휴대폰 한 대와
어댑터 한 개를 상대로 개발했고, LE 전송은 그 휴대폰이 제시하는 32 kHz / 10 ms /
80 옥텟 구성을 전제합니다. 다른 구성은 협상하지 않습니다.

통화 시작 후 약 500 ms 구간이 끊길 수 있습니다. 그 구간에서 LE 수신과 RTP 송신
모두 깨끗하게 측정되므로 남은 용의자는 휴대폰이 보내는 쪽인데, 아직 규명하지
못했습니다.

BlueZ 쪽은 맨 위에 적었듯 선택 사항이 아닙니다. 전송 IO 참조를 아직 들고 있는
Unicast Server는 `Releasing`에서 빠져나오지 못하고, 약 3.5초 뒤 휴대폰의 LE
Audio 워치독이 그룹을 내려버립니다. 그 뒤로는 첫 통화 이후의 모든 통화가 조용히
실패합니다. 그 수정과 나머지 세 개가 [bluez-leaudio-server-fixes][bluez]에
있습니다.

[bluez]: https://github.com/YDaBang/bluez-leaudio-server-fixes

## 라이선스

Asterisk와 같은 GPL-2.0-only입니다. `LICENSE`를 보십시오.
