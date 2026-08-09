#include "RuntimePowerManager.h"

#include <driver/gpio.h>
#include <esp_sleep.h>
#include <esp_timer.h>
#include <esp32-hal-cpu.h>

#include "BoardConfig.h"

namespace pokepod {
namespace {

constexpr uint64_t kLightSleepSliceUs = 100000;

}  // namespace

bool RuntimePowerManager::begin(Print &log) {
  snapshot_.cpuMhz = static_cast<uint16_t>(getCpuFrequencyMhz());
#if CONFIG_PM_ENABLE
  snapshot_.automaticPmSupported = true;
#endif
#if CONFIG_BT_CTRL_MODEM_SLEEP
  snapshot_.bleModemSleepSupported = true;
#endif
  log.printf("{\"event\":\"power_manager\",\"ok\":true,\"cpu_mhz\":%u,\"automatic_pm\":%s,\"ble_modem_sleep\":%s}\n",
             snapshot_.cpuMhz,
             snapshot_.automaticPmSupported ? "true" : "false",
             snapshot_.bleModemSleepSupported ? "true" : "false");
  return true;
}

bool RuntimePowerManager::setCpuMhz(uint16_t mhz, Print &log) {
  if (snapshot_.cpuMhz == mhz) return true;
  if (!setCpuFrequencyMhz(mhz)) {
    snapshot_.lastError = -1;
    log.printf("{\"event\":\"power_cpu\",\"ok\":false,\"requested_mhz\":%u}\n",
               mhz);
    return false;
  }
  snapshot_.cpuMhz = static_cast<uint16_t>(getCpuFrequencyMhz());
  ++snapshot_.transitions;
  log.printf("{\"event\":\"power_cpu\",\"ok\":true,\"mhz\":%u}\n",
             snapshot_.cpuMhz);
  return snapshot_.cpuMhz == mhz;
}

PowerDecision RuntimePowerManager::apply(const PowerInputs &inputs,
                                         Print &log) {
  const PowerDecision next = decidePower(inputs);
  if (next.mode != snapshot_.mode) {
    snapshot_.mode = next.mode;
    ++snapshot_.transitions;
    log.printf("{\"event\":\"power_mode\",\"mode\":\"%s\"}\n",
               powerModeName(next.mode));
  }
  setCpuMhz(next.cpuMhz, log);
  decision_ = next;
  return decision_;
}

bool RuntimePowerManager::enterLightSleep(
    const PowerInputs &verifiedInputs, Print &log) {
  const PowerDecision verified = decidePower(verifiedInputs);
  if (!verified.allowLightSleep || verified.mode != PowerMode::lightSleep) {
    return false;
  }
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  esp_err_t error = esp_sleep_enable_timer_wakeup(kLightSleepSliceUs);
  if (error == ESP_OK) {
    error = gpio_wakeup_enable(static_cast<gpio_num_t>(kBootButtonPin),
                               GPIO_INTR_LOW_LEVEL);
  }
  if (error == ESP_OK) {
    error = gpio_wakeup_enable(static_cast<gpio_num_t>(kTouchInterruptPin),
                               GPIO_INTR_LOW_LEVEL);
  }
  if (error == ESP_OK) error = esp_sleep_enable_gpio_wakeup();
  if (error != ESP_OK) {
    snapshot_.lastError = error;
    log.printf("{\"event\":\"light_sleep\",\"ok\":false,\"error\":%ld}\n",
               static_cast<long>(error));
    return false;
  }
  const int64_t started = esp_timer_get_time();
  error = esp_light_sleep_start();
  const int64_t elapsed = esp_timer_get_time() - started;
  if (error != ESP_OK) {
    snapshot_.lastError = error;
    return false;
  }
  ++snapshot_.lightSleepCount;
  if (elapsed > 0) snapshot_.lightSleepUs += static_cast<uint64_t>(elapsed);
  snapshot_.lastWakeCause =
      static_cast<uint8_t>(esp_sleep_get_wakeup_cause());
  return true;
}

}  // namespace pokepod
