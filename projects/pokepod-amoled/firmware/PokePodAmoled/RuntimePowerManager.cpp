#include "RuntimePowerManager.h"

#include <driver/gpio.h>
#include <driver/rtc_io.h>
#include <esp_attr.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp32-hal-cpu.h>

#include "BoardConfig.h"

namespace pokepod {
namespace {

RTC_DATA_ATTR uint32_t retainedDeepSleepWakeCount = 0;

}  // namespace

bool RuntimePowerManager::begin(Print &log) {
  snapshot_.cpuMhz = static_cast<uint16_t>(getCpuFrequencyMhz());
  snapshot_.lastWakeCause =
      static_cast<uint8_t>(esp_sleep_get_wakeup_cause());
  snapshot_.wokeFromDeepSleep = esp_reset_reason() == ESP_RST_DEEPSLEEP;
  if (snapshot_.wokeFromDeepSleep) ++retainedDeepSleepWakeCount;
  snapshot_.deepSleepWakeCount = retainedDeepSleepWakeCount;
#if CONFIG_PM_ENABLE
  snapshot_.automaticPmSupported = true;
#endif
#if CONFIG_BT_CTRL_MODEM_SLEEP
  snapshot_.bleModemSleepSupported = true;
#endif
  log.printf("{\"event\":\"power_manager\",\"ok\":true,\"cpu_mhz\":%u,\"automatic_pm\":%s,\"ble_modem_sleep\":%s,\"deep_wake\":%s,\"wake_count\":%lu,\"wake_cause\":%u}\n",
             snapshot_.cpuMhz,
             snapshot_.automaticPmSupported ? "true" : "false",
             snapshot_.bleModemSleepSupported ? "true" : "false",
             snapshot_.wokeFromDeepSleep ? "true" : "false",
             static_cast<unsigned long>(snapshot_.deepSleepWakeCount),
             snapshot_.lastWakeCause);
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
    const PowerInputs &verifiedInputs,
    const PowerDecision &verifiedDecision, Print &log) {
  const PowerDecision verified = decidePower(verifiedInputs);
  if (!verified.allowLightSleep || verified.mode != PowerMode::lightSleep ||
      !verifiedDecision.allowLightSleep ||
      verifiedDecision.lightSleepTimerUs == 0) {
    return false;
  }
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  (void)gpio_wakeup_disable(static_cast<gpio_num_t>(kBootButtonPin));
  (void)gpio_wakeup_disable(static_cast<gpio_num_t>(kTouchInterruptPin));
  esp_err_t error = esp_sleep_enable_timer_wakeup(
      verifiedDecision.lightSleepTimerUs);
  if (error == ESP_OK) {
    error = gpio_wakeup_enable(static_cast<gpio_num_t>(kBootButtonPin),
                               GPIO_INTR_LOW_LEVEL);
  }
  if (error == ESP_OK && verifiedInputs.automaticWakeEnabled) {
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

bool RuntimePowerManager::armDeepSleepWakeSources(bool touchWakeEnabled,
                                                   Print &log) {
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  uint64_t wakeMask = 1ULL << kBootButtonPin;
  pinMode(kBootButtonPin, INPUT_PULLUP);
  if (touchWakeEnabled) {
    pinMode(kTouchInterruptPin, INPUT_PULLUP);
    // A held-low touch IRQ would immediately reboot the device. Consume the
    // interrupt and only arm the optional source once the line is released.
    if (digitalRead(kTouchInterruptPin) == HIGH) {
      wakeMask |= 1ULL << kTouchInterruptPin;
    }
  }
  snapshot_.deepSleepTouchWakeArmed =
      (wakeMask & (1ULL << kTouchInterruptPin)) != 0;
  const esp_err_t error = esp_sleep_enable_ext1_wakeup_io(
      wakeMask, ESP_EXT1_WAKEUP_ANY_LOW);
  if (error != ESP_OK) {
    snapshot_.lastError = error;
    log.printf("{\"event\":\"deep_sleep_arm\",\"ok\":false,\"error\":%ld}\n",
               static_cast<long>(error));
    return false;
  }
  log.printf("{\"event\":\"deep_sleep_arm\",\"ok\":true,\"boot\":true,\"touch\":%s}\n",
             snapshot_.deepSleepTouchWakeArmed
                 ? "true" : "false");
  return true;
}

[[noreturn]] void RuntimePowerManager::startDeepSleep(Print &log) {
  log.flush();
  esp_deep_sleep_start();
  while (true) delay(1000);
}

}  // namespace pokepod
