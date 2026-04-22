### esp-idf 公式
https://github.com/espressif/esp-idf/blob/master/examples/peripherals/usb/host/hid/README.md

https://github.com/espressif/esp-idf/tree/master/examples/peripherals/usb/device/tusb_hid

https://github.com/espressif/esp-idf?tab=readme-ov-file

https://github.com/espressif/arduino-esp32/blob/master/libraries/USB/src/USBHIDKeyboard.h

https://docs.espressif.com/projects/esp-idf/en/v6.0/esp32s3/api-reference/kconfig-reference.html

sdkconfigの読み込み
sdkconfig.*なファイルがどこにあるか検索
自動生成されるものとそうでないものを確認、うまく読み込まれない場合は自動生成のものを消す

 idf-monitor -p COM5 -b 115200

前提
esp32を使ってfido2キーを自作したいです
platform.io,esp-idf, tinyusbを利用予定です
usb hidの扱いを学んだうえでfidoの使用を理解しつつ実装していきたいです

BLEとHID を切り替え可能なデバイスにしたいです
FIDO2処理部分等、再利用可能な機能はできるだけ再利用したいです
物理スイッチを切り替えて再起動し、BLEとHIDのモードを切り替えます

まずはBLEとHIDの切り替え第一歩として起動時にGPIOにつながる物理スイッチを見て処理を2つに分岐したいです




--------------
一言でいうと、**HID と FIDO2 を「輸送」「フレーミング」「CTAP コマンド」「認証器ロジック」に分けて、下の層から 1 段ずつ作る**のが最も学習効率が高いです。ESP32-S3 の USB Device Stack は TinyUSB ベースで、HID や vendor-specific class を含む USB デバイスを実装できます。TinyUSB 側も `tud_` が device stack の API だと明示しているので、まずは **TinyUSB の device 側で HID レポートを扱える状態** を作るのが最初の足場になります。 ([Espressif Systems][1])

おすすめの全体像はこれです。

```text
[物理/USB配線]
    ↓
[USB Device / TinyUSB]
    ↓
[HIDレポート入出力]
    ↓
[CTAPHIDパケット分割・再構築]
    ↓
[CTAP2コマンド処理(CBOR)]
    ↓
[FIDO2認証器ロジック]
    ├─ getInfo
    ├─ makeCredential
    ├─ getAssertion
    ├─ PIN/UV
    └─ 永続化・カウンタ・鍵管理
```

CTAP 仕様では、USB 上では CTAP メッセージは HID を使って運ばれ、これを **CTAPHID** と呼びます。さらに、要求/応答はメッセージであり、その下で **packet = HID report** に分割され、複数クライアントを扱うために **32-bit の Channel ID** が使われます。つまり、HID ができただけでは FIDO2 にはならず、その上に **CTAPHID の分割再構築とチャネル管理** が必要です。 ([FIDO Alliance][2])

学習順としては、次の 6 段階がかなり良いです。

### 1. まずは「普通の HID」を完全に理解する

最初は FIDO を忘れて、ESP-IDF の HID 例のように、**PC に HID デバイスとして認識され、ボタン操作でレポートを送れる**状態まで持っていくのがよいです。Espressif の `tusb_hid` 例も、TinyUSB を使って HID キーボード/マウスを動かす構成です。ここで学ぶのは、**USB 接続、descriptor、HID report の送受信、TinyUSB のコールバックの流れ**です。 ([GitHub][3])

この段階の到達目標は、

* デバイスが列挙される
* 自分の descriptor を置き換えられる
* 送信タイミングを自分で制御できる
  の 3 つです。USB Concepts の説明どおり、TinyUSB では `tud_` 系 API が device stack 側です。ここを身体で覚えるのが先です。 ([TinyUSB][4])

### 2. 次に「FIDO 用 HID」に置き換える

ESP-IDF の USB Device Stack は HID だけでなく vendor-specific function も扱えますが、FIDO の USB バインディングは **HID ベース**です。なので次は、キーボード/マウス向けの HID ではなく、**FIDO/CTAPHID 用の HID インターフェース**として descriptor と report サイズを合わせる段階です。ここではまだ認証処理は作らず、**受け取った HID report をログに出すだけ**で十分です。 ([Espressif Systems][1])

この段階で理解するべきことは、

* 1 回の HID report が CTAPHID の 1 packet になる
* 大きな CTAP メッセージは複数 packet に分かれる
* HID レイヤと CTAP レイヤは別物
  の 3 つです。仕様でも「packet は CTAPHID では HID reports にマップされる」と説明されています。 ([FIDO Alliance][2])

### 3. CTAPHID の最小実装を作る

ここで初めて FIDO らしくなります。最小セットとしては、**CTAPHID_INIT** と **CTAPHID_CBOR** を実装するのがよいです。CTAPHID_INIT は新しい channel を払い出すために使われ、broadcast channel `0xFFFFFFFF` に対して 8-byte nonce を送ると、新しい channel ID とプロトコル情報、バージョン、capabilities を返します。CTAPHID_CBOR は CTAP command byte + CBOR payload を運ぶコマンドです。 ([Espressif Systems][1])

ここでの学習課題は、

* init packet / continuation packet の組み立て
* channel ごとの受信バッファ
* 完全なメッセージ完成後に上位へ渡す
* 応答時も packet に再分割する
  です。
  この時点では、上位はダミーでも構いません。たとえば `CTAPHID_INIT` には正しく返答し、`CTAPHID_CBOR` には「未実装」のエラーを返すだけでも大きな前進です。 ([FIDO Alliance][2])

### 4. 最初の CTAP2 コマンドは `authenticatorGetInfo`

CTAP2 の最初の実装としては **`authenticatorGetInfo (0x04)`** が最適です。仕様でも、これは入力を取らず、authenticator が対応する protocol version、extensions、AAGUID、capabilities などを返すための問い合わせとされています。ブラウザやプラットフォームはこの応答を見て、その後のコマンドをどう送るかを決めます。 ([FIDO Alliance][5])

学習上の利点は大きくて、

* CBOR エンコード/デコードを理解できる
* 「FIDO2 の本体ロジック」が初めて入る
* 鍵生成や署名がまだ不要
  だからです。
  つまり **最初に作る“本物の FIDO2 機能”は `getInfo`** がよいです。 ([FIDO Alliance][5])

### 5. 次に `makeCredential` / `getAssertion`

CTAP2 の本丸は `authenticatorMakeCredential` と `authenticatorGetAssertion` です。`getAssertion` は RP ID、allowList、user presence / user verification 条件などを受け取り、正しい authenticator data と signature を返す流れです。仕様でも WebAuthn の抽象操作と CTAP の対応が説明されています。 ([FIDO Alliance][5])

ただし、ここから急に難しくなります。理由は、

* COSE 鍵
* credential ID 設計
* authenticatorData の生成
* sign counter
* 永続化
* user presence / user verification
  が一気に必要になるからです。
  なので学習重視なら、まずは **non-resident / PINなし / UVなし / 単一 credential の最小実装** を目標にするのが現実的です。これは私の実装方針としての提案ですが、仕様上も `getInfo` を先に使って feature detection する前提になっているので、機能を小さく刻む考え方と相性が良いです。 ([FIDO Alliance][5])

### 6. BLE は transport 差し替えとして後で載せる

あなたの方針どおり、**FIDO2 本体は共通化し、USB HID と BLE は transport だけ差し替える**のがよいです。CTAP 自体は「roaming authenticator と platform 間の application layer protocol」で、USB / BLE など複数 transport への binding を定義しています。つまり設計上も、**上位の CTAP ロジックは transport 非依存に寄せる**のが自然です。 ([FIDO Alliance][5])

そのため、コード構成はこうするのがおすすめです。

```text
components/
  fido_core/
    ctap_dispatch.c       // CTAP command byteで分岐
    ctap_cbor.c           // CBOR入出力
    authenticator_info.c  // getInfo
    authenticator_make.c  // makeCredential
    authenticator_get.c   // getAssertion
    credential_store.c    // 永続化
    crypto_iface.c        // 署名・鍵生成の薄い抽象化

  transport_usb_hid/
    ctaphid_framing.c     // INIT/CONT/チャネル管理
    usb_hid_transport.c   // TinyUSBとの接続

  transport_ble/
    ble_fido_transport.c  // 後で追加
```

この分け方なら、USB HID で学んだ CTAP2 ロジックを BLE でも再利用しやすいです。これは仕様の transport binding の考え方にも沿っています。 ([FIDO Alliance][5])

---

## 実際の学習順ロードマップ

おすすめはこの順です。

**段階A**
GPIO ボタン・LED・再起動切替
→ もう着手済み。これは user presence や mode 切替の土台です。

**段階B**
TinyUSB で最小 HID
→ FIDO なし。descriptor と送受信だけ理解する。 ([GitHub][3])

**段階C**
FIDO 用 HID descriptor + report I/O
→ まだ認証なし。HID packet をそのまま受信/送信するだけ。 ([FIDO Alliance][2])

**段階D**
CTAPHID_INIT 実装
→ channel allocation が通る。ここで “FIDO over HID” の最初の核心を学べます。 ([Espressif Systems][1])

**段階E**
CTAPHID_CBOR + `authenticatorGetInfo`
→ 最初の CTAP2 実装。CBOR と capability 応答を理解する。 ([FIDO Alliance][5])

**段階F**
`makeCredential` の最小版
→ 単一アルゴリズム、単純な保存、UP のみ。 ([FIDO Alliance][5])

**段階G**
`getAssertion` の最小版
→ allowList と署名、sign counter。 ([FIDO Alliance][5])

**段階H**
PIN / UV / resident key / credMgmt
→ ここは後半。いきなり入れない方がよいです。 `getInfo` の options/versions を使って feature detection する設計になっています。 ([FIDO Alliance][5])

---

## 学習時に毎回確認するべき「今どの層か」

毎段階で、次の質問に答えられるようにすると理解が崩れにくいです。

1. **今触っているのは USB か、HID か、CTAPHID か、CTAP2 か？**
2. **入力は packet か、message か、CBOR command か？**
3. **この層は transport 依存か、共通化できるか？**

特に CTAP 仕様は、transaction / message / packet / channel を明確に分けているので、この区別を崩さないことが重要です。 ([FIDO Alliance][2])

---

## いま次にやるべき具体的な一歩

次に作るべきものは、**「FIDO本体」ではなく `transport_usb_hid` の最小殻**です。
具体的には、

* TinyUSB で HID device が列挙される
* 受信した report をバッファへ入れる
* report 1 本ぶんの生データをログに出す
* 送信用 API `usb_hid_send_packet(const uint8_t *buf, size_t len)` を用意する

ここまでです。
この段階では、まだ `getInfo` すら不要です。まず **「1 packet を確実に受けて、1 packet を返せる」** を完成させるのがよいです。CTAPHID は packet ベースで HID report に載るので、この順番が自然です。 ([FIDO Alliance][2])

次に、あなたの今の PlatformIO + ESP-IDF 構成に合わせて、**学習用の第1段階として `transport_usb_hid/` の最小ファイル構成と、FIDO 用に広げやすい Generic HID のコピペ用骨組み** を出します。

[1]: https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/usb_device.html "USB Device Stack - ESP32-S3 -  — ESP-IDF Programming Guide v6.0 documentation"
[2]: https://fidoalliance.org/specs/fido-v2.1-ps-20210615/fido-client-to-authenticator-protocol-v2.1-ps-errata-20220621.pdf "Client to Authenticator Protocol (CTAP)"
[3]: https://github.com/espressif/esp-idf/blob/master/examples/peripherals/usb/device/tusb_hid/README.md "esp-idf/examples/peripherals/usb/device/tusb_hid/README.md at master · espressif/esp-idf · GitHub"
[4]: https://docs.tinyusb.org/en/latest/reference/usb_concepts.html "USB Concepts - TinyUSB"
[5]: https://fidoalliance.org/specs/fido-v2.2-ps-20250714/fido-client-to-authenticator-protocol-v2.2-ps-20250714.html "Client to Authenticator Protocol (CTAP)"
