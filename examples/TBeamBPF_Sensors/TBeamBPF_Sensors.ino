/**
 * @file      TBeamBPF_Sensors.ino
 * @brief     All-sensor display for LilyGo T-Beam BPF (S3, 144 MHz)
 *
 * Reads and displays on SH1106 OLED (128x64):
 *   - BME680  : Temperature, Humidity, Pressure, Gas Resistance (Qwiic/I2C)
 *   - GPS     : Lat, Lon, Altitude, Satellites, Speed, Time
 *   - PMU     : Battery voltage/%, VBUS voltage, charge state (AXP2101)
 *
 * Button 1 (GPIO0): cycle pages manually
 *
 * Serial output at 115200 baud mirrors all sensor data.
 */

#define T_BEAM_S3_BPF
#include "LoRaBoards.h"
#include <Adafruit_BME680.h>
#include <TinyGPS++.h>

// ── BME680 ────────────────────────────────────────────────────────────────────
Adafruit_BME680 bme;
bool            bmeOnline      = false;
float           bmeTemp        = 0;
float           bmeHumid       = 0;
float           bmePressure    = 0;
float           bmeGas         = 0;   // kΩ
uint32_t        bmeReadTimer   = 0;

// ── GPS ───────────────────────────────────────────────────────────────────────
TinyGPSPlus     gps;

// ── Display pages ─────────────────────────────────────────────────────────────
enum Page { PAGE_BME = 0, PAGE_GPS, PAGE_PMU, PAGE_COUNT };
static int      currentPage   = PAGE_BME;
static uint32_t pageTimer     = 0;
static const uint32_t PAGE_INTERVAL_MS = 6000;

// ── Button ────────────────────────────────────────────────────────────────────
static bool     lastBtn       = HIGH;
static uint32_t btnDebounce   = 0;

// ─────────────────────────────────────────────────────────────────────────────
void initBME680()
{
    // Try both standard I2C addresses
    if (bme.begin(0x77, &Wire)) {
        bmeOnline = true;
    } else if (bme.begin(0x76, &Wire)) {
        bmeOnline = true;
    }

    if (bmeOnline) {
        bme.setTemperatureOversampling(BME680_OS_8X);
        bme.setHumidityOversampling(BME680_OS_2X);
        bme.setPressureOversampling(BME680_OS_4X);
        bme.setIIRFilterSize(BME680_FILTER_SIZE_3);
        bme.setGasHeater(320, 150);   // 320 °C for 150 ms
        Serial.println("[BME680] Online");
    } else {
        Serial.println("[BME680] NOT FOUND — check Qwiic cable & I2C address");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
void readBME680()
{
    if (!bmeOnline) return;
    if (millis() - bmeReadTimer < 2000) return;   // ~0.5 Hz (gas heater is slow)

    if (bme.performReading()) {
        bmeTemp     = bme.temperature * 9.0f / 5.0f + 32.0f; // °C → °F
        bmeHumid    = bme.humidity;
        bmePressure = bme.pressure / 100.0f * 0.02953f;      // Pa → inHg
        bmeGas      = bme.gas_resistance / 1000.0f;          // Ω → kΩ

        Serial.printf("[BME680] Temp=%.1f°F  Humid=%.1f%%  Press=%.2finHg  Gas=%.1fkΩ\n",
                      bmeTemp, bmeHumid, bmePressure, bmeGas);
    }
    bmeReadTimer = millis();
}

// ─────────────────────────────────────────────────────────────────────────────
void drawBMEPage()
{
    char buf[32];

    u8g2->setFont(u8g2_font_6x10_tf);
    u8g2->drawStr(0, 10, bmeOnline ? "=== BME680 ===" : "=== BME680 (offline) ===");

    if (bmeOnline) {
        snprintf(buf, sizeof(buf), "Temp : %.1f F", bmeTemp);
        u8g2->drawStr(0, 22, buf);

        snprintf(buf, sizeof(buf), "Humid: %.1f %%", bmeHumid);
        u8g2->drawStr(0, 32, buf);

        snprintf(buf, sizeof(buf), "Press: %.2f inHg", bmePressure);
        u8g2->drawStr(0, 42, buf);

        snprintf(buf, sizeof(buf), "Gas  : %.1f kOhm", bmeGas);
        u8g2->drawStr(0, 52, buf);

        // Heat-stability indicator (higher gas resistance = cleaner air)
        u8g2->drawStr(0, 62, bmeGas > 50 ? "Air: OK" : "Air: Poor");
    } else {
        u8g2->drawStr(0, 32, "Sensor not found");
        u8g2->drawStr(0, 44, "Check Qwiic cable");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
void drawGPSPage()
{
    char buf[32];

    u8g2->setFont(u8g2_font_6x10_tf);
    u8g2->drawStr(0, 10, "=== GPS ===");

    if (gps.location.isValid()) {
        snprintf(buf, sizeof(buf), "Lat: %.5f", gps.location.lat());
        u8g2->drawStr(0, 22, buf);
        snprintf(buf, sizeof(buf), "Lon: %.5f", gps.location.lng());
        u8g2->drawStr(0, 32, buf);
    } else {
        u8g2->drawStr(0, 22, "Lat: ---");
        u8g2->drawStr(0, 32, "Lon: ---");
    }

    if (gps.altitude.isValid())
        snprintf(buf, sizeof(buf), "Alt: %.0fm", gps.altitude.meters());
    else
        snprintf(buf, sizeof(buf), "Alt: ---");
    u8g2->drawStr(0, 42, buf);

    snprintf(buf, sizeof(buf), "Sat: %d  Spd: %.1fkm/h",
             (int)gps.satellites.value(),
             gps.speed.isValid() ? gps.speed.kmph() : 0.0);
    u8g2->drawStr(0, 52, buf);

    if (gps.time.isValid()) {
        snprintf(buf, sizeof(buf), "UTC %02d:%02d:%02d",
                 gps.time.hour(), gps.time.minute(), gps.time.second());
        u8g2->drawStr(0, 62, buf);
    } else {
        u8g2->drawStr(0, 62, "No fix yet...");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
void drawPMUPage()
{
    char buf[32];

    u8g2->setFont(u8g2_font_6x10_tf);
    u8g2->drawStr(0, 10, "=== Power (AXP2101) ===");

    if (PMU) {
        uint16_t battmV  = PMU->getBattVoltage();
        uint16_t vbusmV  = PMU->getVbusVoltage();
        uint16_t sysmV   = PMU->getSystemVoltage();
        bool     charging = PMU->isCharging();
        bool     vbusIn   = PMU->isVbusIn();

        snprintf(buf, sizeof(buf), "Batt: %d mV", battmV);
        u8g2->drawStr(0, 22, buf);

        if (PMU->isBatteryConnect()) {
            snprintf(buf, sizeof(buf), "     %d %%", (int)PMU->getBatteryPercent());
            u8g2->drawStr(0, 32, buf);
        } else {
            u8g2->drawStr(0, 32, "     No battery");
        }

        snprintf(buf, sizeof(buf), "VBUS: %d mV", vbusmV);
        u8g2->drawStr(0, 42, buf);

        snprintf(buf, sizeof(buf), "Sys : %d mV", sysmV);
        u8g2->drawStr(0, 52, buf);

        snprintf(buf, sizeof(buf), "%s  USB:%s",
                 charging ? "CHG:ON " : "CHG:OFF",
                 vbusIn   ? "YES" : "NO");
        u8g2->drawStr(0, 62, buf);

        Serial.printf("[PMU] Batt=%dmV  VBUS=%dmV  Sys=%dmV  CHG=%s  USB=%s\n",
                      battmV, vbusmV, sysmV,
                      charging ? "YES" : "NO",
                      vbusIn   ? "YES" : "NO");
    } else {
        u8g2->drawStr(0, 32, "PMU not found");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
void drawPageIndicator()
{
    // Small dots at bottom-right: filled = current page
    const int dotR = 2, spacing = 7;
    int startX = 128 - (PAGE_COUNT * spacing);
    for (int i = 0; i < PAGE_COUNT; i++) {
        int cx = startX + i * spacing;
        if (i == currentPage)
            u8g2->drawDisc(cx, 63, dotR);
        else
            u8g2->drawCircle(cx, 63, dotR);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
void updateDisplay()
{
    if (!u8g2) return;

    u8g2->clearBuffer();

    switch (currentPage) {
    case PAGE_BME: drawBMEPage(); break;
    case PAGE_GPS: drawGPSPage(); break;
    case PAGE_PMU: drawPMUPage(); break;
    }

    drawPageIndicator();
    u8g2->sendBuffer();
}

// ─────────────────────────────────────────────────────────────────────────────
void checkButton()
{
    bool btn = digitalRead(BUTTON_PIN);
    if (btn == LOW && lastBtn == HIGH && millis() - btnDebounce > 200) {
        currentPage  = (currentPage + 1) % PAGE_COUNT;
        pageTimer    = millis();
        btnDebounce  = millis();
    }
    lastBtn = btn;
}

// ─────────────────────────────────────────────────────────────────────────────
void setup()
{
    setupBoards();  // init Serial, I2C, PMU (AXP2101), GPS, OLED (U8G2/SH1106)

    initBME680();

    pinMode(BUTTON_PIN, INPUT_PULLUP);

    pageTimer = millis();

    Serial.println("=== T-Beam BPF Sensor Dashboard Ready ===");
    Serial.println("Pages: BME680 → GPS → Power  (auto 5s or Button1)");
}

// ─────────────────────────────────────────────────────────────────────────────
void loop()
{
    // Feed GPS NMEA data
    while (SerialGPS.available()) {
        gps.encode(SerialGPS.read());
    }

    readBME680();

    // Auto-advance page
    if (millis() - pageTimer > PAGE_INTERVAL_MS) {
        currentPage = (currentPage + 1) % PAGE_COUNT;
        pageTimer   = millis();
    }

    checkButton();

    updateDisplay();

    loopPMU(nullptr);   // handle PMU interrupts

    delay(50);
}
