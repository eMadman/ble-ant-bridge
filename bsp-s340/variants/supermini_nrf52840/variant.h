/*
 * variant.h — SuperMini nRF52840 (Nice!Nano pin-compatible), S340 build.
 *
 * Derived from the stock Adafruit BSP `pca10056` variant. Differences that
 * matter for this board:
 *   - USE_LFRC: the SuperMini / Nice!Nano has NO 32 kHz crystal — the LF clock
 *     must run from the internal RC, or the SoftDevice clock config #errors and
 *     BLE is unstable. (pca10056 uses USE_LFXO; that would be wrong here.)
 *   - ONE LED on P0.15, active-high (LED_STATE_ON = 1). Confirmed against the
 *     bootloader board: src/boards/supermini_nrf52840/board.h.
 *   - No on-board QSPI flash defines (the SuperMini has none).
 *
 * NOTE: the peripheral pin assignments (Serial1 / SPI / Wire / analog) below are
 * placeholders carried over from the generic map so the core compiles. The PoC
 * (USB-CDC Serial + BLE + status LED) does not use them. Re-map them to the
 * SuperMini silkscreen before relying on I2C/SPI/UART in later phases.
 */

#ifndef _VARIANT_SUPERMINI_NRF52840_
#define _VARIANT_SUPERMINI_NRF52840_

/** Master clock frequency */
#define VARIANT_MCK       (64000000ul)

// #define USE_LFXO     // Board uses 32kHz crystal for LF
#define USE_LFRC        // SuperMini / Nice!Nano: internal RC for LF (no crystal)

/*----------------------------------------------------------------------------
 *        Headers
 *----------------------------------------------------------------------------*/

#include "WVariant.h"

#ifdef __cplusplus
extern "C"
{
#endif // __cplusplus

// Number of pins defined in PinDescription array
#define PINS_COUNT           (48)
#define NUM_DIGITAL_PINS     (48)
#define NUM_ANALOG_INPUTS    (6)
#define NUM_ANALOG_OUTPUTS   (0)

// LEDs — SuperMini has a single LED on P0.15 (index 15 in the sequential map),
// active-high. The Bluefruit library references LED_CONN (connection) and
// LED_BLUE (heartbeat blinky); on this one-LED board they all alias to P0.15.
#define PIN_LED1             (15)        // P0.15
#define LED_BUILTIN          PIN_LED1
#define LED_CONN             PIN_LED1
#define LED_RED              PIN_LED1
#define LED_BLUE             PIN_LED1
#define LED_STATE_ON         1           // LED is lit when the pin is HIGH

/*
 * Analog pins (placeholders — AIN-capable GPIOs; re-map to silkscreen later)
 */
#define PIN_A0               (4)         // P0.04 / AIN2
#define PIN_A1               (5)         // P0.05 / AIN3
#define PIN_A2               (28)        // P0.28 / AIN4
#define PIN_A3               (29)        // P0.29 / AIN5
#define PIN_A4               (30)        // P0.30 / AIN6
#define PIN_A5               (31)        // P0.31 / AIN7
#define PIN_A6               (0xff)
#define PIN_A7               (0xff)

static const uint8_t A0  = PIN_A0 ;
static const uint8_t A1  = PIN_A1 ;
static const uint8_t A2  = PIN_A2 ;
static const uint8_t A3  = PIN_A3 ;
static const uint8_t A4  = PIN_A4 ;
static const uint8_t A5  = PIN_A5 ;
static const uint8_t A6  = PIN_A6 ;
static const uint8_t A7  = PIN_A7 ;
#define ADC_RESOLUTION    14

// Other pins
#define PIN_AREF           (2)
#define PIN_NFC1           (9)
#define PIN_NFC2           (10)

static const uint8_t AREF = PIN_AREF;

/*
 * Serial interfaces (placeholder — Nice!Nano D0/D1)
 */
#define PIN_SERIAL1_RX      (8)          // P0.08
#define PIN_SERIAL1_TX      (6)          // P0.06

/*
 * SPI Interfaces (placeholder)
 */
#define SPI_INTERFACES_COUNT 1

#define PIN_SPI_MISO         (46)        // P1.14
#define PIN_SPI_MOSI         (45)        // P1.13
#define PIN_SPI_SCK          (47)        // P1.15

static const uint8_t SS   = 44 ;
static const uint8_t MOSI = PIN_SPI_MOSI ;
static const uint8_t MISO = PIN_SPI_MISO ;
static const uint8_t SCK  = PIN_SPI_SCK ;

/*
 * Wire Interfaces (placeholder)
 */
#define WIRE_INTERFACES_COUNT 1

#define PIN_WIRE_SDA         (26)        // P0.26
#define PIN_WIRE_SCL         (27)        // P0.27

#ifdef __cplusplus
}
#endif

/*----------------------------------------------------------------------------
 *        Arduino objects - C++ only
 *----------------------------------------------------------------------------*/

#endif // _VARIANT_SUPERMINI_NRF52840_
