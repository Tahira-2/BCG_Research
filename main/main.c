#include <stdio.h>
#include <string.h>
#include <stdlib.h>

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

#include <math.h>

#include <dirent.h>
#include <sys/stat.h>
#include <time.h>

// --------------- OTA --------------------
#include "esp_ota_ops.h"

// -------------- Reset -------------------
#include "esp_system.h"

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


// Driven by weightTask: true only while the learned subject is on the bed.
// Starts false — nothing is recorded until presence is confirmed.
static volatile bool collecting = false;
static volatile bool ota_in_progress = false;
bool sdAvailable = false;

// Load-cell channels, in the order readADCs() fills them. J1 (data1) carries
// FL/FR, J2 (data2) carries BL/BR; the second field on each port is a
// daisy-chained cell. A full rig populates all four (see g_cal[].enabled).
enum { CH_FL, CH_FR, CH_BL, CH_BR, NUM_CH };

static volatile float g_rawCounts[NUM_CH] = {0};    //Latest raw ADC counts per channel.


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

// ---------------- ADC READ ----------------
//Read the ADCs on the board and add the data to a buffer for uploading
void readADCs() {
    ets_delay_us(10);

    uint64_t data1 = 0;
	uint64_t data2 = 0;

    for (int i = 0; i < 8; i++) {
        data1 = (data1 << 8) | shiftInCustom(ADC_DATA_PIN1, ADC_DATA_CLK1);
		data2 = (data2 << 8) | shiftInCustom(ADC_DATA_PIN2, ADC_DATA_CLK2);

    }
	// Data is stored as a 64-bit value, separated into 4 16-bit fields for each ADC
    dataBuffer.FL = (data1 >> 48 & 0xFFFF);
	dataBuffer.FR = (data1 >> 16 & 0xFFFF);
	dataBuffer.BL = (data2 >> 48 & 0xFFFF);
	dataBuffer.BR = (data2 >> 16 & 0xFFFF);

    // Publish each channel every sample so weightTask can watch the bed for subject's presence
    g_rawCounts[CH_FL] = dataBuffer.FL;
    g_rawCounts[CH_FR] = dataBuffer.FR;
    g_rawCounts[CH_BL] = dataBuffer.BL;
    g_rawCounts[CH_BR] = dataBuffer.BR;

    if (!collecting)
        return;     // subject not on the bed -> don't record

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

//Make File if not existing     - no longer in use
void init_file() {
    FILE* f = fopen("/sdcard/data.csv", "r+");

    if (!f) {
        f = fopen("/sdcard/data.csv", "w");
        fprintf(f, "ts(us),FL,FR,BL,BR\n");
        fclose(f);
        return;
    }

    // Read first line
    char line[128];
    fgets(line, sizeof(line), f);

    if (strstr(line, "FL") == NULL) {
        // Missing header => rewrite
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

// ---------------- SD TASK ----------------

// Start a new SD trial file (/sdcard/trial_N.csv) this often.
#define TRIAL_ROLL_US (60LL * 1000000 * 2.5)   // 5 min

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
#define DATA_LOG      MOUNT_POINT "/data_log.csv"

// Set by plan_trial_numbering() at boot, read by sdTask and bootLogTask.
static int g_lastTrial   = 0;   
static int g_gapMarker   = 0;   
static int g_resumeTrial = 1;   

static volatile bool s_wifiGotIp = false;   // Set true by the WiFi event handler once we actually have an IP.

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

// Decide this session's starting trial index. 
static void plan_trial_numbering(void) {
    g_lastTrial = find_last_trial_index();
    if (g_lastTrial == 0) {
        g_resumeTrial = 1;   
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

// Append one row to data_log.csv each time `collecting` flips 
// Columns: Time, s_weight, c_weight, collecting -- 1 while collecting.
// Time is a UTC string once NTP has set the clock, else "uptime_<s>s".
static void write_data_log(float s_weight, float c_weight, bool collecting) {
    struct stat st;
    bool fresh = (stat(DATA_LOG, &st) != 0) || st.st_size == 0;
    FILE* f = fopen(DATA_LOG, "a");
    if (!f) { ESP_LOGW("SD", "data_log.csv open failed"); return; }

    char when[32];
    time_t now = time(NULL);
    if (now > 1700000000) {                 // clock set by NTP
        struct tm tm_utc;
        gmtime_r(&now, &tm_utc);
        strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &tm_utc);
    } else {
        snprintf(when, sizeof(when), "uptime_%llds",
                 (long long)(esp_timer_get_time() / 1000000));
    }

    if (fresh)
        fprintf(f, "Time,s_weight,c_weight,collecting\n");
    fprintf(f, "%s,%.1f,%.1f,%d\n", when, s_weight, c_weight, collecting ? 1 : 0);
    fclose(f);
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

//SD rolls a new trial_N.csv every minute.
void sdTask(void *arg) {
    char buffer[128];
    int trialIndex = g_resumeTrial;
    char trialPath[64];

    FILE* trial = open_trial_file(trialIndex, trialPath, sizeof(trialPath));

    int64_t fileStart = esp_timer_get_time();

    bool prevCollecting = true;   

    while (true) {
        if (collecting && !prevCollecting)
            ESP_LOGI("CYCLE", "collection started at trial #%d", trialIndex);

        if (!collecting && prevCollecting)
            ESP_LOGI("CYCLE", "collection stopped at/after trial #%d", trialIndex);

        prevCollecting = collecting;

        // Stopped: flush and close the in-progress trial file, then idle
        // until collecting goes true again.
        if (!collecting && trial) {
            while (xQueueReceive(dataQueue, buffer, 0)) {
                fputs(buffer, trial);
            }
            fflush(trial);
            fclose(trial);
            trial = NULL;
        }

        if (!collecting) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        // Resume handling: start a fresh trial file so post-resume samples
        if (collecting && !trial) {
            trialIndex++;
            trial = open_trial_file(trialIndex, trialPath, sizeof(trialPath));
            fileStart = esp_timer_get_time();
        }

        // Roll over to a new trial file once the interval has elapsed.
        if ( collecting && trial && (esp_timer_get_time() - fileStart >= TRIAL_ROLL_US)) {
            fclose(trial);
            trialIndex++;
            trial = open_trial_file(trialIndex, trialPath, sizeof(trialPath));
            fileStart = esp_timer_get_time();
        }


        if (uxQueueMessagesWaiting(dataQueue) >= 450) {
            for (int i = 0; i < 450; i++) {
                if (xQueueReceive(dataQueue, buffer, 0)) {
                    // fputs(buffer, f);
                    if (trial) fputs(buffer, trial);
                }
            }
            if (trial) fflush(trial);   // flush each batch so data reaches the card
        } else {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
}

// ---------------- BED PRESENCE / SUBJECT WEIGHT ----------------
/* Recording runs only while the learned subject is on the bed.
   flash/OTA reboot -> forget everything, re-tare to whatever is on the bed
                       (mattress, bedding), then learn the next person as the
                       new subject
   power loss       -> restore tare + subject from NVS, resume immediately if
                       that subject is on the bed
 Every transition must hold for PRESENCE_STABLE_US before it is acted on
 */

#define WEIGHT_NS         "bcg"      // NVS namespace
#define KEY_TARE          "tare"     // empty-bed weight in lb
#define KEY_SUBJECT       "subject"  // learned subject weight (lb)

// !! CALIBRATE !! Per-channel gain: raw ADC counts per pound for each load cell,
// in channel order (FL, FR, BL, BR). Each channel is converted to pounds with
// its own gain before the channels are summed (see bed_weight_lb). 
static float g_countsPerLb[NUM_CH] = { 20.0f, 20.0f, 20.0f, 20.0f };

#define SUBJECT_TOL_LB    5.0f       // subject recognised within +/- this
#define PRESENCE_MIN_LB   15.0f      // below this the bed counts as empty
#define SETTLE_NOISE_LB   2.0f       // drift allowed while still "stable"
#define PRESENCE_STABLE_US (5LL * 1000000)   // a condition must hold this long
#define WEIGHT_POLL_MS    100

// Calibration aid. Set to 0 once g_countsPerLb[] and SETTLE_NOISE_LB are dialled in.
#define WEIGHT_DEBUG      0
#define WEIGHT_DEBUG_US   (1LL * 1000000)    // one "WCAL" line per second

// Reset reasons that mean "new deployment": drop the stored subject and re-tare.
// ESP_RST_USB/JTAG = freshly flashed, ESP_RST_SW = esp_restart() after OTA.
static bool is_fresh_deployment(esp_reset_reason_t r) {
    return r == ESP_RST_USB || r == ESP_RST_JTAG ||
           r == ESP_RST_SW  || r == ESP_RST_UNKNOWN;
}

static esp_err_t weight_nvs_get(const char* key, float* out) {
    nvs_handle_t h;
    if (nvs_open(WEIGHT_NS, NVS_READONLY, &h) != ESP_OK) return ESP_FAIL;
    size_t sz = sizeof(float);
    esp_err_t err = nvs_get_blob(h, key, out, &sz);
    nvs_close(h);
    return (err == ESP_OK && sz == sizeof(float)) ? ESP_OK : ESP_FAIL;
}

static void weight_nvs_set(const char* key, float value) {
    nvs_handle_t h;
    if (nvs_open(WEIGHT_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW("WEIGHT", "nvs_open failed, %s not persisted", key);
        return;
    }
    if (nvs_set_blob(h, key, &value, sizeof(value)) == ESP_OK) nvs_commit(h);
    else ESP_LOGW("WEIGHT", "nvs_set_blob(%s) failed", key);
    nvs_close(h);
}

static void weight_nvs_erase(void) {
    nvs_handle_t h;
    if (nvs_open(WEIGHT_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_all(h);
    nvs_commit(h);
    nvs_close(h);
}

// Gross bed load in pounds: each channel scaled by its own gain, then summed.
static float bed_weight_lb(void) {
    float total = 0.0f;
    for (int i = 0; i < NUM_CH; i++)
        total += g_rawCounts[i] / g_countsPerLb[i];
    return total;
}

// Tracks how long a weight reading has stayed put, for taring and for learning
// a new subject. Any drift beyond SETTLE_NOISE_LB restarts the window.
typedef struct {
    float   referenceLb;   // value the window is measuring drift against
    int64_t windowStartUs; // when the current settled window began
    bool    tracking;      // false = no window open yet (first call / after reset)
} WeightSettleTracker;

// True once the weight has held within SETTLE_NOISE_LB for PRESENCE_STABLE_US.
static bool weight_has_settled(WeightSettleTracker* tracker, float weightLb, int64_t nowUs) {
    if (!tracker->tracking || fabsf(weightLb - tracker->referenceLb) > SETTLE_NOISE_LB) {
        tracker->referenceLb   = weightLb;
        tracker->windowStartUs = nowUs;
        tracker->tracking      = true;
        return false;
    }
    return (nowUs - tracker->windowStartUs) >= PRESENCE_STABLE_US;
}

// Tracks how long the on-bed/off-bed verdict has held
typedef struct {
    bool    lastVerdict;   // the on-bed/off-bed value currently being timed
    int64_t windowStartUs; // when that verdict was first seen
    bool    tracking;      // false = no verdict timed yet
} PresenceDebouncer;

// True once the same verdict has held for PRESENCE_STABLE_US.
static bool presence_has_held(PresenceDebouncer* debouncer, bool isOnBed, int64_t nowUs) {
    if (!debouncer->tracking || debouncer->lastVerdict != isOnBed) {
        debouncer->lastVerdict   = isOnBed;
        debouncer->windowStartUs = nowUs;
        debouncer->tracking      = true;
        return false;
    }
    return (nowUs - debouncer->windowStartUs) >= PRESENCE_STABLE_US;
}

static void weightTask(void* arg) {
    esp_reset_reason_t resetReason = esp_reset_reason();   // decides remember vs. re-tare

    float emptyBedLb   = 0.0f;    // tare: what the bed reads with nobody on it
    float subjectLb    = 0.0f;    // the person this deployment is recording
    bool  haveTare     = false;   // emptyBedLb is valid
    bool  haveSubject  = false;   // subjectLb is valid

    if (is_fresh_deployment(resetReason)) {
        weight_nvs_erase();
        ESP_LOGI("WEIGHT", "reset=%d (flash/OTA) -> re-taring, subject forgotten", resetReason);
    } else {
        haveTare    = (weight_nvs_get(KEY_TARE, &emptyBedLb) == ESP_OK);
        haveSubject = (weight_nvs_get(KEY_SUBJECT, &subjectLb) == ESP_OK) && subjectLb > 0.0f;
        ESP_LOGI("WEIGHT", "reset=%d (power loss) -> tare=%s subject=%s",
                 resetReason,
                 haveTare    ? "restored" : "none",
                 haveSubject ? "restored" : "none");
        if (haveTare)    ESP_LOGI("WEIGHT", "  empty bed = %.1f lb", emptyBedLb);
        if (haveSubject) ESP_LOGI("WEIGHT", "  subject   = %.1f lb", subjectLb);
    }

    WeightSettleTracker settleTracker    = {0};   // times the tare / subject-learning windows
    PresenceDebouncer   presenceDebouncer = {0};  // times the on-bed / off-bed verdict

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(WEIGHT_POLL_MS));

        int64_t nowUs        = esp_timer_get_time();
        float   totalLoadLb  = bed_weight_lb();   // gain-corrected sum of all cells

#if WEIGHT_DEBUG
        {
            static int64_t lastDebugUs  = 0;        // when the last WCAL line printed
            static float   netMinLb     = 0.0f;     // lowest net seen this window
            static float   netMaxLb     = 0.0f;     // highest net seen this window
            static bool    spreadOpen   = false;    // false = window needs re-seeding

            float netLb = haveTare ? (totalLoadLb - emptyBedLb) : 0.0f;

            if (!spreadOpen) { netMinLb = netMaxLb = netLb; spreadOpen = true; }
            if (netLb < netMinLb) netMinLb = netLb;
            if (netLb > netMaxLb) netMaxLb = netLb;

            if (nowUs - lastDebugUs >= WEIGHT_DEBUG_US) {
                lastDebugUs = nowUs;
                ESP_LOGI("WCAL",
                    "FL=%.0f FR=%.0f BL=%.0f BR=%.0f | net=%.1f p2p=%.1f | tare=%s subj=%s coll=%d",
                    g_rawCounts[CH_FL], g_rawCounts[CH_FR],
                    g_rawCounts[CH_BL], g_rawCounts[CH_BR],
                    netLb, netMaxLb - netMinLb,
                    haveTare    ? "yes" : "no",
                    haveSubject ? "yes" : "no",
                    (int)collecting);
                spreadOpen = false;   // start a fresh peak-to-peak window
            }
        }
#endif

        // 1. Tare: the first stable reading after a flash/OTA is the empty bed.
        if (!haveTare) {
            if (weight_has_settled(&settleTracker, totalLoadLb, nowUs)) {
                emptyBedLb = totalLoadLb;
                haveTare   = true;
                weight_nvs_set(KEY_TARE, emptyBedLb);
                settleTracker.tracking = false;   // reuse the tracker for subject learning
                ESP_LOGI("WEIGHT", "tared: empty bed = %.1f lb", emptyBedLb);
            }
            continue;
        }

        float bedLoadLb = totalLoadLb - emptyBedLb;   // load on top of the empty bed

        // 2. Learn the subject: the first stable occupancy after taring.
        if (!haveSubject) {
            if (bedLoadLb >= PRESENCE_MIN_LB &&
                weight_has_settled(&settleTracker, bedLoadLb, nowUs)) {
                subjectLb   = bedLoadLb;
                haveSubject = true;
                weight_nvs_set(KEY_SUBJECT, subjectLb);
                ESP_LOGI("WEIGHT", "subject learned: %.1f lb", subjectLb);
            } else if (bedLoadLb < PRESENCE_MIN_LB) {
                settleTracker.tracking = false;   // bed empty again, restart the window
            }
            continue;
        }

        // 3. Steady state: is that subject on the bed right now?
        bool isOnBed = fabsf(bedLoadLb - subjectLb) <= SUBJECT_TOL_LB;

        if (presence_has_held(&presenceDebouncer, isOnBed, nowUs) && collecting != isOnBed) {
            collecting = isOnBed;
            ESP_LOGI("WEIGHT", "%s (bed %.1f lb vs subject %.1f lb)",
                     isOnBed ? "subject on bed -> collecting"
                             : "subject off bed -> stopped",
                     bedLoadLb, subjectLb);
            if (sdAvailable)
                write_data_log(subjectLb, bedLoadLb, collecting);  // log the transition
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

    //for any wifi event -- call esp-event-handler
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    //for the specific IP received -- call esp-event-handler
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

    // --------------- List ----------------------
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

// ------------------- Get File Content --------------------
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

// ---------------- ESP Firmware Update OTA  --------------
static esp_err_t esp_update_handler(httpd_req_t* req){

    ota_in_progress = true;
    //TO-DO:    Add a check for unfinished update -> esp_ota_resume()
    ESP_LOGI("OTA Handler", "OTA request received");

    //find the next partition
    const esp_partition_t* partition = esp_ota_get_next_update_partition(NULL);
    if (partition == NULL){
        ESP_LOGE("Next Partition", "Passive OTA Partition NOT found");
        ota_in_progress = false;
        return ESP_ERR_NOT_FOUND;
    }
    esp_ota_handle_t handle = 0;
    ESP_LOGI("OTA Handler", "OTA Process Beginning");
    esp_err_t ota = esp_ota_begin(partition, OTA_SIZE_UNKNOWN, &handle);

    if(ota != ESP_OK){
        //error
        ESP_LOGE("OTA Handler", "OTA Begin failed: %s", esp_err_to_name(ota));
        ota_in_progress = false;
        return ota;
    }
    ESP_LOGI("OTP Handler", "OTA-begin returned success");

    //read from buffer
    char ota_buffer[1024];
    int remaining = req->content_len;
    int recv_chunk;

    while(remaining > 0){
        recv_chunk = httpd_req_recv(req, ota_buffer, MIN(remaining, sizeof(ota_buffer)));
        
        //error
        if (recv_chunk <= 0){
            if (recv_chunk == HTTPD_SOCK_ERR_TIMEOUT) 
            continue;   //time out -- try again
            
            esp_ota_abort(handle);  // clean up after error
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "receive failed");
            ota_in_progress = false;
            return ESP_FAIL;
        }
    
        //success -- write
        ota = esp_ota_write(handle, (const void*) ota_buffer, recv_chunk);
        
        if( ota != ESP_OK){
            //error
            esp_ota_abort(handle);
            ESP_LOGI("OTA Handler", "Abbort was called -- write failed");
            ESP_LOGE("OTA Handler", "OTA Write Failed: %s", esp_err_to_name(ota));
            ota_in_progress = false;
            return ota;
        }
        remaining -= recv_chunk;
    }
    ESP_LOGI("OTA-Handler", "OTA-write returned success");

    //ota end
    ota = esp_ota_end(handle);
    if(ota != ESP_OK){
        ESP_LOGE("OTA Handler", "Firmware Failed Validation: %s", esp_err_to_name(ota));
        ota_in_progress = false;
        return ota;
    }
    ESP_LOGI("OTA Handler", "OTA-end returned success");

    ota = esp_ota_set_boot_partition(partition);
    if (ota != ESP_OK){
        ESP_LOGE("OTA Handler", "Couldn't Select New firmware: %s", esp_err_to_name(ota));
        ota_in_progress = false;
        return ota;       
    }

    //success
    ESP_LOGI("OTA Handler", "OTA process ended successfully -- reboot starts");
    httpd_resp_sendstr(req, "Update successful, rebooting...");

    //Delay for http to receive signal
    vTaskDelay(pdMS_TO_TICKS(2000));

    ota_in_progress = false;
    esp_restart();
}

// ---------------- ESP Firmware Update OTA  --------------
static esp_err_t esp_update_handler(httpd_req_t* req){

    ota_in_progress = true;
    //TO-DO:    Add a check for unfinished update -> esp_ota_resume()
    ESP_LOGI("OTA Handler", "OTA request received");

    //find the next partition
    const esp_partition_t* partition = esp_ota_get_next_update_partition(NULL);
    if (partition == NULL){
        ESP_LOGE("Next Partition", "Passive OTA Partition NOT found");
        ota_in_progress = false;
        return ESP_ERR_NOT_FOUND;
    }
    esp_ota_handle_t handle = 0;
    ESP_LOGI("OTA Handler", "OTA Process Beginning");
    esp_err_t ota = esp_ota_begin(partition, OTA_SIZE_UNKNOWN, &handle);

    if(ota != ESP_OK){
        //error
        ESP_LOGE("OTA Handler", "OTA Begin failed: %s", esp_err_to_name(ota));
        ota_in_progress = false;
        return ota;
    }
    ESP_LOGI("OTP Handler", "OTA-begin returned success");

    //read from buffer
    char ota_buffer[1024];
    int remaining = req->content_len;
    int recv_chunk;

    while(remaining > 0){
        recv_chunk = httpd_req_recv(req, ota_buffer, MIN(remaining, sizeof(ota_buffer)));
        
        //error
        if (recv_chunk <= 0){
            if (recv_chunk == HTTPD_SOCK_ERR_TIMEOUT) 
            continue;   //time out -- try again
            
            esp_ota_abort(handle);  // clean up after error
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "receive failed");
            ota_in_progress = false;
            return ESP_FAIL;
        }
    
        //success -- write
        ota = esp_ota_write(handle, (const void*) ota_buffer, recv_chunk);
        
        if( ota != ESP_OK){
            //error
            esp_ota_abort(handle);
            ESP_LOGI("OTA Handler", "Abbort was called -- write failed");
            ESP_LOGE("OTA Handler", "OTA Write Failed: %s", esp_err_to_name(ota));
            ota_in_progress = false;
            return ota;
        }
        remaining -= recv_chunk;
    }
    ESP_LOGI("OTA-Handler", "OTA-write returned success");s

    //ota end
    ota = esp_ota_end(handle);
    if(ota != ESP_OK){
        ESP_LOGE("OTA Handler", "Firmware Failed Validation: %s", esp_err_to_name(ota));
        ota_in_progress = false;
        return ota;
    }
    ESP_LOGI("OTA Handler", "OTA-end returned success");

    ota = esp_ota_set_boot_partition(partition);
    if (ota != ESP_OK){
        ESP_LOGE("OTA Handler", "Couldn't Select New firmware: %s", esp_err_to_name(ota));
        ota_in_progress = false;
        return ota;       
    }

    //success
    ESP_LOGI("OTA Handler", "OTA process ended successfully -- reboot starts");
    httpd_resp_sendstr(req, "Update successful, rebooting...");

    //Delay for http to receive signal
    vTaskDelay(pdMS_TO_TICKS(2000));

    ota_in_progress = false;
    esp_restart();
}




static void start_webserver(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t list_uri = { .uri = "/list", .method = HTTP_GET, .handler = list_handler };
        httpd_uri_t get_uri  = { .uri = "/get",  .method = HTTP_GET, .handler = get_handler };
        httpd_uri_t ota_uri = { .uri = "/update", .method = HTTP_POST, .handler = esp_update_handler};

        httpd_register_uri_handler(server, &list_uri);
        httpd_register_uri_handler(server, &get_uri);
        httpd_register_uri_handler(server, &ota_uri);

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
	dataQueue = xQueueCreate(600, 128);  //QUEUE MUST BE MADE BEFORE TIMER
	
    // Timer (ESP-IDF way)
    const esp_timer_create_args_t timer_args = {
        .callback = &onTimer,
        .name = "adc_timer"
    };

    esp_timer_handle_t timer;
    esp_timer_create(&timer_args, &timer);
    esp_timer_start_periodic(timer, 1000000 / (2 * FREQ));
	
	// WiFi
	esp_err_t nvs = nvs_flash_init();
	if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
		nvs_flash_erase();
		nvs = nvs_flash_init();
	}
	ESP_ERROR_CHECK(nvs);

	// Bed presence: drives `collecting`. Started after NVS (it reads the stored
	// tare/subject) and after the timer (it needs live weight samples).
	xTaskCreate(weightTask, "weightTask", 4096, NULL, 1, NULL);

	wifi_init_sta();                                        // NTP is started later, in bootLogTask

	//SD Begin
	ESP_LOGI("SD", "SD INIT");
	init_sd_card();
	if (sdAvailable && !ota_in_progress) {
		// init_file();  // disabled — no longer creating/seeding data.csv
		plan_trial_numbering();                             // continue numbering; mark power-loss gap
		start_webserver();                                  // serve SD files over HTTP
		xTaskCreate(sdTask, "sdTask", 4096, NULL, 1, NULL);
		xTaskCreate(bootLogTask, "bootLog", 4096, NULL, 1, NULL);  // log boot to interrupt.csv
	} else {
		ESP_LOGI("SD", "SD FAIL");
	}
}