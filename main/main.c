#include "freertos/FreeRTOS.h"  // FreeRTOS の基本定義
#include "freertos/task.h"      // vTaskDelay(), xTaskCreate() などタスク関連
#include "driver/gpio.h"        // GPIO の初期化・入出力操作
#include "esp_log.h"            // ESP_LOGI() などのログ出力
#include "esp_system.h"         // esp_restart() などシステム制御

// =========================
// GPIO番号の割り当て
// =========================

// モード切替用スイッチがつながるGPIO
// このピンは入力として使い、内部プルアップを有効にする。
// スイッチで GND に落とされたとき 0 を読む想定。
#define MODE_SW_GPIO     GPIO_NUM_1

// FIDO用ボタンのGPIO
// 現時点のこのコードではまだ使っていない。
// 将来、ユーザー確認ボタンなどに使う想定の予約ピン。
#define FIDO_BUTTON_GPIO GPIO_NUM_2

// HIDモード表示用LEDのGPIO
// 外付けLEDを想定しており、gpio_set_level(..., 1) で点灯する前提。
#define HID_LED_GPIO     GPIO_NUM_3

// BLEモード表示用LEDのGPIO
// 外付けLEDを想定しており、gpio_set_level(..., 1) で点灯する前提。
#define BLE_LED_GPIO     GPIO_NUM_4

// =========================
// モード種別
// =========================

// デバイスの動作モードを表す列挙型。
// 0 が USB HID、1 が BLE。
typedef enum {
    DEVICE_MODE_USB_HID = 0,
    DEVICE_MODE_BLE = 1,
} device_mode_t;

// ログ出力時に使うタグ文字列。
// ESP_LOGI(TAG, "...") の TAG 部分に入る。
static const char *TAG = "mode_select";

// =========================
// GPIO共通初期化関数
// =========================

// GPIOの初期化を汎用化した関数。
// 引数で、どのGPIOを、入力/出力のどちらにし、
// プルアップ/プルダウン/割り込みをどうするかを指定できる。
static void gpio_init_basic(
    gpio_num_t gpio_num,
    gpio_mode_t mode,
    gpio_pullup_t pullup,
    gpio_pulldown_t pulldown,
    gpio_int_type_t intr_type)
{
    // gpio_config_t は GPIO設定用の構造体。
    // ここで対象ピンやモードをまとめて指定する。
    gpio_config_t io_conf = {
        // どのGPIOを設定対象にするかをビットマスクで指定。
        // 例えば gpio_num = 3 なら (1ULL << 3) で bit3 を立てる。
        .pin_bit_mask = (1ULL << gpio_num),

        // 入力 / 出力 / 入出力 のモード指定
        .mode = mode,

        // 内部プルアップを有効/無効
        .pull_up_en = pullup,

        // 内部プルダウンを有効/無効
        .pull_down_en = pulldown,

        // 割り込み種別
        .intr_type = intr_type,
    };

    // 上で作った設定を ESP-IDF の GPIO ドライバへ適用する。
    gpio_config(&io_conf);
}

// =========================
// 出力用GPIO初期化
// =========================

// 指定したGPIOを「出力ピン」として初期化する。
// LEDなどをつなぐ用途を想定している。
// プルアップ/プルダウン/割り込みは使わない。
static void gpio_init_output(gpio_num_t gpio_num)
{
    gpio_init_basic(
        gpio_num,
        GPIO_MODE_OUTPUT,         // 出力モード
        GPIO_PULLUP_DISABLE,      // 内部プルアップ無効
        GPIO_PULLDOWN_DISABLE,    // 内部プルダウン無効
        GPIO_INTR_DISABLE         // 割り込み無効
    );
}

// =========================
// 入力 + プルアップGPIO初期化
// =========================

// 指定したGPIOを「入力ピン」として初期化し、内部プルアップを有効にする。
// スイッチ片側をGPIO、もう片側をGNDにつなぐ構成を想定。
// この場合、未押下では 1、押すと 0 を読む。
static void gpio_init_input_pullup(gpio_num_t gpio_num)
{
    gpio_init_basic(
        gpio_num,
        GPIO_MODE_INPUT,          // 入力モード
        GPIO_PULLUP_ENABLE,       // 内部プルアップ有効
        GPIO_PULLDOWN_DISABLE,    // 内部プルダウン無効
        GPIO_INTR_DISABLE         // 割り込み無効
    );
}

// =========================
// LED制御関数
// =========================

// 指定GPIOのLEDを点灯する。
// このコードは外付けLED前提で、1 を出力すると点灯する想定。
static void led_on(gpio_num_t gpio_num)
{
    gpio_set_level(gpio_num, 1);
}

// 指定GPIOのLEDを消灯する。
// このコードは外付けLED前提で、0 を出力すると消灯する想定。
static void led_off(gpio_num_t gpio_num)
{
    gpio_set_level(gpio_num, 0);
}

// =========================
// スイッチ入力値 → モード変換
// =========================

// スイッチから読んだ電圧レベルを、デバイスモードに変換する。
// このコードの前提は以下の通り:
//
// - GPIOは内部プルアップ有効
// - スイッチでGNDに落とす
//
// そのため:
// - sw_level == 0 なら BLE
// - sw_level == 1 なら USB HID
static device_mode_t mode_from_switch_level(int sw_level)
{
    // プルアップ + スイッチでGNDに落とす
    // 0 = BLE
    // 1 = HID
    return (sw_level == 0) ? DEVICE_MODE_BLE : DEVICE_MODE_USB_HID;
}

// =========================
// 起動時モード判定
// =========================

// 起動直後にスイッチ状態を読んで、どちらのモードで起動するかを決める関数。
// 読み取り直後はスイッチ状態が不安定な可能性があるため、少し待ってから2回読む。
// 2回とも同じ値ならその値を採用する。
// もし2回の値が異なったら、不安定とみなして既定値として USB HID を返す。
static device_mode_t detect_boot_mode(gpio_num_t sw_gpio)
{
    // 電源投入直後や再起動直後のピン状態安定待ち
    vTaskDelay(pdMS_TO_TICKS(20));

    // 1回目の読み取り
    int level1 = gpio_get_level(sw_gpio);

    // 少し待って2回目を読む
    vTaskDelay(pdMS_TO_TICKS(5));
    int level2 = gpio_get_level(sw_gpio);

    // 2回とも同じなら、その値をモードへ変換して返す
    if (level1 == level2) {
        return mode_from_switch_level(level1);
    }

    // 読み取りが不安定だった場合は安全側として USB HID を既定値にする
    return DEVICE_MODE_USB_HID;
}

// =========================
// モード表示LEDの切り替え
// =========================

// 現在のモードに応じて、BLE用LED / HID用LED のどちらを光らせるかを決める。
// BLEモードなら BLE_LED_GPIO を点灯し、HID_LED_GPIO を消灯。
// HIDモードならその逆にする。
static void set_mode_leds(device_mode_t mode)
{
    if (mode == DEVICE_MODE_BLE) {
        led_on(BLE_LED_GPIO);
        led_off(HID_LED_GPIO);
    } else {
        led_off(BLE_LED_GPIO);
        led_on(HID_LED_GPIO);
    }
}

// =========================
// スイッチ監視タスク
// =========================

// 実行中にスイッチ状態が変わったことを監視するタスク。
// 変化を見つけたら、すぐには確定せず、一定時間同じ状態が続いたら
// 「本当に切り替わった」と判断して esp_restart() を呼ぶ。
//
// これにより、スイッチのチャタリング（接点の細かい揺れ）で
// 誤反応しにくくしている。
static void switch_monitor_task(void *arg)
{
    // 引数として受け取った GPIO番号を sw_gpio として使う。
    // app_main() で MODE_SW_GPIO が渡される。
    gpio_num_t sw_gpio = (gpio_num_t)(uintptr_t)arg;

    // 現時点で安定しているとみなすスイッチ値
    int stable_level = gpio_get_level(sw_gpio);

    // 「変化候補」として観測中の値
    int candidate_level = stable_level;

    // candidate_level が観測され始めた時刻
    TickType_t candidate_since = 0;

    // 何msごとにスイッチを読むか
    const TickType_t poll_interval = pdMS_TO_TICKS(20);

    // 変化が何ms続いたら本物とみなすか
    const TickType_t debounce_time = pdMS_TO_TICKS(80);

    while (1) {
        // 現在のスイッチ値を読む
        int current = gpio_get_level(sw_gpio);

        // 現在値が stable_level と違うなら、
        // 「変化した可能性がある」状態
        if (current != stable_level) {

            // 今回の current が、前回観測していた candidate_level と違う場合:
            // 変化候補がさらに変わったので、観測し直しを始める
            if (current != candidate_level) {
                candidate_level = current;
                candidate_since = xTaskGetTickCount();
            } else {
                // current == candidate_level の場合:
                // 同じ変化候補が継続しているので、継続時間を確認する
                TickType_t now = xTaskGetTickCount();

                // 一定時間以上同じ変化候補が続いたら、変化確定とみなす
                if ((now - candidate_since) >= debounce_time) {
                    ESP_LOGI(TAG, "Switch changed: %d -> %d, restarting...", stable_level, candidate_level);

                    // ログが出る時間を少し与えてから再起動
                    vTaskDelay(pdMS_TO_TICKS(50));

                    // ソフトウェア再起動
                    // これにより app_main() からやり直しになる
                    esp_restart();
                }
            }
        } else {
            // current == stable_level の場合:
            // 状態は変わっていないので、候補状態をリセットする
            candidate_level = stable_level;
            candidate_since = 0;
        }

        // 次回の監視まで少し待つ
        vTaskDelay(poll_interval);
    }
}

// =========================
// USB HIDモードの本体処理
// =========================

// USB HIDモードで動作していることをログで示す関数。
// 現段階ではまだ本物の HID 処理は入っておらず、
// 1秒ごとにログを出し続けるダミー実装になっている。
static void run_usb_hid_mode(void)
{
    ESP_LOGI(TAG, "Boot mode: USB HID");

    while (1) {
        ESP_LOGI(TAG, "USB HID task running...");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// =========================
// BLEモードの本体処理
// =========================

// BLEモードで動作していることをログで示す関数。
// 現段階ではまだ本物の BLE 処理は入っておらず、
// 1秒ごとにログを出し続けるダミー実装になっている。
static void run_ble_mode(void)
{
    ESP_LOGI(TAG, "Boot mode: BLE");

    while (1) {
        ESP_LOGI(TAG, "BLE task running...");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}



// =========================
// アプリの開始点
// =========================

// ESP-IDFアプリのエントリポイント。
// ここで以下の順に処理する:
//
// 1. BLE/HID 用LEDを出力初期化
// 2. モード切替スイッチを入力+プルアップで初期化
// 3. いったん両LEDを消灯
// 4. 起動時スイッチ状態を読んでモード決定
// 5. 決定したモードに応じてLED表示切り替え
// 6. 実行中のスイッチ監視タスクを起動
// 7. BLEモードまたはHIDモードの本体処理に入る
void app_main(void)
{
    // BLE表示LEDを出力として使えるようにする
    gpio_init_output(BLE_LED_GPIO);

    // HID表示LEDを出力として使えるようにする
    gpio_init_output(HID_LED_GPIO);

    // モード切替スイッチを入力 + 内部プルアップで初期化
    gpio_init_input_pullup(MODE_SW_GPIO);

    // 起動直後はいったん両LEDを消しておく
    led_off(BLE_LED_GPIO);
    led_off(HID_LED_GPIO);

    // スイッチの状態を読んで起動モードを決める
    device_mode_t mode = detect_boot_mode(MODE_SW_GPIO);

    // 選ばれたモードに応じて、対応するLEDだけ点灯する
    set_mode_leds(mode);

    // 実行中にスイッチが切り替わったら再起動するための監視タスクを作成
    // タスク:別スレッドに近い？スイッチ監視を行う
    xTaskCreate(
        switch_monitor_task,                // 実行する関数
        "switch_monitor",                   // タスク名
        2048,                               // スタックサイズ
        (void *)(uintptr_t)MODE_SW_GPIO,    // タスク関数へ渡す引数
        5,                                  // 優先度
        NULL                                // タスクハンドルは不要
    );

    // 起動時に決めたモードに応じて本処理へ入る
    if (mode == DEVICE_MODE_BLE) {
        run_ble_mode();
    } else {
        run_usb_hid_mode();
    }
}