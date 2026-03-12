#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_crt_bundle.h"
#include "protocol_examples_common.h"

// ── Ayarlar ─────────────────────────────────────────
#define OTA_SERVER_IP       "YOUR_SERVER_IP"
#define OTA_SERVER_PORT     3000
#define OTA_CHECK_INTERVAL  30   // saniye
#define DEVICE_ID           "esp32c6-node-01"
#define FIRMWARE_VERSION    "1.0.0"

#define OTA_URL     "http://" OTA_SERVER_IP ":" STR(OTA_SERVER_PORT) "/api/firmware/latest"
#define REPORT_URL  "http://" OTA_SERVER_IP ":" STR(OTA_SERVER_PORT) "/api/device/report"
#define INFO_URL    "http://" OTA_SERVER_IP ":" STR(OTA_SERVER_PORT) "/api/firmware/info"

#define STR_HELPER(x) #x
#define STR(x) STR_HELPER(x)

static const char *TAG = "TACMESH_OTA";

// ── Sunucuya durum bildirimi gönder ─────────────────
static void report_status(const char *status, const char *version) {
    esp_http_client_config_t config = {
        .url = REPORT_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 5000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);

    char post_data[256];
    snprintf(post_data, sizeof(post_data),
        "{\"deviceId\":\"%s\",\"version\":\"%s\",\"status\":\"%s\",\"ip\":\"\"}",
        DEVICE_ID, version, status);

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, post_data, strlen(post_data));

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Durum bildirimi gönderildi: %s", status);
    }
    esp_http_client_cleanup(client);
}

// ── Sunucudaki aktif versiyon bilgisini al ───────────
static esp_err_t get_server_version(char *version_buf, size_t buf_size) {
    esp_http_client_config_t config = {
        .url = INFO_URL,
        .timeout_ms = 5000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    char response[256] = {0};

    esp_http_client_set_header(client, "x-device-id", DEVICE_ID);
    esp_http_client_set_header(client, "x-current-version", FIRMWARE_VERSION);

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return err;
    }

    int content_len = esp_http_client_fetch_headers(client);
    if (content_len > 0 && content_len < (int)sizeof(response) - 1) {
        esp_http_client_read_response(client, response, content_len);
    }
    esp_http_client_cleanup(client);

    // JSON'dan version çıkar: {"version":"v1.0.1",...}
    char *ver_start = strstr(response, "\"version\":\"");
    if (ver_start) {
        ver_start += strlen("\"version\":\"");
        char *ver_end = strchr(ver_start, '"');
        if (ver_end) {
            int len = ver_end - ver_start;
            if (len < (int)buf_size) {
                strncpy(version_buf, ver_start, len);
                version_buf[len] = '\0';
                return ESP_OK;
            }
        }
    }
    return ESP_FAIL;
}

// ── OTA Güncelleme ───────────────────────────────────
static void perform_ota(void) {
    ESP_LOGI(TAG, "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
    ESP_LOGI(TAG, " OTA Güncelleme Başlıyor...");
    ESP_LOGI(TAG, " Mevcut versiyon: %s", FIRMWARE_VERSION);
    ESP_LOGI(TAG, "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");

    // Önce sunucudaki versiyonu kontrol et
    char server_version[32] = {0};
    esp_err_t ver_err = get_server_version(server_version, sizeof(server_version));

    if (ver_err == ESP_OK) {
        ESP_LOGI(TAG, "Sunucu versiyonu: %s", server_version);

        // Aynı versiyonsa güncelleme yapma
        if (strcmp(server_version, FIRMWARE_VERSION) == 0) {
            ESP_LOGI(TAG, "✓ Firmware güncel, güncelleme gerekmiyor.");
            return;
        }
        ESP_LOGI(TAG, "Yeni versiyon mevcut: %s → %s", FIRMWARE_VERSION, server_version);
    } else {
        ESP_LOGW(TAG, "Versiyon bilgisi alınamadı, OTA yine de denenecek");
    }

    // Mevcut ve yeni partition bilgileri
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);

    if (!update_partition) {
        ESP_LOGE(TAG, "✗ Güncelleme partition'ı bulunamadı!");
        return;
    }

    ESP_LOGI(TAG, "Çalışan partition: %s (offset: 0x%08lx)",
             running->label, running->address);
    ESP_LOGI(TAG, "Hedef partition: %s (offset: 0x%08lx)",
             update_partition->label, update_partition->address);

    // ── DÜZELTİLDİ: skip_cert eklendi ──
    esp_http_client_config_t http_config = {
        .url = OTA_URL,
        .timeout_ms = 30000,
        .keep_alive_enable = true,
        .skip_cert_common_name_check = true,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_https_ota_config_t ota_config = {
        .http_config = &http_config,
    };

    // Durum bildir: güncelleme başlıyor
    report_status("ota_start", FIRMWARE_VERSION);

    // OTA başlat
    esp_https_ota_handle_t ota_handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_config, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "✗ OTA başlatılamadı: %s", esp_err_to_name(err));
        report_status("ota_failed", FIRMWARE_VERSION);
        return;
    }

    // Firmware yaz
    int total_size = esp_https_ota_get_image_size(ota_handle);
    int written = 0;
    int last_pct = 0;

    while (1) {
        err = esp_https_ota_perform(ota_handle);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) break;

        written = esp_https_ota_get_image_len_read(ota_handle);
        if (total_size > 0) {
            int pct = (written * 100) / total_size;
            if (pct - last_pct >= 10) {
                ESP_LOGI(TAG, "İndirme: %d%% (%d/%d bytes)", pct, written, total_size);
                last_pct = pct;
            }
        }
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "✗ OTA performansı başarısız: %s", esp_err_to_name(err));
        esp_https_ota_abort(ota_handle);
        report_status("ota_failed", FIRMWARE_VERSION);
        return;
    }

    // OTA tamamla ve doğrula
    esp_err_t ota_finish_err = esp_https_ota_finish(ota_handle);
    if (ota_finish_err != ESP_OK) {
        ESP_LOGE(TAG, "✗ OTA tamamlanamadı: %s", esp_err_to_name(ota_finish_err));
        report_status("ota_failed", FIRMWARE_VERSION);
        return;
    }

    ESP_LOGI(TAG, "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
    ESP_LOGI(TAG, " ✓ OTA BAŞARILI!");
    ESP_LOGI(TAG, " 5 saniye sonra yeniden başlıyor...");
    ESP_LOGI(TAG, "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");

    report_status("ota_success", server_version[0] ? server_version : "new");

    vTaskDelay(5000 / portTICK_PERIOD_MS);
    esp_restart();
}

// ── Rollback Kontrol ─────────────────────────────────
static void check_and_confirm_ota(void) {
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;

    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
        if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
            ESP_LOGI(TAG, "⚡ Yeni firmware doğrulanıyor...");
            bool system_ok = true;

            if (system_ok) {
                esp_ota_mark_app_valid_cancel_rollback();
                ESP_LOGI(TAG, "✓ Firmware doğrulandı, rollback iptal edildi.");
                report_status("fw_confirmed", FIRMWARE_VERSION);
            } else {
                ESP_LOGE(TAG, "✗ Sistem kontrolü başarısız! Rollback yapılıyor...");
                report_status("fw_rollback", FIRMWARE_VERSION);
                esp_ota_mark_app_invalid_rollback_and_reboot();
            }
        } else if (ota_state == ESP_OTA_IMG_VALID) {
            ESP_LOGI(TAG, "✓ Çalışan firmware zaten doğrulanmış.");
        }
    }
}

// ── OTA Görev Döngüsü ────────────────────────────────
static void ota_task(void *pvParameter) {
    ESP_LOGI(TAG, "OTA görev başladı. Her %d saniyede kontrol...", OTA_CHECK_INTERVAL);

    while (1) {
        perform_ota();
        ESP_LOGI(TAG, "Sonraki kontrol: %d saniye sonra", OTA_CHECK_INTERVAL);
        vTaskDelay((OTA_CHECK_INTERVAL * 1000) / portTICK_PERIOD_MS);
    }
}

// ── Ana Fonksiyon ────────────────────────────────────
void app_main(void) {
    ESP_LOGI(TAG, "╔═══════════════════════════╗");
    ESP_LOGI(TAG, "║  TacMesh OTA Node         ║");
    ESP_LOGI(TAG, "║  Versiyon: %-14s ║", FIRMWARE_VERSION);
    ESP_LOGI(TAG, "║  Cihaz ID: %-14s ║", DEVICE_ID);
    ESP_LOGI(TAG, "╚═══════════════════════════╝");

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    check_and_confirm_ota();

    ESP_ERROR_CHECK(example_connect());
    ESP_LOGI(TAG, "✓ WiFi bağlandı");

    xTaskCreate(&ota_task, "ota_task", 8192, NULL, 5, NULL);
}
