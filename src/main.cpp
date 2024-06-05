#include "BluetoothSerial.h"
#include "ELMduino.h"

#include <TFT_eSPI.h>
#include <SPI.h>
#include "honda.h"
#include "vfr.h"

#include "WiFi.h"
#include <Wire.h>
#include "Button2.h"
#include <esp_adc_cal.h>
#include "driver/rtc_io.h"

BluetoothSerial SerialBT;
#define ELM_PORT SerialBT
#define DEBUG_PORT Serial


ELM327 myELM327;

// OBD-II Adapter MAC Address
uint8_t address[6] = {0x00, 0x10, 0xcc, 0x4f, 0x36, 0x03};
const char *pin = "1234";

#define ADC_EN 14
#define ADC_PIN 34
#define BUTTON_1 35
#define BUTTON_2 0
#define FIRST_SCREEN_CS 13
#define SECOND_SCREEN_CS 2

TFT_eSPI tft = TFT_eSPI(135, 240); // Invoke custom library

Button2 btn1(BUTTON_1);
Button2 btn2(BUTTON_2);
bool btnCick = true;

char buff[512];
int vref = 1100;

uint16_t RangeColors[4][2] = {
    {50, TFT_BLUE},
    {80, TFT_GREEN},
    {100, TFT_YELLOW},
    {120, TFT_RED}};

uint16_t RangeColorsRPM[3][2] = {
    {4000, TFT_BLUE},
    {6500, TFT_YELLOW},
    {11000, TFT_RED}};

uint16_t RangeColorsSpeed[1][2] = {
    {300, TFT_GREEN}};

uint16_t RangeColorsFuel[2][2] = {
    {0, TFT_RED},
    {100, TFT_GREEN}};

uint16_t RangeColorsFuelTrim[2][2] = {
    {0, TFT_RED},
    {20, TFT_GREEN}};

void espDelay(int ms)
{
    esp_sleep_enable_timer_wakeup(ms * 1000);
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
    esp_light_sleep_start();
}

void showVoltage()
{
    static uint64_t timeStamp = 0;
    if (millis() - timeStamp > 1000) {
        timeStamp = millis();
        uint16_t v = analogRead(ADC_PIN);
        float battery_voltage = ((float)v / 4095.0) * 2.0 * 3.3 * (vref / 1000.0);
        String voltage = "Voltage :" + String(battery_voltage) + "V";
        Serial.println(voltage);
        tft.fillScreen(TFT_BLACK);
        tft.setTextDatum(MC_DATUM);
        tft.drawString(voltage,  tft.width() / 2, tft.height() / 2 );
    }
}

void button_init()
{
    btn1.setLongClickHandler([](Button2 & b) {
        btnCick = false;
        int r = digitalRead(TFT_BL);
        tft.fillScreen(TFT_BLACK);
        tft.setTextColor(TFT_GREEN, TFT_BLACK);
        tft.setTextDatum(MC_DATUM);
        tft.drawString("Press again to wake up",  tft.width() / 2, tft.height() / 2 );
        espDelay(6000);
        digitalWrite(TFT_BL, !r);

        tft.writecommand(TFT_DISPOFF);
        tft.writecommand(TFT_SLPIN);
        //After using light sleep, you need to disable timer wake, because here use external IO port to wake up
        esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
        // esp_sleep_enable_ext1_wakeup(GPIO_SEL_35, ESP_EXT1_WAKEUP_ALL_LOW);
        rtc_gpio_init(GPIO_NUM_14);
        rtc_gpio_set_direction(GPIO_NUM_14, RTC_GPIO_MODE_OUTPUT_ONLY);
        rtc_gpio_set_level(GPIO_NUM_14, 1);
        delay(500); 
        esp_sleep_enable_ext0_wakeup(GPIO_NUM_35, 0);
        delay(200);
        esp_deep_sleep_start();
    });
    btn1.setPressedHandler([](Button2 & b) {
        Serial.println("Detect Voltage..");
        btnCick = true;
    });

    btn2.setPressedHandler([](Button2 & b) {
        btnCick = false;
        Serial.println("btn press wifi scan");
    });
}

void button_loop()
{
    btn1.loop();
    btn2.loop();
}

// Interpolate color function
uint16_t interpolateColor(float value, float min, float max, uint16_t color1, uint16_t color2)
{
    float ratio = (value - min) / (max - min);
    uint8_t r = (1 - ratio) * ((color1 >> 11) & 0x1F) + ratio * ((color2 >> 11) & 0x1F);
    uint8_t g = (1 - ratio) * ((color1 >> 5) & 0x3F) + ratio * ((color2 >> 5) & 0x3F);
    uint8_t b = (1 - ratio) * (color1 & 0x1F) + ratio * (color2 & 0x1F);
    return (r << 11) | (g << 5) | b;
}

// Generic method to draw a bar graph
void drawBarGraph(TFT_eSPI &tft, float value, float minValue, float maxValue, int barX, int barY, int barWidth, int barHeight,
                  uint16_t colorRanges[][2], int colorRangeCount)
{

    tft.fillRect(barX, barY, barWidth, barHeight, TFT_DARKGREY);
    int filledWidth = (int)map(value, minValue, maxValue, 0, barWidth);

    uint16_t color = TFT_WHITE;
    for (int i = 0; i < colorRangeCount; i++)
    {
        if (value < colorRanges[i][0])
        {
            color = colorRanges[i][1];
            break;
        }
        else if (i < colorRangeCount - 1 && value < colorRanges[i + 1][0])
        {
            color = interpolateColor(value, colorRanges[i][0], colorRanges[i + 1][0], colorRanges[i][1], colorRanges[i + 1][1]);
            break;
        }
        else
        {
            color = colorRanges[i][1];
        }
    }

    tft.fillRect(barX, barY, filledWidth, barHeight, color);
    tft.drawRect(barX, barY, barWidth, barHeight, TFT_WHITE);
}

// Generic method to draw the value
void drawValue(TFT_eSPI &tft, float value, const char *label, int barX, int barY, int barWidth, int barHeight, int decimals = 0)
{
    tft.setTextDatum(TL_DATUM);

    // Convert value to integer and create string
    String valueString = "";
    if (decimals == 0)
    {
        int intValue = (int)value;
        char valueStr[10];
        sprintf(valueStr, "%d", intValue);
        valueString = valueStr;
    }
    else
    {
        char valueStr[10];
        sprintf(valueStr, "%.*f", decimals, value);
        valueString = valueStr;
    }

    // Set the text size for the value and calculate its width
    tft.setTextSize(2);
    int valueWidth = tft.textWidth(valueString);

    // clear the area where the value will be drawn
    tft.fillRect(barX + barWidth + 5, barY, tft.width() - barWidth - 5, barHeight, TFT_BLACK);

    // Draw the value with a larger font size
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString(valueString, barX + barWidth + 5, barY + (barHeight / 2) - 8);

    // Set the text size for the label and draw it after the value string
    tft.setTextSize(1);
    tft.drawString(label, barX + barWidth + 5 + valueWidth + 5, barY + (barHeight / 2) - 4);
}

// Method to select the screen to draw the graph
void selectScreen(int screen)
{
    if (screen == 0)
    {
        digitalWrite(FIRST_SCREEN_CS, HIGH);
        digitalWrite(SECOND_SCREEN_CS, LOW);
    }
    else
    {
        digitalWrite(FIRST_SCREEN_CS, LOW);
        digitalWrite(SECOND_SCREEN_CS, HIGH);
    }
}

// Usage for Global Bar Graph
void DrawBar(TFT_eSPI &tft, float value, int x_min, int x_max, String label,
             uint16_t colorRanges[][2], int screen = 0, int order = 0, int decimals = 0)
{

    int barX = 1;
    int barY = 1 + (order * 27);      // Adjust Y position based on order (0, 1, 2, 3, ...
    int barWidth = tft.width() - 100; // Adjust width to leave space for the value
    int barHeight = 20;

    selectScreen(screen); // Select the screen to draw the graph

    drawBarGraph(tft, value, x_min, x_max, barX, barY, barWidth, barHeight, colorRanges, 4);
    drawValue(tft, value, label.c_str(), barX, barY, barWidth, barHeight, decimals);
}

void setup()
{
    pinMode(ADC_EN, OUTPUT);
    digitalWrite(ADC_EN, HIGH);
    

    Serial.begin(115200);
    button_init();
    esp_adc_cal_characteristics_t adc_chars;
    esp_adc_cal_value_t val_type = esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_11, ADC_WIDTH_BIT_12, 1100, &adc_chars);    //Check type of calibration value used to characterize ADC
    if (val_type == ESP_ADC_CAL_VAL_EFUSE_VREF) {
        Serial.printf("eFuse Vref:%u mV", adc_chars.vref);
        vref = adc_chars.vref;
    } else if (val_type == ESP_ADC_CAL_VAL_EFUSE_TP) {
        Serial.printf("Two Point --> coeff_a:%umV coeff_b:%umV\n", adc_chars.coeff_a, adc_chars.coeff_b);
    } else {
        Serial.println("Default Vref: 1100mV");
    }

    pinMode(FIRST_SCREEN_CS, OUTPUT);
    digitalWrite(FIRST_SCREEN_CS, LOW);
    pinMode(SECOND_SCREEN_CS, OUTPUT);
    digitalWrite(SECOND_SCREEN_CS, LOW);

    tft.init();
    tft.setRotation(1);
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);

    tft.setSwapBytes(true);
    selectScreen(0);
    tft.pushImage(0, 0, 240, 135, honda);
    selectScreen(1);
    tft.pushImage(0, 0, 240, 135, vfr);
    delay(5000);

    tft.fillScreen(TFT_BLACK);
    selectScreen(0);
    tft.fillScreen(TFT_BLACK);

    tft.setTextSize(4);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    tft.drawString("ArduHUD", 120, 67);
    selectScreen(2);
    tft.drawString("Connecting", 120, 47);
    tft.drawString("to OBD-II", 120, 87);

    pinMode(35, INPUT_PULLUP);
    pinMode(0, INPUT_PULLUP);
    // SerialBT.setPin("1234");
    ELM_PORT.begin("ArduHUD", true);

    if (!ELM_PORT.connect(address))
    {
        tft.fillScreen(TFT_BLACK);
        tft.drawString("Failed to connect", 120, 47);
        tft.drawString("Trying again", 120, 87);
        while (!ELM_PORT.connect(address))
            ;
    }

    if (!myELM327.begin(ELM_PORT, false, 2000, '0'))
    {
        tft.fillScreen(TFT_BLACK);
        tft.drawString("Failed Second Phase", 120, 47);
        tft.drawString("Restarting", 120, 87);
        delay(5000);
        ESP.restart();
    }

    tft.fillScreen(TFT_BLACK);
    tft.setTextSize(2);
    tft.drawString("Connected", 120, 67);
}

float voltageBefore = 0;
int pwm = 255;

void loop()
{
    if (btnCick) {
        showVoltage();
    }
    button_loop();
    /* float voltage = myELM327.batteryVoltage();
    if (myELM327.nb_rx_state == ELM_SUCCESS && voltage > 0 && voltage != voltageBefore)
    {
        voltageBefore = voltage;
        digitalWrite(FIRST_SCREEN_CS, HIGH);
        digitalWrite(SECOND_SCREEN_CS, LOW);
        tft.fillScreen(TFT_BLACK);
        DrawBar(tft, random(20, 120), 20, 120, "COOLANT", RangeColors, 0, 0);
        DrawBar(tft, random(20, 120), 20, 120, "OIL TEMP", RangeColors, 0, 1);
        DrawBar(tft, random(10, 60), 10, 60, "AIR INTAKE", RangeColors, 0, 2);
        DrawBar(tft, random(0,100), 0, 100, "THROTTLE", RangeColors, 0, 3);
        DrawBar(tft, random(0, 11000), 0, 11000, "RPM", RangeColorsRPM, 1, 0);
        DrawBar(tft, random(0, 300), 0, 300, "SPEED", RangeColorsSpeed, 1, 1);
        DrawBar(tft, random(0, 15), 0, 15, "FUEL", RangeColorsFuel, 1, 2);
        DrawBar(tft, random(-20, 20), -20, 20, "STFT", RangeColors, 1, 3);
    } */
    float voltage = myELM327.batteryVoltage();
    while (myELM327.nb_rx_state == ELM_GETTING_MSG)
        ;

    float rpm = myELM327.rpm();
        while (myELM327.nb_rx_state == ELM_GETTING_MSG)
        ;
    float speed = myELM327.kph();
        while (myELM327.nb_rx_state == ELM_GETTING_MSG)
        ;
    float throttle = myELM327.throttle();
        while (myELM327.nb_rx_state == ELM_GETTING_MSG)
        ;
    float coolant = myELM327.engineCoolantTemp();
        while (myELM327.nb_rx_state == ELM_GETTING_MSG)
        ;
    float oil = myELM327.oilTemp();
        while (myELM327.nb_rx_state == ELM_GETTING_MSG)
        ;
    float air = myELM327.intakeAirTemp();
        while (myELM327.nb_rx_state == ELM_GETTING_MSG)
        ;
    float fuel = myELM327.fuelLevel();
        while (myELM327.nb_rx_state == ELM_GETTING_MSG)
        ;
    float stft = myELM327.shortTermFuelTrimBank_1();
        while (myELM327.nb_rx_state == ELM_GETTING_MSG)
        ;
    float ltft = myELM327.longTermFuelTrimBank_1();
        while (myELM327.nb_rx_state == ELM_GETTING_MSG)
        ;


    DrawBar(tft, coolant, 20, 120, "COOLANT", RangeColors, 0, 0);
    DrawBar(tft, oil, 20, 120, "OIL TEMP", RangeColors, 0, 1);
    DrawBar(tft, air, 10, 60, "AIR INTAKE", RangeColors, 0, 2);
    DrawBar(tft, throttle, 0, 100, "THROTTLE", RangeColors, 0, 3);
    DrawBar(tft, voltage, 9, 15, "BATTERY", RangeColors, 0, 4, 1);
    DrawBar(tft, rpm, 0, 11000, "RPM", RangeColorsRPM, 1, 0);
    DrawBar(tft, speed, 0, 300, "SPEED", RangeColorsSpeed, 1, 1);
    DrawBar(tft, fuel, 0, 100, "FUEL", RangeColorsFuel, 1, 2);
    DrawBar(tft, stft, -20, 20, "STFT", RangeColorsFuelTrim, 1, 3, 1);
    DrawBar(tft, ltft, -20, 20, "LTFT", RangeColorsFuelTrim, 1, 4, 1);
    delay(1000);
}
