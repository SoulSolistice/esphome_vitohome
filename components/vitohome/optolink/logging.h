/*
Copyright (c) 2023 Bert Melis. All rights reserved.

This work is licensed under the terms of the MIT license.
For a copy, see <https://opensource.org/licenses/MIT> or the LICENSE file.

Modified as part of vitohome (vendored & de-branded) - see THIRD_PARTY.md.

The engine's internal logging. Off by default. When VITOHOME_DEBUG_OPTOLINK
is defined, route to the ESP-IDF logger, which is present under both the
esp-idf and arduino-on-ESP32 frameworks (via ESP_PLATFORM). The upstream
<iostream> PC branch is removed so pure ESP-IDF builds do not drag in
iostream/iomanip, and so the native host harness output stays clean.
*/

#pragma once

#if defined(VITOHOME_DEBUG_OPTOLINK) && defined(ESP_PLATFORM)
  #include "esp_log.h"
  #define optolink_log_i(...) ESP_LOGI("optolink", __VA_ARGS__)
  #define optolink_log_e(...) ESP_LOGE("optolink", __VA_ARGS__)
  #define optolink_log_w(...) ESP_LOGW("optolink", __VA_ARGS__)
#else
  #define optolink_log_i(...) do {} while (0)
  #define optolink_log_e(...) do {} while (0)
  #define optolink_log_w(...) do {} while (0)
#endif
