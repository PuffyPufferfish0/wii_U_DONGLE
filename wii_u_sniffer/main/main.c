#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "rom/ets_sys.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_random.h"

// --- LCD PIN CONFIGURATION ---
#define LCD_RS GPIO_NUM_6
#define LCD_EN GPIO_NUM_7
#define LCD_D4 GPIO_NUM_0
#define LCD_D5 GPIO_NUM_1
#define LCD_D6 GPIO_NUM_2
#define LCD_D7 GPIO_NUM_3

// --- TARGET MAC ADDRESS ---
uint8_t gamepad_mac[6] = {0x5A, 0x02, 0x83, 0xCD, 0xC2, 0x9F};

volatile bool target_locked = false;
char global_wps_pin[9]; // Stored globally so the AP can use it as the password

// Nintendo 5GHz Channel Spectrum
const int wiiu_channels[] = {36, 40, 44, 48, 149, 153, 157, 161};
volatile int current_channel_index = 0;
volatile int current_channel = 36;

// --- CUSTOM SYMBOLS ---
uint8_t spade[8]   = {0x04, 0x0E, 0x1F, 0x1F, 0x04, 0x0E, 0x00, 0x00};
uint8_t heart[8]   = {0x00, 0x0A, 0x1F, 0x1F, 0x0E, 0x04, 0x00, 0x00};
uint8_t diamond[8] = {0x04, 0x0E, 0x1F, 0x0E, 0x04, 0x00, 0x00, 0x00};
uint8_t club[8]    = {0x0E, 0x0E, 0x1F, 0x1F, 0x04, 0x0E, 0x00, 0x00};

// --- NATIVE LCD DRIVER ---
void lcd_pulse(void) {
    gpio_set_level(LCD_EN, 1);
    ets_delay_us(1);
    gpio_set_level(LCD_EN, 0);
    ets_delay_us(50);
}

void lcd_write_nibble(uint8_t nibble) {
    gpio_set_level(LCD_D4, (nibble >> 0) & 0x01);
    gpio_set_level(LCD_D5, (nibble >> 1) & 0x01);
    gpio_set_level(LCD_D6, (nibble >> 2) & 0x01);
    gpio_set_level(LCD_D7, (nibble >> 3) & 0x01);
    lcd_pulse();
}

void lcd_send(uint8_t value, uint8_t mode) {
    gpio_set_level(LCD_RS, mode);
    lcd_write_nibble(value >> 4);
    lcd_write_nibble(value & 0x0F);
    if (value == 0x01 || value == 0x02) ets_delay_us(2000); 
}

void lcd_print(const char* str) {
    while (*str) lcd_send(*str++, 1);
}

void lcd_create_char(uint8_t location, uint8_t* charmap) {
    location &= 0x7; 
    lcd_send(0x40 | (location << 3), 0);
    for (int i=0; i<8; i++) lcd_send(charmap[i], 1);
}

void lcd_init(void) {
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL<<LCD_RS) | (1ULL<<LCD_EN) | (1ULL<<LCD_D4) | (1ULL<<LCD_D5) | (1ULL<<LCD_D6) | (1ULL<<LCD_D7),
        .mode = GPIO_MODE_OUTPUT, .pull_up_en = 0, .pull_down_en = 0, .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf);
    
    vTaskDelay(50 / portTICK_PERIOD_MS);
    gpio_set_level(LCD_RS, 0);
    gpio_set_level(LCD_EN, 0);
    
    lcd_write_nibble(0x03); vTaskDelay(5 / portTICK_PERIOD_MS);
    lcd_write_nibble(0x03); ets_delay_us(150);
    lcd_write_nibble(0x03);
    lcd_write_nibble(0x02); 
    
    lcd_send(0x28, 0); 
    lcd_send(0x0C, 0); 
    lcd_send(0x01, 0); 
}

// --- WII U PIN LOGIC ---
void calculate_wps_pin(int sym0, int sym1, int sym2, int sym3, char* out_pin) {
    long pinBase = (sym0 * 64) + (sym1 * 16) + (sym2 * 4) + sym3;
    long temp = pinBase;
    int accum = 0;
    
    accum += 3 * ((temp / 1000000) % 10);
    accum += 1 * ((temp / 100000) % 10);
    accum += 3 * ((temp / 10000) % 10);
    accum += 1 * ((temp / 1000) % 10);
    accum += 3 * ((temp / 100) % 10);
    accum += 1 * ((temp / 10) % 10);
    accum += 3 * (temp % 10);
    
    int checksum = (10 - (accum % 10)) % 10;
    sprintf(out_pin, "%07ld%d", pinBase, checksum);
}

// --- RAW HARDWARE SNIFFER ---
// Triggers the instant the GamePad's radio waves touch our current channel
void sniffer_callback(void* buf, wifi_promiscuous_pkt_type_t type) {
    wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
    uint8_t *frame = pkt->payload;

    if (frame[0] == 0x40) { // Probe Request
        uint8_t mac[6];
        for(int i=0; i<6; i++) mac[i] = frame[10+i];

        if (memcmp(mac, gamepad_mac, 6) == 0) {
            if (!target_locked) {
                target_locked = true;
                printf("\n[RAW RADIO] GAMEPAD PROBE CAUGHT ON CHANNEL %d!\n", current_channel);
                printf("=> Dropping anchor. Channel hopping disabled.\n");
            }
        }
    }
}

// --- WI-FI EVENT HANDLER ---
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_id == WIFI_EVENT_AP_PROBEREQRECVED) {
        wifi_event_ap_probe_req_rx_t* event = (wifi_event_ap_probe_req_rx_t*) event_data;
        if (memcmp(event->mac, gamepad_mac, 6) == 0) {
            if (!target_locked) {
                target_locked = true;
                printf("\n[TARGET LOCKED] AP recognized GamePad. Projecting WPS Flags...\n");
            }
        }
    } 
    else if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
        printf("\n[!!!] DEVICE CONNECTED [!!!]\n");
        
        if (memcmp(event->mac, gamepad_mac, 6) == 0) {
            printf("=> ASSOCIATION SUCCESSFUL! HANDSHAKE INITIATED!\n");
            lcd_send(0x01, 0); 
            lcd_send(0x80, 0); 
            lcd_print("GamePad Found!");
            lcd_send(0xC0, 0); 
            lcd_print("Handshaking...");
        }
    } 
}

void start_smart_hunter_ap(void) {
    nvs_flash_init();
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL);

    wifi_config_t ap_config = {
        .ap = {
            .ssid = "WiiU_Dongle",
            .ssid_len = strlen("WiiU_Dongle"),
            .channel = current_channel, 
            .password = "", // Will be filled with global_wps_pin
            .max_connection = 1,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = { .required = false },
        },
    };
    // Inject the calculated PIN as the raw WPA2 password
    strcpy((char*)ap_config.ap.password, global_wps_pin);

    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &ap_config);

    // --- REFINED WPS SPOOFING ---
    // Perfect byte-for-byte replica of a Registrar WPS element
    uint8_t wps_ie[] = {
        0xDD, 0x1F, 0x00, 0x50, 0xF2, 0x04, // OUI Header & Length (31 bytes)
        0x10, 0x4A, 0x00, 0x01, 0x10,       // Version 1.0
        0x10, 0x44, 0x00, 0x01, 0x02,       // Setup State: Configured
        0x10, 0x41, 0x00, 0x01, 0x01,       // Selected Registrar: True
        0x10, 0x12, 0x00, 0x02, 0x00, 0x00, // Device Password ID: PIN
        0x10, 0x53, 0x00, 0x02, 0x01, 0x08  // Config Methods: Keypad & Display
    };
    
    esp_wifi_set_vendor_ie(true, WIFI_VND_IE_TYPE_BEACON, WIFI_VND_IE_ID_0, wps_ie);
    esp_wifi_set_vendor_ie(true, WIFI_VND_IE_TYPE_PROBE_RESP, WIFI_VND_IE_ID_0, wps_ie);

    esp_wifi_start();
    
    // Enable raw promiscuous mode ALONGSIDE the Access Point
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_promiscuous_rx_cb(&sniffer_callback);

    printf("\n--- Smart Hunter AP Started ---\n");
    printf("Waiting for Target MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
            gamepad_mac[0], gamepad_mac[1], gamepad_mac[2],
            gamepad_mac[3], gamepad_mac[4], gamepad_mac[5]);
}

// --- MAIN THREAD ---
void app_main(void) {
    printf("\n--- Wii U Dongle Booting (Smart Hunter Mode) ---\n");

    lcd_init();
    lcd_create_char(0, spade);
    lcd_create_char(1, heart);
    lcd_create_char(2, diamond);
    lcd_create_char(3, club);

    uint32_t r = esp_random();
    int sym0 = r % 4;
    int sym1 = (r >> 2) % 4;
    int sym2 = (r >> 4) % 4;
    int sym3 = (r >> 6) % 4;

    calculate_wps_pin(sym0, sym1, sym2, sym3, global_wps_pin);

    printf("WPS PIN Generated: %s\n", global_wps_pin);

    lcd_send(0x80, 0); 
    lcd_print("Pair GamePad:");
    lcd_send(0xC0, 0); 
    lcd_send(sym0, 1); lcd_print(" ");
    lcd_send(sym1, 1); lcd_print(" ");
    lcd_send(sym2, 1); lcd_print(" ");
    lcd_send(sym3, 1);

    start_smart_hunter_ap();

    while(1) {
        if (!target_locked) {
            // Stay on a channel for 3 seconds to give the GamePad time to sweep it
            vTaskDelay(3000 / portTICK_PERIOD_MS);
            
            if (!target_locked) {
                current_channel_index = (current_channel_index + 1) % 8;
                current_channel = wiiu_channels[current_channel_index];
                
                // Safely swap AP channels
                esp_wifi_set_promiscuous(false);
                esp_wifi_set_channel(current_channel, WIFI_SECOND_CHAN_NONE);
                esp_wifi_set_promiscuous(true);
                
                printf("No hit. Moving to Channel %d...\n", current_channel);
            }
        } else {
            // Target locked! Keep the thread alive but do nothing.
            vTaskDelay(1000 / portTICK_PERIOD_MS);
        }
    }
}