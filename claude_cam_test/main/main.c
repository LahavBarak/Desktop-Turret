/*
 * claude_cam_test — ESP32-P4 Nano OV5647 camera streaming
 *
 * Pipeline:
 *   OV5647 (RAW8, MIPI-CSI)
 *     → CSI controller
 *       → ISP  (RAW8 → RGB565)
 *         → JPEG hardware encoder
 *           → UART0 @ 2 Mbaud
 *             → PC (viewer.py)
 *
 * Wire protocol:
 *   [0xAA 0x55 0xAA 0x55]  magic  (4 bytes)
 *   [length]               uint32 little-endian (4 bytes)
 *   [JPEG payload]         <length> bytes
 *   [0xFF 0xD9]            footer (2 bytes)
 *
 * ── Key constraints ────────────────────────────────────────────────────────
 *
 *  UART binary writes: always uart_write_bytes(), NEVER printf/fwrite.
 *    The VFS layer performs CRLF translation (0x0A → 0x0D 0x0A), silently
 *    corrupting every JPEG frame.  uart_write_bytes() is raw; no translation.
 *
 *  Baud rate: fixed in sdkconfig.defaults (CONFIG_ESP_CONSOLE_UART_BAUDRATE).
 *    Do NOT call uart_set_baudrate() during a live stream.
 *
 *  UART driver: must be installed before calling uart_write_bytes().
 *    The console UART has no driver installed by default in ESP-IDF 5.x.
 *    We install it with a 128 KB TX ring buffer so writes never block.
 *
 *  Cache coherency: call esp_cache_msync() after each DMA receive to
 *    invalidate CPU D-cache so the CPU reads what the DMA wrote to SPIRAM.
 *
 *  Logging: disable all ESP-IDF log output before the streaming loop.
 *    Any log byte on UART0 breaks the binary protocol.  We drain the TX
 *    FIFO with a 50 ms delay before silencing, then install the UART driver.
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
#include "driver/i2c_master.h"
#include "driver/isp.h"
#include "driver/isp_core.h"
#include "driver/jpeg_encode.h"

#include "esp_ldo_regulator.h"
#include "esp_cam_ctlr.h"
#include "esp_cam_ctlr_csi.h"
#include "esp_cam_sensor.h"
#include "esp_cam_sensor_detect.h"
#include "esp_sccb_intf.h"
#include "esp_sccb_i2c.h"

/* ── Hardware pin / LDO config ─────────────────────────────────────────────── */

#define SCCB_SDA        GPIO_NUM_7   /* OV5647 I2C data */
#define SCCB_SCL        GPIO_NUM_8   /* OV5647 I2C clock */
#define SCCB_SPEED_HZ   100000

#define MIPI_LDO_CHAN   3            /* MIPI PHY power rail */
#define MIPI_LDO_MV     2500

#define CSI_DATA_LANES  2
#define CSI_MBPS        200          /* per-lane bit rate */

/* ── Stream parameters ─────────────────────────────────────────────────────── */

#define BYTES_PER_PX    2            /* RGB565 */
#define FALLBACK_W      800
#define FALLBACK_H      640

#define JPEG_QUALITY    30           /* 1–100; lower = smaller frames, higher FPS */
#define JPEG_BUF_BYTES  (200 * 1024)

/* ── Frame protocol ────────────────────────────────────────────────────────── */

static const uint8_t MAGIC[4]  = {0xAA, 0x55, 0xAA, 0x55};
static const uint8_t FOOTER[2] = {0xFF, 0xD9};

/* ── DMA frame buffer (single, shared via ISR callbacks) ───────────────────── */

static void   *s_fb       = NULL;
static size_t  s_fb_bytes = 0;

/* Called from ISR after each completed DMA transfer.
 * We always re-arm with the same buffer, accepting that a slow encode cycle
 * means the new incoming frame overwrites the previous one (frame drop). */
static bool IRAM_ATTR csi_on_get_trans(esp_cam_ctlr_handle_t handle,
                                       esp_cam_ctlr_trans_t  *trans,
                                       void                  *ctx)
{
    trans->buffer = s_fb;
    trans->buflen = s_fb_bytes;
    return false;
}

static bool IRAM_ATTR csi_on_trans_done(esp_cam_ctlr_handle_t handle,
                                        esp_cam_ctlr_trans_t  *trans,
                                        void                  *ctx)
{
    return false;
}

/* ── Raw UART helper ───────────────────────────────────────────────────────── */

static void uart_raw_write(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    while (len > 0) {
        int wrote = uart_write_bytes(CONFIG_ESP_CONSOLE_UART_NUM, p, len);
        if (wrote <= 0) break;
        p   += (size_t)wrote;
        len -= (size_t)wrote;
    }
}

/* ── Step 1: Enable MIPI PHY LDO ──────────────────────────────────────────── */

static void init_mipi_ldo(void)
{
    esp_ldo_channel_handle_t ldo = NULL;
    esp_ldo_channel_config_t cfg = {
        .chan_id    = MIPI_LDO_CHAN,
        .voltage_mv = MIPI_LDO_MV,
    };
    ESP_ERROR_CHECK(esp_ldo_acquire_channel(&cfg, &ldo));
    ESP_LOGI("init", "MIPI LDO ch%d @ %d mV — OK", MIPI_LDO_CHAN, MIPI_LDO_MV);
}

/* ── Step 2: Detect OV5647 via SCCB (I2C) ─────────────────────────────────── */

static esp_cam_sensor_device_t *detect_sensor(void)
{
    /* Init I2C master bus */
    i2c_master_bus_handle_t bus = NULL;
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port                     = I2C_NUM_0,
        .sda_io_num                   = SCCB_SDA,
        .scl_io_num                   = SCCB_SCL,
        .clk_source                   = I2C_CLK_SRC_DEFAULT,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus));
    ESP_LOGI("init", "I2C bus OK  SDA=%d  SCL=%d", SCCB_SDA, SCCB_SCL);

    /* Walk the sensor detect-function table built by esp_cam_sensor component */
    esp_cam_sensor_detect_fn_t *fn  = &__esp_cam_sensor_detect_fn_array_start;
    esp_cam_sensor_detect_fn_t *end = &__esp_cam_sensor_detect_fn_array_end;
    ESP_LOGI("init", "%d sensor driver(s) in detection table", (int)(end - fn));

    for (; fn < end; fn++) {
        if (fn->port != ESP_CAM_SENSOR_MIPI_CSI)
            continue;

        ESP_LOGI("init", "  Probing addr 0x%02x ...", fn->sccb_addr);

        esp_sccb_io_handle_t sccb = NULL;
        sccb_i2c_config_t sccb_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address  = fn->sccb_addr,
            .scl_speed_hz    = SCCB_SPEED_HZ,
        };
        if (sccb_new_i2c_io(bus, &sccb_cfg, &sccb) != ESP_OK)
            continue;

        esp_cam_sensor_config_t sensor_cfg = {
            .sccb_handle = sccb,
            .reset_pin   = -1,
            .pwdn_pin    = -1,
            .xclk_pin    = -1,
            .sensor_port = ESP_CAM_SENSOR_MIPI_CSI,
        };
        esp_cam_sensor_device_t *dev = fn->detect(&sensor_cfg);
        if (dev) {
            ESP_LOGI("init", "  Found: %s", dev->name ? dev->name : "(unknown)");
            return dev;
        }
        esp_sccb_del_i2c_io(sccb);
    }

    return NULL;
}

/* ── Step 3: Select streaming format ──────────────────────────────────────── */
/*
 * Strategy: prefer 640×480 (lowest bandwidth, most stable); fall back to the
 * format with the smallest pixel count; as a last resort fall back to index 0.
 */
static const esp_cam_sensor_format_t *
select_format(const esp_cam_sensor_format_array_t *fmts)
{
    if (!fmts || fmts->count == 0) return NULL;

    ESP_LOGI("init", "Sensor reports %lu format(s):", (unsigned long)fmts->count);
    for (uint32_t i = 0; i < fmts->count; i++) {
        const esp_cam_sensor_format_t *f = &fmts->format_array[i];
        ESP_LOGI("init", "  [%lu]  %-20s  %4u×%-4u  %d fps",
                 (unsigned long)i,
                 f->name   ? f->name   : "?",
                 f->width, f->height, f->fps);
    }

    /* Prefer 640×480 */
    for (uint32_t i = 0; i < fmts->count; i++) {
        const esp_cam_sensor_format_t *f = &fmts->format_array[i];
        if (f->width == 640 && f->height == 480)
            return f;
    }

    /* Smallest pixel count */
    const esp_cam_sensor_format_t *best = &fmts->format_array[0];
    for (uint32_t i = 1; i < fmts->count; i++) {
        const esp_cam_sensor_format_t *f = &fmts->format_array[i];
        if ((uint32_t)f->width * f->height < (uint32_t)best->width * best->height)
            best = f;
    }
    return best;
}

/* ═══════════════════════════════════════════════════════════════════════════ */

void app_main(void)
{
    esp_err_t ret;

    /* Small delay so the USB-serial adapter has time to enumerate on the PC */
    vTaskDelay(pdMS_TO_TICKS(2000));

    ESP_LOGW("main", "═══ claude_cam_test ═══");

    /* ── 1. MIPI PHY power ─────────────────────────────────────────────────── */
    init_mipi_ldo();

    /* ── 2. Camera sensor detection — retry until found ───────────────────── */
    esp_cam_sensor_device_t *sensor = NULL;
    for (int attempt = 1; !sensor; attempt++) {
        sensor = detect_sensor();
        if (!sensor) {
            ESP_LOGE("init", "No sensor found (attempt %d) — check ribbon cable, retrying in 3 s", attempt);
            vTaskDelay(pdMS_TO_TICKS(3000));
        }
    }

    /* ── 3. Format selection ───────────────────────────────────────────────── */
    esp_cam_sensor_format_array_t fmts = {};
    ESP_ERROR_CHECK(esp_cam_sensor_query_format(sensor, &fmts));

    const esp_cam_sensor_format_t *chosen = select_format(&fmts);
    if (!chosen) {
        ESP_LOGE("init", "Sensor returned no formats — cannot continue");
        return;
    }

    ret = esp_cam_sensor_set_format(sensor, chosen);
    if (ret != ESP_OK) {
        ESP_LOGW("init", "set_format('%s') → %s — falling back to driver default",
                 chosen->name ? chosen->name : "?", esp_err_to_name(ret));
        ret = esp_cam_sensor_set_format(sensor, NULL);   /* NULL = Kconfig default */
        ESP_ERROR_CHECK(ret);
    }

    /* Read back the format that was actually applied */
    esp_cam_sensor_format_t active = {};
    esp_cam_sensor_get_format(sensor, &active);
    uint16_t W = active.width  > 0 ? active.width  : FALLBACK_W;
    uint16_t H = active.height > 0 ? active.height : FALLBACK_H;
    ESP_LOGI("init", "Active: %s  %u×%u @ %d fps",
             active.name ? active.name : "?", W, H, active.fps);

    /* ── 4. Frame buffer ───────────────────────────────────────────────────── */
    s_fb_bytes = (size_t)W * H * BYTES_PER_PX;
    ESP_LOGI("init", "Allocating frame buffer: %zu bytes (~%zu KB)", s_fb_bytes, s_fb_bytes / 1024);

    /* 64-byte aligned for DMA; prefer SPIRAM */
    s_fb = heap_caps_aligned_calloc(64, 1, s_fb_bytes, MALLOC_CAP_SPIRAM);
    if (!s_fb)
        s_fb = heap_caps_aligned_calloc(64, 1, s_fb_bytes, MALLOC_CAP_DEFAULT);
    if (!s_fb) {
        ESP_LOGE("init", "Frame buffer alloc FAILED (need %zu bytes)", s_fb_bytes);
        return;
    }
    ESP_LOGI("init", "Frame buffer @ %p — OK", s_fb);

    esp_cam_ctlr_trans_t recv_trans = { .buffer = s_fb, .buflen = s_fb_bytes };

    /* ── 5. CSI controller ─────────────────────────────────────────────────── */
    ESP_LOGI("init", "CSI: %u×%u  %d-lane  %d Mbps  RAW8→RGB565",
             W, H, CSI_DATA_LANES, CSI_MBPS);

    esp_cam_ctlr_csi_config_t csi_cfg = {
        .ctlr_id                = 0,
        .h_res                  = W,
        .v_res                  = H,
        .lane_bit_rate_mbps     = CSI_MBPS,
        .input_data_color_type  = CAM_CTLR_COLOR_RAW8,
        .output_data_color_type = CAM_CTLR_COLOR_RGB565,
        .data_lane_num          = CSI_DATA_LANES,
        .byte_swap_en           = false,
        .queue_items            = 1,
    };
    esp_cam_ctlr_handle_t cam = NULL;
    ESP_ERROR_CHECK(esp_cam_new_csi_ctlr(&csi_cfg, &cam));

    esp_cam_ctlr_evt_cbs_t cbs = {
        .on_get_new_trans  = csi_on_get_trans,
        .on_trans_finished = csi_on_trans_done,
    };
    ESP_ERROR_CHECK(esp_cam_ctlr_register_event_callbacks(cam, &cbs, &recv_trans));
    ESP_LOGI("init", "CSI controller — OK");

    /* ── 6. ISP (de-Bayer RAW8 → RGB565) ──────────────────────────────────── */
    ESP_LOGI("init", "ISP ...");
    isp_proc_handle_t isp = NULL;
    esp_isp_processor_cfg_t isp_cfg = {
        .clk_hz                 = 80 * 1000 * 1000,
        .input_data_source      = ISP_INPUT_DATA_SOURCE_CSI,
        .input_data_color_type  = ISP_COLOR_RAW8,
        .output_data_color_type = ISP_COLOR_RGB565,
        .has_line_start_packet  = true,
        .has_line_end_packet    = false,
        .h_res                  = W,
        .v_res                  = H,
    };
    ESP_ERROR_CHECK(esp_isp_new_processor(&isp_cfg, &isp));
    ESP_ERROR_CHECK(esp_isp_enable(isp));
    ESP_LOGI("init", "ISP — OK");

    /* ── 7. JPEG hardware encoder ──────────────────────────────────────────── */
    ESP_LOGI("init", "JPEG encoder (quality=%d) ...", JPEG_QUALITY);
    jpeg_encoder_handle_t jpeg_enc = NULL;
    jpeg_encode_engine_cfg_t eng_cfg = {
        .intr_priority = 0,
        .timeout_ms    = 50,   /* 50 ms is more than enough for one frame */
    };
    ESP_ERROR_CHECK(jpeg_new_encoder_engine(&eng_cfg, &jpeg_enc));

    jpeg_encode_memory_alloc_cfg_t out_mem_cfg = {
        .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER,
    };
    size_t jpeg_out_actual = 0;
    uint8_t *jpeg_out = (uint8_t *)jpeg_alloc_encoder_mem(
        JPEG_BUF_BYTES, &out_mem_cfg, &jpeg_out_actual);
    if (!jpeg_out) {
        ESP_LOGE("init", "JPEG output buffer alloc FAILED");
        return;
    }
    ESP_LOGI("init", "JPEG encoder — OK  (out buf %zu KB)", jpeg_out_actual / 1024);

    jpeg_encode_cfg_t jpeg_cfg = {
        .width         = W,
        .height        = H,
        .src_type      = JPEG_ENCODE_IN_FORMAT_RGB565,
        .sub_sample    = JPEG_DOWN_SAMPLING_YUV422,
        .image_quality = JPEG_QUALITY,
    };

    /* ── 8. Start the camera ───────────────────────────────────────────────── */
    ESP_LOGI("init", "Starting camera ...");
    ESP_ERROR_CHECK(esp_cam_ctlr_enable(cam));
    ESP_ERROR_CHECK(esp_cam_ctlr_start(cam));

    int stream_on = 1;
    ret = esp_cam_sensor_ioctl(sensor, ESP_CAM_SENSOR_IOC_S_STREAM, &stream_on);
    if (ret != ESP_OK)
        ESP_LOGW("init", "stream_on ioctl: %s (may be benign)", esp_err_to_name(ret));

    ESP_LOGW("main", "Camera streaming — run  python viewer.py  on the PC");

    /* ── 9. Switch UART0 to raw binary mode ────────────────────────────────────
     *
     * We must NOT let any ESP-IDF log byte reach UART0 after this point.
     * Sequence:
     *   a) Disable all logging.
     *   b) Wait 50 ms to drain any bytes already queued in the FIFO.
     *   c) Delete the existing UART driver (no-op if none installed).
     *   d) Re-install with a 128 KB TX ring buffer.
     *      This makes uart_write_bytes() non-blocking for any normal JPEG size
     *      and ensures truly raw output — no CRLF translation.
     *      The baud rate is NOT changed here; sdkconfig already set it to
     *      2 Mbaud and reinstalling the driver preserves that.
     */
    esp_log_level_set("*", ESP_LOG_NONE);
    vTaskDelay(pdMS_TO_TICKS(50));

    uart_driver_delete(CONFIG_ESP_CONSOLE_UART_NUM);
    ESP_ERROR_CHECK(uart_driver_install(
        CONFIG_ESP_CONSOLE_UART_NUM,
        0,            /* RX buffer — not needed */
        128 * 1024,   /* TX ring buffer */
        0, NULL, 0));

    /* ── 10. Streaming loop ────────────────────────────────────────────────── */
    uint32_t frame_count = 0;
    int64_t  t_last_fps  = esp_timer_get_time();
    uint32_t last_jpeg   = 0;

    while (1) {
        /* Block until one DMA frame is ready (2 s timeout) */
        ret = esp_cam_ctlr_receive(cam, &recv_trans, pdMS_TO_TICKS(2000));
        if (ret != ESP_OK)
            continue;

        /*
         * Invalidate D-cache lines covering the DMA-written region.
         * Without this the CPU may read stale cached data instead of the
         * pixel data the DMA controller wrote to SPIRAM.
         */
        esp_cache_msync(s_fb, s_fb_bytes, ESP_CACHE_MSYNC_FLAG_DIR_M2C);

        /* Compress to JPEG */
        uint32_t jpeg_len = 0;
        ret = jpeg_encoder_process(jpeg_enc, &jpeg_cfg,
                                   (const uint8_t *)s_fb, s_fb_bytes,
                                   jpeg_out, jpeg_out_actual,
                                   &jpeg_len);
        if (ret != ESP_OK || jpeg_len == 0)
            continue;

        /*
         * Emit one frame on UART0:
         *   [MAGIC 4B][length 4B LE][JPEG payload][FOOTER 2B]
         */
        uint8_t hdr[8];
        memcpy(hdr, MAGIC, 4);
        hdr[4] = (uint8_t)( jpeg_len        & 0xFF);
        hdr[5] = (uint8_t)((jpeg_len >>  8) & 0xFF);
        hdr[6] = (uint8_t)((jpeg_len >> 16) & 0xFF);
        hdr[7] = (uint8_t)((jpeg_len >> 24) & 0xFF);

        uart_raw_write(hdr,      8);
        uart_raw_write(jpeg_out, jpeg_len);
        uart_raw_write(FOOTER,   2);

        frame_count++;
        last_jpeg = jpeg_len;

        /* Heartbeat via JTAG every 5 s (separate peripheral; does not touch UART0) */
        int64_t now = esp_timer_get_time();
        if (now - t_last_fps >= 5000000LL) {
            float fps = (float)frame_count / ((float)(now - t_last_fps) * 1e-6f);
            esp_log_level_set("main", ESP_LOG_INFO);
            ESP_LOGI("main", "%.1f fps  last JPEG %lu B", fps, (unsigned long)last_jpeg);
            esp_log_level_set("*",    ESP_LOG_NONE);
            frame_count = 0;
            t_last_fps  = now;
        }
    }
}
