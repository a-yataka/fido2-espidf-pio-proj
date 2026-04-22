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