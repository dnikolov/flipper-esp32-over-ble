// meshcore_esp_hal.h
// RadioLib ESP-IDF hardware abstraction layer, vendored for the `meshcore_scan` capability
// (docs/PLAN.md's "MeshCore Scan Capability" design plan).
//
// Adapted, with only cosmetic changes (renamed from EspHal.h/.cpp, this attribution comment
// added), from RadioLib's own `src/hal/ESP-IDF/EspHal.h`/`EspHal.cpp`
// (https://github.com/jgromes/RadioLib, MIT license) -- that HAL was added to RadioLib's
// master branch after the `jgromes/radiolib` 7.7.1 release this project's
// heltec/main/idf_component.yml currently pins (confirmed by inspecting both the tagged
// release fetched into heltec/managed_components/ and a fresh clone of master during this
// session): 7.7.1's RadioLib.h has no `hal/ESP-IDF/` directory at all and does not
// auto-include any native ESP-IDF HAL (only Arduino's, gated on RADIOLIB_BUILD_ARDUINO).
// Vendoring this single small adapter file is the pattern RadioLib's own wiki documents for
// "Porting to non-Arduino Platforms" and is exactly what its own (newer, unreleased)
// `NonArduino/ESP-IDF` example now ships inline rather than expecting from the package --
// once a future `jgromes/radiolib` release includes this natively, this file and its .cpp
// should be deleted and meshcore_radio.cpp switched back to including
// `<hal/ESP-IDF/EspHal.h>` from the managed component directly.
#if !defined(_RADIOLIB_ESP_IDF_HAL_H)
#define _RADIOLIB_ESP_IDF_HAL_H

#if defined(ESP_PLATFORM) && !defined(ARDUINO)
#include "RadioLib.h"
#include "driver/spi_master.h"

#if !defined(RADIOLIB_TONE_ESP32_CHANNEL)
#define RADIOLIB_TONE_ESP32_CHANNEL (LEDC_CHANNEL_0)
#endif

/**
 * @brief ESP-IDF HAL for RadioLib using spi_master driver
 * This implementation properly handles SPI transactions and works with shared
 * SPI buses
 */
class EspHal : public RadioLibHal {
public:
  /**
   * @brief Constructor
   * @param sck SPI clock pin
   * @param miso SPI MISO pin
   * @param mosi SPI MOSI pin
   * @param host SPI host to use (SPI2_HOST or SPI3_HOST)
   * @param clockHz SPI clock frequency in Hz (default 2 MHz; raise per-module,
   *                e.g. SPI_MASTER_FREQ_8M for SX12xx if supported by the module)
   */
  EspHal(int8_t sck, int8_t miso, int8_t mosi,
         spi_host_device_t host = SPI2_HOST, uint32_t clockHz = 2000000);

  virtual ~EspHal();

  // implementations of pure virtual RadioLibHal methods
  void pinMode(uint32_t pin, uint32_t mode) override;
  void digitalWrite(uint32_t pin, uint32_t value) override;
  uint32_t digitalRead(uint32_t pin) override;
  void attachInterrupt(uint32_t interruptNum, void (*interruptCb)(void),
                       uint32_t mode) override;
  void detachInterrupt(uint32_t interruptNum) override;
  void delay(unsigned long ms) override;
  void delayMicroseconds(unsigned long us) override;
  unsigned long millis() override;
  unsigned long micros() override;
  long pulseIn(uint32_t pin, uint32_t state, unsigned long timeout) override;
  void spiBegin();
  void spiBeginTransaction();
  void spiTransfer(uint8_t *out, size_t len, uint8_t *in);
  void spiEndTransaction();
  void spiEnd();

  // implementations of virtual RadioLibHal methods
  void init() override;
  void term() override;
  void tone(uint32_t pin, unsigned int frequency,
            RadioLibTime_t duration = 0) override;
  void noTone(uint32_t pin) override;
  void yield() override;
  void pullUpDown(uint32_t pin, bool enable, bool up) override;

private:
  int8_t spiSCK;
  int8_t spiMISO;
  int8_t spiMOSI;
  spi_host_device_t spiHost;
  uint32_t spiClockHz;
  spi_device_handle_t spiDevice;
  bool busInitialized;
  bool deviceAdded;
  bool halInitialized;
  int32_t tonePrevFreq;
};
#endif // ESP_PLATFORM && !ARDUINO
#endif // _RADIOLIB_ESP_IDF_HAL_H
