/*
 * ESP32-P4 Camera Tester
 *
 * Captures OV5647 frames via MIPI-CSI, JPEG-encodes in hardware, streams
 * over USB serial at 2 Mbaud.  Pair with cam_viewer.py on the PC.
 *
 * Key design decisions vs. the original:
 *
 *  1. Console UART starts at 2 Mbaud (set in sdkconfig.defaults, not at runtime).
 *     There is no mid-stream baud-rate switch; the viewer just syncs to the
 *     first frame header and discards the initial boot noise.
 *
 *  2. Binary frames are sent with uart_write_bytes(), NOT fwrite()/printf().
 *     The VFS/stdio path can silently insert 0x0D before every 0x0A byte in
 *     the JPEG payload (CRLF translation), corrupting every frame.
 *     uart_write_bytes() is raw: no translation, no hidden buffering.
 *     We install a 128 KB TX ring buffer so the call never truncates.
 *
 *  3. Single frame buffer fed to esp_cam_ctlr_receive().  The on_get_new_trans
 *     callback keeps returning the same buffer so the camera is always ready
 *     for the next frame; frames that arrive while we are encoding/transmitting
 *     are simply overwritten (the sensor runs at 50 FPS, we stream at ~15–25
 *     FPS after JPEG compression + serial bandwidth).
 *
 * Wire format (must match cam_viewer.py):
 *   [0xAA 0x55 0xAA 0x55][length 4B LE][JPEG data][0xFF 0xD9]
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "driver/uart.h"
#include "esp_ldo_regulator.h"
#include "driver/i2c_master.h"
#include "esp_cam_ctlr.h"
#include "esp_cam_ctlr_csi.h"
#include "driver/isp.h"
#include "driver/isp_core.h"
#include "driver/jpeg_encode.h"
#include "esp_cam_sensor.h"
#include "esp_cam_sensor_detect.h"
#include "esp_sccb_intf.h"
#include "esp_sccb_i2c.h"

static const char *TAG = "cam";

/* ── hardware ─────────────────────────────────────────────────────────── */
#define SCCB_SDA            GPIO_NUM_7
#define SCCB_SCL            GPIO_NUM_8
#define SCCB_FREQ_HZ        100000

#define CSI_LANE_BITRATE_MBPS   200
#define CSI_DATA_LANES          2

#define MIPI_LDO_CHAN       3
#define MIPI_LDO_MV         2500

/* ── stream ───────────────────────────────────────────────────────────── */
#define CAM_HRES            800
#define CAM_VRES            640
#define BYTES_PER_PX        2       /* RGB565 */

/*
 * JPEG quality 25:
 *   800×640 frame → typically 8–18 KB after encode.
 *   At 2 Mbaud = 250 KB/s effective throughput, that yields ~14–30 FPS
 *   over the wire.  Raise to 40 for better image quality at the cost of FPS.
 */
#define JPEG_QUALITY        25
#define JPEG_BUF_SIZE       (150 * 1024)

/* ── protocol ─────────────────────────────────────────────────────────── */
static const uint8_t FRAME_MAGIC[4]  = {0xAA, 0x55, 0xAA, 0x55};
static const uint8_t FRAME_FOOTER[2] = {0xFF, 0xD9};

/* ── single frame buffer (shared with DMA via on_get_new_trans) ──────── */
static void  *s_frame_buf;
static size_t s_frame_bytes;

static bool IRAM_ATTR on_get_new_trans(esp_cam_ctlr_handle_t handle,
                                       esp_cam_ctlr_trans_t *trans,
                                       void *user_data)
{
    /*
     * Called in ISR after each completed frame to set up the next DMA target.
     * We always hand back the same single buffer so the camera never stalls.
     * If the CPU is still encoding, the new incoming frame will overwrite the
     * previous one — this is acceptable frame-drop behaviour.
     */
    trans->buffer = s_frame_buf;
    trans->buflen = s_frame_bytes;
    return false;
}

static bool IRAM_ATTR on_trans_finished(esp_cam_ctlr_handle_t handle,
                                        esp_cam_ctlr_trans_t *trans,
                                        void *user_data)
{
    return false;
}

/* ── sensor init ─────────────────────────────────────────────────────── */

static esp_cam_sensor_device_t *init_sensor(void)
{
    i2c_master_bus_handle_t bus = NULL;
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port               = I2C_NUM_0,
        .sda_io_num             = SCCB_SDA,
        .scl_io_num             = SCCB_SCL,
        .clk_source             = I2C_CLK_SRC_DEFAULT,
        .flags.enable_internal_pullup = true,
    };
    if (i2c_new_master_bus(&bus_cfg, &bus) != ESP_OK) {
        ESP_LOGE(TAG, "I2C init failed");
        return NULL;
    }
    ESP_LOGI(TAG, "I2C bus SDA=%d SCL=%d OK", SCCB_SDA, SCCB_SCL);

    esp_cam_sensor_detect_fn_t *p   = &__esp_cam_sensor_detect_fn_array_start;
    esp_cam_sensor_detect_fn_t *end = &__esp_cam_sensor_detect_fn_array_end;
    ESP_LOGI(TAG, "%d sensor driver(s) in detect table", (int)(end - p));

    for (; p < end; p++) {
        if (p->port != ESP_CAM_SENSOR_MIPI_CSI)
            continue;

        ESP_LOGI(TAG, "  Trying 0x%02x ...", p->sccb_addr);

        esp_sccb_io_handle_t sccb = NULL;
        sccb_i2c_config_t sccb_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address  = p->sccb_addr,
            .scl_speed_hz    = SCCB_FREQ_HZ,
        };
        if (sccb_new_i2c_io(bus, &sccb_cfg, &sccb) != ESP_OK)
            continue;

        esp_cam_sensor_config_t cfg = {
            .sccb_handle = sccb,
            .reset_pin   = -1,
            .pwdn_pin    = -1,
            .xclk_pin    = -1,
            .sensor_port = ESP_CAM_SENSOR_MIPI_CSI,
        };
        esp_cam_sensor_device_t *dev = p->detect(&cfg);
        if (dev) {
            ESP_LOGI(TAG, "  Detected: %s", dev->name ? dev->name : "?");
            return dev;
        }
        esp_sccb_del_i2c_io(sccb);
    }

    ESP_LOGE(TAG, "No sensor found — check ribbon cable");
    return NULL;
}

/* ── raw UART write (no VFS, no CRLF translation) ────────────────────── */

static void uart_send(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    while (len > 0) {
        int n = uart_write_bytes(CONFIG_ESP_CONSOLE_UART_NUM, p, len);
        if (n <= 0) break;
        p   += n;
        len -= (size_t)n;
    }
}

/* ── app_main ────────────────────────────────────────────────────────── */

void app_main(void)
{
    esp_err_t ret;

    /* Give the USB-serial adapter time to enumerate */
    vTaskDelay(pdMS_TO_TICKS(3000));

    ESP_LOGW(TAG, "=== ESP32-P4 Camera Tester ===");

    /* 1 — MIPI PHY LDO */
    ESP_LOGI(TAG, "[1] MIPI LDO ch%d @ %d mV", MIPI_LDO_CHAN, MIPI_LDO_MV);
    esp_ldo_channel_handle_t ldo = NULL;
    esp_ldo_channel_config_t ldo_cfg = { .chan_id = MIPI_LDO_CHAN, .voltage_mv = MIPI_LDO_MV };
    ret = esp_ldo_acquire_channel(&ldo_cfg, &ldo);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "LDO failed: %s", esp_err_to_name(ret)); return; }
    ESP_LOGI(TAG, "[1] LDO OK");

    /* 2 — Detect sensor; retry forever so the log makes the problem obvious */
    ESP_LOGI(TAG, "[2] Detecting camera sensor ...");
    esp_cam_sensor_device_t *sensor = NULL;
    while (!sensor) {
        sensor = init_sensor();
        if (!sensor) {
            ESP_LOGE(TAG, "No sensor — retrying in 5 s (check ribbon cable)");
            vTaskDelay(pdMS_TO_TICKS(5000));
        }
    }

    /* List and set format */
    esp_cam_sensor_format_array_t fmts = {};
    if (esp_cam_sensor_query_format(sensor, &fmts) == ESP_OK) {
        ESP_LOGI(TAG, "%lu format(s) available:", (unsigned long)fmts.count);
        for (uint32_t i = 0; i < fmts.count; i++) {
            ESP_LOGI(TAG, "  [%lu] %s  %ux%u @ %dfps",
                (unsigned long)i,
                fmts.format_array[i].name ? fmts.format_array[i].name : "?",
                fmts.format_array[i].width,
                fmts.format_array[i].height,
                fmts.format_array[i].fps);
        }
    }

    ret = esp_cam_sensor_set_format(sensor, NULL); /* NULL → Kconfig default */
    if (ret != ESP_OK && fmts.count > 0)
        ret = esp_cam_sensor_set_format(sensor, &fmts.format_array[0]);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "set_format failed: %s", esp_err_to_name(ret)); return; }

    esp_cam_sensor_format_t cur = {};
    esp_cam_sensor_get_format(sensor, &cur);
    uint16_t hres = cur.width  > 0 ? cur.width  : CAM_HRES;
    uint16_t vres = cur.height > 0 ? cur.height : CAM_VRES;
    ESP_LOGI(TAG, "[2] Active: %s  %ux%u", cur.name ? cur.name : "default", hres, vres);

    /* 3 — Frame buffer (64-byte aligned for DMA) */
    s_frame_bytes = (size_t)hres * vres * BYTES_PER_PX;
    ESP_LOGI(TAG, "[3] Allocating frame buffer (%zu B) ...", s_frame_bytes);
    s_frame_buf = heap_caps_aligned_calloc(64, 1, s_frame_bytes, MALLOC_CAP_SPIRAM);
    if (!s_frame_buf)
        s_frame_buf = heap_caps_aligned_calloc(64, 1, s_frame_bytes, MALLOC_CAP_DEFAULT);
    if (!s_frame_buf) { ESP_LOGE(TAG, "Frame buffer alloc failed"); return; }
    ESP_LOGI(TAG, "[3] Frame buffer @ %p OK", s_frame_buf);

    esp_cam_ctlr_trans_t recv_trans = { .buffer = s_frame_buf, .buflen = s_frame_bytes };

    /* 4 — CSI controller */
    ESP_LOGI(TAG, "[4] CSI init (%ux%u, %d-lane, %d Mbps) ...",
             hres, vres, CSI_DATA_LANES, CSI_LANE_BITRATE_MBPS);
    esp_cam_ctlr_csi_config_t csi_cfg = {
        .ctlr_id                = 0,
        .h_res                  = hres,
        .v_res                  = vres,
        .lane_bit_rate_mbps     = CSI_LANE_BITRATE_MBPS,
        .input_data_color_type  = CAM_CTLR_COLOR_RAW8,
        .output_data_color_type = CAM_CTLR_COLOR_RGB565,
        .data_lane_num          = CSI_DATA_LANES,
        .byte_swap_en           = false,
        .queue_items            = 1,
    };
    esp_cam_ctlr_handle_t cam = NULL;
    ret = esp_cam_new_csi_ctlr(&csi_cfg, &cam);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "CSI init failed: %s", esp_err_to_name(ret)); return; }

    esp_cam_ctlr_evt_cbs_t cbs = {
        .on_get_new_trans  = on_get_new_trans,
        .on_trans_finished = on_trans_finished,
    };
    ESP_ERROR_CHECK(esp_cam_ctlr_register_event_callbacks(cam, &cbs, &recv_trans));
    ESP_LOGI(TAG, "[4] CSI OK");

    /* 5 — ISP (RAW8 → RGB565) */
    ESP_LOGI(TAG, "[5] ISP init ...");
    isp_proc_handle_t isp = NULL;
    esp_isp_processor_cfg_t isp_cfg = {
        .clk_hz                 = 80 * 1000 * 1000,
        .input_data_source      = ISP_INPUT_DATA_SOURCE_CSI,
        .input_data_color_type  = ISP_COLOR_RAW8,
        .output_data_color_type = ISP_COLOR_RGB565,
        .has_line_start_packet  = true,
        .has_line_end_packet    = false,
        .h_res                  = hres,
        .v_res                  = vres,
    };
    ret = esp_isp_new_processor(&isp_cfg, &isp);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "ISP init failed: %s", esp_err_to_name(ret)); return; }
    ESP_ERROR_CHECK(esp_isp_enable(isp));
    ESP_LOGI(TAG, "[5] ISP OK");

    /* 6 — JPEG hardware encoder */
    ESP_LOGI(TAG, "[6] JPEG encoder init (quality=%d) ...", JPEG_QUALITY);
    jpeg_encoder_handle_t jpeg_enc = NULL;
    jpeg_encode_engine_cfg_t eng_cfg = { .intr_priority = 0, .timeout_ms = 1000 };
    ret = jpeg_new_encoder_engine(&eng_cfg, &jpeg_enc);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "JPEG engine failed: %s", esp_err_to_name(ret)); return; }

    jpeg_encode_memory_alloc_cfg_t out_mem_cfg = { .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER };
    size_t jpeg_out_actual = 0;
    uint8_t *jpeg_out = (uint8_t *)jpeg_alloc_encoder_mem(JPEG_BUF_SIZE, &out_mem_cfg, &jpeg_out_actual);
    if (!jpeg_out) { ESP_LOGE(TAG, "JPEG buf alloc failed"); return; }
    ESP_LOGI(TAG, "[6] JPEG OK (output buf %zu KB)", jpeg_out_actual / 1024);

    jpeg_encode_cfg_t jpeg_cfg = {
        .height        = vres,
        .width         = hres,
        .src_type      = JPEG_ENCODE_IN_FORMAT_RGB565,
        .sub_sample    = JPEG_DOWN_SAMPLING_YUV422,
        .image_quality = JPEG_QUALITY,
    };

    /* 7 — Start camera */
    ESP_LOGI(TAG, "[7] Starting camera ...");
    ret = esp_cam_ctlr_enable(cam);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "ctlr_enable: %s", esp_err_to_name(ret)); return; }
    ret = esp_cam_ctlr_start(cam);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "ctlr_start: %s", esp_err_to_name(ret)); return; }

    int stream_on = 1;
    ret = esp_cam_sensor_ioctl(sensor, ESP_CAM_SENSOR_IOC_S_STREAM, &stream_on);
    if (ret != ESP_OK) ESP_LOGW(TAG, "stream_on: %s", esp_err_to_name(ret));

    ESP_LOGW(TAG, "[7] Streaming — run cam_viewer.py on the PC");

    /*
     * 8 — Switch the UART into raw binary mode.
     *
     * We disable all ESP-IDF logging first (so no text bytes leak into the
     * binary stream), wait 50 ms for any already-queued log bytes to drain
     * at 2 Mbaud, then:
     *
     *   a) Delete the console UART driver (if one was installed by ESP-IDF).
     *      This is safe because logging is now off.
     *   b) Reinstall it with a 128 KB TX ring buffer.  This makes
     *      uart_write_bytes() non-blocking for typical JPEG frame sizes and —
     *      crucially — sends raw bytes with zero CRLF translation.
     *   c) Re-assert 2 Mbaud in case the reinstall reset the hardware register.
     *      (sdkconfig already boots at 2 Mbaud; this is belt-and-suspenders.)
     */
    esp_log_level_set("*", ESP_LOG_NONE);
    vTaskDelay(pdMS_TO_TICKS(50));

    uart_driver_delete(CONFIG_ESP_CONSOLE_UART_NUM);   /* no-op if not installed */
    ESP_ERROR_CHECK(uart_driver_install(CONFIG_ESP_CONSOLE_UART_NUM,
                                        /*rx_buf=*/0,
                                        /*tx_buf=*/128 * 1024,
                                        /*queue_size=*/0, NULL, 0));
    uart_set_baudrate(CONFIG_ESP_CONSOLE_UART_NUM, 2000000);

    /* 9 — Main streaming loop */
    uint32_t frame_count = 0;
    int64_t  log_timer   = esp_timer_get_time();

    while (1) {
        ret = esp_cam_ctlr_receive(cam, &recv_trans, pdMS_TO_TICKS(2000));
        if (ret != ESP_OK)
            continue;

        /* Invalidate D-cache lines covering the DMA buffer so the CPU
         * sees what the CSI DMA controller actually wrote to SPIRAM. */
        esp_cache_msync(s_frame_buf, s_frame_bytes, ESP_CACHE_MSYNC_FLAG_DIR_M2C);

        /* JPEG encode */
        uint32_t jpeg_size = 0;
        ret = jpeg_encoder_process(jpeg_enc, &jpeg_cfg,
                                   (const uint8_t *)s_frame_buf, s_frame_bytes,
                                   jpeg_out, jpeg_out_actual,
                                   &jpeg_size);
        if (ret != ESP_OK || jpeg_size == 0)
            continue;

        /* Transmit frame: [magic 4B][length 4B LE][jpeg][footer 2B] */
        uint8_t hdr[8];
        memcpy(hdr, FRAME_MAGIC, 4);
        hdr[4] = (uint8_t)(jpeg_size & 0xFF);
        hdr[5] = (uint8_t)((jpeg_size >>  8) & 0xFF);
        hdr[6] = (uint8_t)((jpeg_size >> 16) & 0xFF);
        hdr[7] = (uint8_t)((jpeg_size >> 24) & 0xFF);

        uart_send(hdr,         8);
        uart_send(jpeg_out,    jpeg_size);
        uart_send(FRAME_FOOTER, 2);

        frame_count++;

        /* Emit a heartbeat on the JTAG debug console every 5 s.
         * This does NOT affect the UART stream (different peripheral
         * when USB-JTAG is enabled; silently dropped otherwise). */
        int64_t now = esp_timer_get_time();
        if (now - log_timer >= 5000000LL) {
            float fps = (float)frame_count / ((float)(now - log_timer) / 1e6f);
            esp_log_level_set(TAG, ESP_LOG_INFO);
            ESP_LOGI(TAG, "%.1f FPS  last JPEG %lu B", fps, (unsigned long)jpeg_size);
            esp_log_level_set("*", ESP_LOG_NONE);
            frame_count = 0;
            log_timer   = now;
        }
    }
}
