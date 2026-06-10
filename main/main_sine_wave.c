#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "driver/gpio.h"
#include "esp_timer.h"
#include <rom/ets_sys.h>

#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/spi_common.h"
#include "driver/sdspi_host.h"

// ----------------- WIFI ------------------- 
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_http_server.h"
#include "esp_netif_sntp.h"   

#include <dirent.h>
#include <sys/stat.h>
#include <time.h>


// ---------------- PINS ----------------
#define CONVST_PIN 0
#define ADC_DATA_PIN1 1
#define ADC_DATA_CLK1 21
#define ADC_DATA_PIN2 2
#define ADC_DATA_CLK2 22

#define FREQ 256 // Sample Frequency

#define SD_MISO_PIN 18
#define SD_MOSI_PIN 20
#define SD_CLK_PIN 19
#define SD_CS_PIN 23

// ---------------- GPIO REGISTERS ----------------
#define GPIO_OUT_REG       *((volatile uint32_t *) 0x60091004)
#define GPIO_IN_REG        *((volatile uint32_t *) 0x6009103C)

// ---------------- DATA ----------------
// Holder for data
typedef struct {
    uint64_t timestamp;
    float FL, FR, BL, BR;
} DataPoint;

static DataPoint dataBuffer;

static QueueHandle_t dataQueue;

bool sdAvailable = false;

// ---------------- SHIFT IN (REPLACEMENT) ----------------
//Shift in data from pin, Serial Data Reader
uint8_t shiftInCustom(uint32_t dataPin, uint32_t clkPin) {
    uint8_t value = 0;

    for (int i = 0; i < 8; i++) {
        GPIO_OUT_REG |= BIT(clkPin);   // clock HIGH
        ets_delay_us(1);

        value <<= 1;
        if (GPIO_IN_REG & BIT(dataPin)) {
            value |= 1;
        }

        GPIO_OUT_REG &= ~BIT(clkPin);  // clock LOW
        ets_delay_us(1);
    }

    return value;
}

// ---------------- ADC READ (SINE TEST MODE) ----------------
// CHECK MODE: instead of reading the real ADCs, synthesize a sine wave so

#define SINE_HZ   20.0f      
#define SINE_MID  32768.0f   //midscale offset (keeps values in 16-bit range)

void readADCs() {
    static uint32_t sampleN = 0; 

    // Phase advances by exactly one sample step each call. 2*pi*f*n/Fs.
    float phase = 2.0f * (float)M_PI * SINE_HZ * (float)sampleN / (float)FREQ;
    float s = sinf(phase);
    sampleN++;

    // Four different amplitudes
    dataBuffer.FL = SINE_MID +  2000.0f * s;
    dataBuffer.FR = SINE_MID +  6000.0f * s;
    dataBuffer.BL = SINE_MID + 12000.0f * s;
    dataBuffer.BR = SINE_MID + 24000.0f * s;

    char line[128];
    snprintf(line, sizeof(line), "%llu,%.6f,%.6f,%.6f,%.6f\n",
             dataBuffer.timestamp,
             dataBuffer.FL,
             dataBuffer.FR,
             dataBuffer.BL,
             dataBuffer.BR);

    xQueueSendFromISR(dataQueue, line, NULL);
}
// ---------------- SD FUNCS ----------------------
#define MOUNT_POINT "/sdcard"

sdmmc_card_t* card;

//Initialize the SD card
void init_sd_card() {
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = SD_MOSI_PIN,
        .miso_io_num = SD_MISO_PIN,
        .sclk_io_num = SD_CLK_PIN,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };

    spi_bus_initialize(host.slot, &bus_cfg, SDSPI_DEFAULT_DMA);

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = SD_CS_PIN;
    slot_config.host_id = host.slot;

    esp_err_t ret = esp_vfs_fat_sdspi_mount(
        MOUNT_POINT,
        &host,
        &slot_config,
        &mount_config,
        &card
    );
	sdAvailable = ret == ESP_OK ? true : false;
}

uint64_t timeOffset = 0;

//Make File if not existing
void init_file() {
    FILE* f = fopen("/sdcard/data.csv", "r+");

    if (!f) {
        // File doesn't exist → create it
        f = fopen("/sdcard/data.csv", "w");
        fprintf(f, "ts(us),FL,FR,BL,BR\n");
        fclose(f);
        return;
    }

    // Read first line
    char line[128];
    fgets(line, sizeof(line), f);

    if (strstr(line, "FL") == NULL) {
        // Missing header → rewrite
        fclose(f);
        f = fopen("/sdcard/data.csv", "w");
        fprintf(f, "ts(us),FL,FR,BL,BR\n");
        fclose(f);
        return;
    }
	
	
	
    // Find last line (for timestamp recovery)
	fseek(f, 0, SEEK_END);
    long pos = ftell(f);

    while (pos > 0) {
        fseek(f, --pos, SEEK_SET);
        if (fgetc(f) == '\n') break;
    }

    fgets(line, sizeof(line), f);

    char* token = strtok(line, ",");
    if (token) {
        timeOffset = atoll(token);
    }

    fclose(f);
}



// ---------------- TIMER CALLBACK ----------------
//Interrupt to sample and read data
void IRAM_ATTR onTimer(void* arg) {
    GPIO_OUT_REG ^= BIT(CONVST_PIN);

    dataBuffer.timestamp = esp_timer_get_time();

    if (GPIO_OUT_REG & BIT(CONVST_PIN)) {
        readADCs();
    }
}

// ---------------- SD TASK (SIMPLIFIED) ----------------


// Start a new SD trial file (/sdcard/trial_N.csv) this often.
#define TRIAL_ROLL_US (60LL * 1000000)   // 60 seconds

// Open /sdcard/trial_<index>.csv for writing and add the header.
static FILE* open_trial_file(int index, char* pathOut, size_t pathLen) {
    snprintf(pathOut, pathLen, "%s/trial_%d.csv", MOUNT_POINT, index);
    FILE* tf = fopen(pathOut, "w");
    if (tf) fprintf(tf, "ts(us),FL,FR,BL,BR\n");
    else    ESP_LOGW("SD", "%s open failed", pathOut);
    return tf;
}

// ---- Trial numbering continuity + power-loss logging ----------------------
#define INTERRUPT_LOG MOUNT_POINT "/interrupt.csv"

// Set by plan_trial_numbering() at boot, read by sdTask and bootLogTask.
static int g_lastTrial   = 0;   // highest trial_N.csv found from the previous session
static int g_gapMarker   = 0;   // empty trial_N.csv left as a power-loss marker (0 = none)
static int g_resumeTrial = 1;   // index sdTask starts writing at this session

// Set true by the WiFi event handler once we actually have an IP. bootLogTask
// waits on this before trying NTP — WiFi association lags boot by a few seconds.
static volatile bool s_wifiGotIp = false;

// Highest N among existing /sdcard/trial_N.csv files (0 if none).
static int find_last_trial_index(void) {
    DIR* dir = opendir(MOUNT_POINT);
    if (!dir) return 0;
    struct dirent* e;
    int maxN = 0, n;
    while ((e = readdir(dir)) != NULL) {
        if (sscanf(e->d_name, "trial_%d.csv", &n) == 1 && n > maxN) maxN = n;
    }
    closedir(dir);
    return maxN;
}

// Decide this session's starting trial index. If trials already exist, the
// previous session ended on a power loss, so leave an empty gap-marker file and
// resume two indices later (e.g. last=6 -> empty trial_7 -> resume at trial_8).
static void plan_trial_numbering(void) {
    g_lastTrial = find_last_trial_index();
    if (g_lastTrial == 0) {
        g_resumeTrial = 1;   // first boot ever: clean start, no gap
        g_gapMarker   = 0;
        return;
    }
    g_gapMarker   = g_lastTrial + 1;   // empty marker => "power loss happened here"
    g_resumeTrial = g_lastTrial + 2;
    char path[64];
    snprintf(path, sizeof(path), "%s/trial_%d.csv", MOUNT_POINT, g_gapMarker);
    FILE* m = fopen(path, "w");         // create as a 0-byte file
    if (m) fclose(m);
    else   ESP_LOGW("SD", "gap marker %s create failed", path);
}

// Append one boot record to the interrupt log (writes a header if file is new).
// whenStr is a wall-clock string, or "NA" if NTP didn't sync.
static void write_interrupt_log(const char* whenStr) {
    struct stat st;
    bool fresh = (stat(INTERRUPT_LOG, &st) != 0) || st.st_size == 0;
    FILE* f = fopen(INTERRUPT_LOG, "a");
    if (!f) { ESP_LOGW("SD", "interrupt.csv open failed"); return; }
    if (fresh)
        fprintf(f, "event,wall_time_utc,uptime_us,last_trial,gap_marker,resume_trial\n");
    fprintf(f, "%s,%s,%lld,%d,%d,%d\n",
            g_gapMarker ? "RESUME_AFTER_POWER_LOSS" : "FIRST_BOOT",
            whenStr, (long long) esp_timer_get_time(),
            g_lastTrial, g_gapMarker, g_resumeTrial);
    fclose(f);
    ESP_LOGI("SD", "interrupt log: %s at %s (resume trial_%d)",
             g_gapMarker ? "resume" : "first boot", whenStr, g_resumeTrial);
}

#define WIFI_IP_WAIT_MS   20000
#define NTP_SYNC_WAIT_MS  15000

static void bootLogTask(void* arg) {
    char when[32];
    strncpy(when, "NA", sizeof(when));

    int waited = 0;
    while (!s_wifiGotIp && waited < WIFI_IP_WAIT_MS) {
        vTaskDelay(pdMS_TO_TICKS(250));
        waited += 250;
    }

    if (!s_wifiGotIp) {
        ESP_LOGW("TIME", "no IP within %d ms -> logging NA (is the hotspot on?)", WIFI_IP_WAIT_MS);
    } else {
        ESP_LOGI("TIME", "WiFi up after ~%d ms; starting NTP", waited);
        esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
        esp_netif_sntp_init(&cfg);
        if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(NTP_SYNC_WAIT_MS)) == ESP_OK) {
            time_t now = time(NULL);
            struct tm tm_utc;
            gmtime_r(&now, &tm_utc);
            strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &tm_utc);
        } else {
            ESP_LOGW("TIME", "got IP but NTP didn't sync -> NA (hotspot sharing internet?)");
        }
        esp_netif_sntp_deinit();
    }

    write_interrupt_log(when);
    vTaskDelete(NULL);
}

//SD write task: appends to data.csv and rolls a new trial_N.csv every minute.
//Data now leaves the device over WiFi (see start_webserver), not over serial.
void sdTask(void *arg) {
    char buffer[128];

    // Rolling per-minute trial file on the SD card. Numbering continues across
    // power cycles (see plan_trial_numbering), so this is trial_1 only on the
    // very first boot; after a power loss it resumes past the gap marker.
    int trialIndex = g_resumeTrial;
    char trialPath[64];
    FILE* trial = open_trial_file(trialIndex, trialPath, sizeof(trialPath));
    int64_t fileStart = esp_timer_get_time();

    while (true) {

        // Roll over to a new trial file once a minute has elapsed.
        if (esp_timer_get_time() - fileStart >= TRIAL_ROLL_US) {
            if (trial) fclose(trial);
            trialIndex++;
            trial = open_trial_file(trialIndex, trialPath, sizeof(trialPath));
            fileStart = esp_timer_get_time();
        }

        if (uxQueueMessagesWaiting(dataQueue) >= 450) {

            // data.csv writing disabled for now — examining trial files only.
            // FILE* f = fopen("/sdcard/data.csv", "a");
            // if (!f) {
            //     ESP_LOGW("SD", "data.csv open failed");
            //     vTaskDelay(pdMS_TO_TICKS(100));
            //     continue;
            // }

            for (int i = 0; i < 450; i++) {
                if (xQueueReceive(dataQueue, buffer, 0)) {
                    // fputs(buffer, f);
                    if (trial) fputs(buffer, trial);
                }
            }

            // fclose(f);
            if (trial) fflush(trial);   // flush each batch so data reaches the card

        } else {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
}

// ---------------- WIFI ----------------

#define WIFI_SSID "test"
#define WIFI_PASS "12345678"

static void wifi_event_handler(void* arg, esp_event_base_t base,
                               int32_t id, void* data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_wifiGotIp = false;
        ESP_LOGW("WIFI", "disconnected, retrying...");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* e = (ip_event_got_ip_t*) data;
        s_wifiGotIp = true;
        ESP_LOGI("WIFI", "connected, IP: " IPSTR, IP2STR(&e->ip_info.ip));
    }
}

static void wifi_init_sta(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config = {
        .sta = { .ssid = WIFI_SSID, .password = WIFI_PASS },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
}

// ---------------- HTTP FILE SERVER (serves /sdcard) ----------------
// GET /list           -> "name,size" per line for every file on the card
// GET /get?file=NAME  -> streams that file's contents

static esp_err_t list_handler(httpd_req_t *req) {
    DIR* dir = opendir(MOUNT_POINT);
    if (!dir) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no sd"); return ESP_FAIL; }

    httpd_resp_set_type(req, "text/plain");
    struct dirent* entry;
    struct stat st;
    char path[300], line[128];
    while ((entry = readdir(dir)) != NULL) {
        snprintf(path, sizeof(path), "%s/%s", MOUNT_POINT, entry->d_name);
        long size = (stat(path, &st) == 0) ? (long) st.st_size : 0;
        int n = snprintf(line, sizeof(line), "%s,%ld\n", entry->d_name, size);
        httpd_resp_send_chunk(req, line, n);
    }
    closedir(dir);
    httpd_resp_send_chunk(req, NULL, 0);   // end response
    return ESP_OK;
}

static esp_err_t get_handler(httpd_req_t *req) {
    char query[160], fname[80];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "file", fname, sizeof(fname)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing ?file=");
        return ESP_FAIL;
    }
    if (strchr(fname, '/') || strstr(fname, "..")) {   // block path traversal
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad name");
        return ESP_FAIL;
    }

    char path[300];
    snprintf(path, sizeof(path), "%s/%s", MOUNT_POINT, fname);
    FILE* f = fopen(path, "r");
    if (!f) { httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no file"); return ESP_FAIL; }

    httpd_resp_set_type(req, "text/csv");
    char chunk[1024];
    size_t r;
    while ((r = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        if (httpd_resp_send_chunk(req, chunk, r) != ESP_OK) { fclose(f); return ESP_FAIL; }
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static void start_webserver(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t list_uri = { .uri = "/list", .method = HTTP_GET, .handler = list_handler };
        httpd_uri_t get_uri  = { .uri = "/get",  .method = HTTP_GET, .handler = get_handler };
        httpd_register_uri_handler(server, &list_uri);
        httpd_register_uri_handler(server, &get_uri);
        ESP_LOGI("HTTP", "file server started");
    } else {
        ESP_LOGW("HTTP", "file server failed to start");
    }
}

// ---------------- MAIN ----------------
//Main setup, initilizes pins and all modules.
void app_main(void) {

    // GPIO setup
    gpio_set_direction((gpio_num_t)CONVST_PIN, GPIO_MODE_OUTPUT);
    gpio_set_direction((gpio_num_t)ADC_DATA_CLK1, GPIO_MODE_OUTPUT);
    gpio_set_direction((gpio_num_t)ADC_DATA_PIN1, GPIO_MODE_INPUT);
    
    gpio_set_direction((gpio_num_t)ADC_DATA_CLK2, GPIO_MODE_OUTPUT);
    gpio_set_direction((gpio_num_t)ADC_DATA_PIN2, GPIO_MODE_INPUT);

	// Queue
	dataQueue = xQueueCreate(600, 128);  //QUEUE MUST BE MADE BEFORE TIMER, OTHERWISE ESP PANICS AND CRASHES
	
    // Timer (ESP-IDF way)
    const esp_timer_create_args_t timer_args = {
        .callback = &onTimer,
        .name = "adc_timer"
    };

    esp_timer_handle_t timer;
    esp_timer_create(&timer_args, &timer);
    esp_timer_start_periodic(timer, 1000000 / (2 * FREQ));
	
	// WiFi (joins your PC hotspot). NVS is required by the WiFi stack.
	esp_err_t nvs = nvs_flash_init();
	if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
		nvs_flash_erase();
		nvs = nvs_flash_init();
	}
	ESP_ERROR_CHECK(nvs);
	wifi_init_sta();                                        // NTP is started later, in bootLogTask

	//SD Begin
	ESP_LOGI("SD", "SD INIT");
	init_sd_card();
	if (sdAvailable) {
		// init_file();  // disabled — no longer creating/seeding data.csv
		plan_trial_numbering();                             // continue numbering; mark power-loss gap
		start_webserver();                                  // serve SD files over HTTP
		xTaskCreate(sdTask, "sdTask", 4096, NULL, 1, NULL);
		xTaskCreate(bootLogTask, "bootLog", 4096, NULL, 1, NULL);  // log boot to interrupt.csv
	} else {
		ESP_LOGI("SD", "SD FAIL");
	}
}