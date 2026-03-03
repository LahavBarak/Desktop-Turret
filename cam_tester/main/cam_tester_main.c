/*
 * ESP32-P4 Camera Tester
 * Captures frames from OV5647 via MIPI-CSI, encodes to JPEG, streams over USB serial.
 * Pair with cam_viewer.py on the PC side.
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_vfs_dev.h"
#include "esp_vfs_usb_serial_jtag.h"
#include "driver/uart.h"

// LDO for MIPI-CSI PHY
#include "esp_ldo_regulator.h"

// I2C master for SCCB
#include "driver/i2c_master.h"

// Camera CSI driver
#include "esp_cam_ctlr.h"
#include "esp_cam_ctlr_csi.h"

// ISP (Image Signal Processor)
#include "driver/isp.h"
#include "driver/isp_core.h"

// JPEG hardware encoder
#include "driver/jpeg_encode.h"

// Camera sensor driver (OV5647) + SCCB
#include "esp_cam_sensor.h"
#include "esp_cam_sensor_detect.h"
#include "esp_sccb_intf.h"
#include "esp_sccb_i2c.h"

static const char *TAG = "cam_tester";

// ----- Configuration -----
#define CAM_HRES          800
#define CAM_VRES          640
#define BYTES_PER_PIXEL   2        // RGB565

#define CSI_LANE_BITRATE_MBPS  200
#define CSI_DATA_LANES         2

// I2C pins for SCCB (camera sensor control bus)
// ESP32-P4 Nano CAM connector: SDA=GPIO7, SCL=GPIO8
#define SCCB_I2C_SDA_IO   GPIO_NUM_7
#define SCCB_I2C_SCL_IO   GPIO_NUM_8
#define SCCB_I2C_FREQ     100000

#define JPEG_QUALITY       40     // 1-100
#define JPEG_BUF_SIZE      (100 * 1024)

#define MIPI_LDO_CHAN_ID   3
#define MIPI_LDO_VOLTAGE   2500   // mV

// Frame framing protocol over serial
static const uint8_t FRAME_HEADER[] = {0xAA, 0x55, 0xAA, 0x55};
static const uint8_t FRAME_FOOTER[] = {0xFF, 0xD9};

// Callback: provide a new buffer for the next frame
static bool IRAM_ATTR s_camera_get_new_vb(esp_cam_ctlr_handle_t handle, esp_cam_ctlr_trans_t *trans, void *user_data)
{
    esp_cam_ctlr_trans_t *new_trans = (esp_cam_ctlr_trans_t *)user_data;
    trans->buffer = new_trans->buffer;
    trans->buflen = new_trans->buflen;
    return false;
}

// Callback: frame finished
static bool IRAM_ATTR s_camera_get_finished_trans(esp_cam_ctlr_handle_t handle, esp_cam_ctlr_trans_t *trans, void *user_data)
{
    return false;
}

// Initialize SCCB (I2C) and detect+configure OV5647 sensor
static esp_cam_sensor_device_t *init_camera_sensor(void)
{
    ESP_LOGI(TAG, "[SENSOR] Creating I2C bus (SDA=%d, SCL=%d)...", SCCB_I2C_SDA_IO, SCCB_I2C_SCL_IO);

    i2c_master_bus_handle_t i2c_bus = NULL;
    i2c_master_bus_config_t i2c_bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = SCCB_I2C_SDA_IO,
        .scl_io_num = SCCB_I2C_SCL_IO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t ret = i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "[SENSOR] I2C bus init failed: %s", esp_err_to_name(ret));
        return NULL;
    }
    ESP_LOGI(TAG, "[SENSOR] I2C bus OK");

    // Iterate through auto-detected sensor drivers
    esp_cam_sensor_detect_fn_t *p = &__esp_cam_sensor_detect_fn_array_start;
    esp_cam_sensor_detect_fn_t *end = &__esp_cam_sensor_detect_fn_array_end;
    int sensor_count = (int)(end - p);
    ESP_LOGI(TAG, "[SENSOR] Found %d sensor driver(s) in detect array", sensor_count);

    for (; p < end; p++) {
        ESP_LOGI(TAG, "[SENSOR] Trying driver: port=%d, addr=0x%02x", p->port, p->sccb_addr);

        if (p->port != ESP_CAM_SENSOR_MIPI_CSI) {
            ESP_LOGI(TAG, "[SENSOR]   Skipping (not MIPI CSI)");
            continue;
        }

        // Create SCCB I2C IO for this sensor's address
        esp_sccb_io_handle_t sccb_handle = NULL;
        sccb_i2c_config_t sccb_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = p->sccb_addr,
            .scl_speed_hz = SCCB_I2C_FREQ,
        };
        ret = sccb_new_i2c_io(i2c_bus, &sccb_cfg, &sccb_handle);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "[SENSOR]   SCCB IO failed: %s", esp_err_to_name(ret));
            continue;
        }

        esp_cam_sensor_config_t sensor_cfg = {
            .sccb_handle = sccb_handle,
            .reset_pin = -1,
            .pwdn_pin = -1,
            .xclk_pin = -1,
            .sensor_port = ESP_CAM_SENSOR_MIPI_CSI,
        };

        ESP_LOGI(TAG, "[SENSOR]   Calling detect()...");
        esp_cam_sensor_device_t *sensor = p->detect(&sensor_cfg);
        if (sensor) {
            ESP_LOGI(TAG, "[SENSOR] *** DETECTED: %s ***", sensor->name ? sensor->name : "unknown");
            return sensor;
        } else {
            ESP_LOGW(TAG, "[SENSOR]   Not detected at 0x%02x", p->sccb_addr);
            esp_sccb_del_i2c_io(sccb_handle);
        }
    }

    ESP_LOGE(TAG, "[SENSOR] No camera sensor detected! Check cable connection.");
    return NULL;
}

void app_main(void)
{
    esp_err_t ret;

    // Delay to let USB serial JTAG connect
    vTaskDelay(pdMS_TO_TICKS(5000));

    // Try bare printf first in case ESP_LOG is missing the JTAG buffer
    printf("\n\n****************************************\n");
    printf("  ESP32-P4 Camera Tester - EARLY BOOT\n");
    printf("****************************************\n\n");

    ESP_LOGW(TAG, "========================================");
    ESP_LOGW(TAG, "  ESP32-P4 Camera Tester v1.0");
    ESP_LOGW(TAG, "========================================");

    // ----- 1. LDO for MIPI-CSI PHY -----
    printf("[STEP 1] Enabling MIPI LDO...\n");
    ESP_LOGI(TAG, "[STEP 1] Enabling MIPI LDO...");
    esp_ldo_channel_handle_t ldo_mipi_phy = NULL;
    esp_ldo_channel_config_t ldo_cfg = {
        .chan_id = MIPI_LDO_CHAN_ID,
        .voltage_mv = MIPI_LDO_VOLTAGE,
    };
    ret = esp_ldo_acquire_channel(&ldo_cfg, &ldo_mipi_phy);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "[STEP 1] LDO FAILED: %s", esp_err_to_name(ret));
        return;
    }
    printf("[STEP 1] LDO OK (%.1fV)\n", MIPI_LDO_VOLTAGE / 1000.0);
    ESP_LOGI(TAG, "[STEP 1] LDO OK (%.1fV)", MIPI_LDO_VOLTAGE / 1000.0);

    // ----- 2. Camera Sensor Init -----
    printf("[STEP 2] Detecting camera sensor...\n");
    ESP_LOGI(TAG, "[STEP 2] Detecting camera sensor...");
    esp_cam_sensor_device_t *sensor = init_camera_sensor();
    if (!sensor) {
        ESP_LOGE(TAG, "[STEP 2] FAILED - No sensor detected!");
        ESP_LOGE(TAG, "         Check camera ribbon cable connection.");
        // Don't return - loop to show this is running
        while (1) {
            ESP_LOGE(TAG, "No camera. Retrying in 5s...");
            vTaskDelay(pdMS_TO_TICKS(5000));
            sensor = init_camera_sensor();
            if (sensor) break;
        }
    }
    ESP_LOGI(TAG, "[STEP 2] Sensor detected: %s", sensor->name ? sensor->name : "unknown");

    // Query and set format
    ESP_LOGI(TAG, "[STEP 2b] Querying sensor formats...");
    esp_cam_sensor_format_array_t fmt_array = {};
    ret = esp_cam_sensor_query_format(sensor, &fmt_array);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "  Sensor has %lu formats:", (unsigned long)fmt_array.count);
        for (uint32_t i = 0; i < fmt_array.count; i++) {
            ESP_LOGI(TAG, "  [%lu] %s (%ux%u@%dfps)",
                     (unsigned long)i,
                     fmt_array.format_array[i].name ? fmt_array.format_array[i].name : "n/a",
                     fmt_array.format_array[i].width,
                     fmt_array.format_array[i].height,
                     fmt_array.format_array[i].fps);
        }
    }

    // Set default format (NULL = use the Kconfig default)
    ESP_LOGI(TAG, "[STEP 2c] Setting sensor format...");
    ret = esp_cam_sensor_set_format(sensor, NULL);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "  set_format(NULL) failed: %s, trying first format...", esp_err_to_name(ret));
        if (fmt_array.count > 0) {
            ret = esp_cam_sensor_set_format(sensor, &fmt_array.format_array[0]);
        }
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "[STEP 2c] FAILED to set format: %s", esp_err_to_name(ret));
        return;
    }

    esp_cam_sensor_format_t cur_fmt = {};
    esp_cam_sensor_get_format(sensor, &cur_fmt);
    ESP_LOGI(TAG, "[STEP 2c] Active: %s (%ux%u)", cur_fmt.name ? cur_fmt.name : "default", cur_fmt.width, cur_fmt.height);

    uint16_t cam_hres = cur_fmt.width > 0 ? cur_fmt.width : CAM_HRES;
    uint16_t cam_vres = cur_fmt.height > 0 ? cur_fmt.height : CAM_VRES;
    uint32_t frame_buf_size = cam_hres * cam_vres * BYTES_PER_PIXEL;

    // ----- 3. Allocate frame buffer -----
    ESP_LOGI(TAG, "[STEP 3] Allocating frame buffer (%lu bytes)...", (unsigned long)frame_buf_size);

    // Try SPIRAM first, fall back to internal RAM with smaller buffer
    void *frame_buffer = heap_caps_aligned_calloc(64, 1, frame_buf_size, MALLOC_CAP_SPIRAM);
    if (!frame_buffer) {
        ESP_LOGW(TAG, "  SPIRAM alloc failed, trying internal RAM...");
        frame_buffer = heap_caps_aligned_calloc(64, 1, frame_buf_size, MALLOC_CAP_DEFAULT);
    }
    if (!frame_buffer) {
        ESP_LOGE(TAG, "[STEP 3] FAILED to allocate frame buffer!");
        return;
    }
    ESP_LOGI(TAG, "[STEP 3] Frame buffer at %p (%lu bytes)", frame_buffer, (unsigned long)frame_buf_size);

    esp_cam_ctlr_trans_t trans = {
        .buffer = frame_buffer,
        .buflen = frame_buf_size,
    };

    // ----- 4. CSI Controller Init -----
    ESP_LOGI(TAG, "[STEP 4] Init CSI controller (%ux%u)...", cam_hres, cam_vres);
    esp_cam_ctlr_csi_config_t csi_cfg = {
        .ctlr_id = 0,
        .h_res = cam_hres,
        .v_res = cam_vres,
        .lane_bit_rate_mbps = CSI_LANE_BITRATE_MBPS,
        .input_data_color_type = CAM_CTLR_COLOR_RAW8,
        .output_data_color_type = CAM_CTLR_COLOR_RGB565,
        .data_lane_num = CSI_DATA_LANES,
        .byte_swap_en = false,
        .queue_items = 1,
    };

    esp_cam_ctlr_handle_t cam_handle = NULL;
    ret = esp_cam_new_csi_ctlr(&csi_cfg, &cam_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "[STEP 4] CSI init FAILED: %s", esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "[STEP 4] CSI OK");

    esp_cam_ctlr_evt_cbs_t cbs = {
        .on_get_new_trans = s_camera_get_new_vb,
        .on_trans_finished = s_camera_get_finished_trans,
    };
    ESP_ERROR_CHECK(esp_cam_ctlr_register_event_callbacks(cam_handle, &cbs, &trans));

    // ----- 5. ISP Init -----
    ESP_LOGI(TAG, "[STEP 5] Init ISP (RAW8 -> RGB565)...");
    isp_proc_handle_t isp_proc = NULL;
    esp_isp_processor_cfg_t isp_cfg = {
        .clk_hz = 80 * 1000 * 1000,
        .input_data_source = ISP_INPUT_DATA_SOURCE_CSI,
        .input_data_color_type = ISP_COLOR_RAW8,
        .output_data_color_type = ISP_COLOR_RGB565,
        .has_line_start_packet = true,
        .has_line_end_packet = false,
        .h_res = cam_hres,
        .v_res = cam_vres,
    };
    ret = esp_isp_new_processor(&isp_cfg, &isp_proc);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "[STEP 5] ISP init FAILED: %s", esp_err_to_name(ret));
        return;
    }
    ESP_ERROR_CHECK(esp_isp_enable(isp_proc));
    ESP_LOGI(TAG, "[STEP 5] ISP OK");

    // ----- 6. JPEG Encoder Init -----
    ESP_LOGI(TAG, "[STEP 6] Init JPEG encoder...");
    jpeg_encoder_handle_t jpeg_encoder = NULL;
    jpeg_encode_engine_cfg_t jpeg_eng_cfg = {
        .intr_priority = 0,
        .timeout_ms = 1000,
    };
    ret = jpeg_new_encoder_engine(&jpeg_eng_cfg, &jpeg_encoder);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "[STEP 6] JPEG engine FAILED: %s", esp_err_to_name(ret));
        return;
    }

    jpeg_encode_memory_alloc_cfg_t jpeg_out_mem_cfg = {
        .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER,
    };
    size_t jpeg_out_buf_actual_size = 0;
    uint8_t *jpeg_out_buf = (uint8_t *)jpeg_alloc_encoder_mem(JPEG_BUF_SIZE, &jpeg_out_mem_cfg, &jpeg_out_buf_actual_size);
    if (!jpeg_out_buf) {
        ESP_LOGE(TAG, "[STEP 6] JPEG buffer alloc FAILED");
        return;
    }
    ESP_LOGI(TAG, "[STEP 6] JPEG OK (quality=%d, buf=%zuKB)", JPEG_QUALITY, jpeg_out_buf_actual_size / 1024);

    jpeg_encode_cfg_t jpeg_cfg = {
        .height = cam_vres,
        .width = cam_hres,
        .src_type = JPEG_ENCODE_IN_FORMAT_RGB565,
        .sub_sample = JPEG_DOWN_SAMPLING_YUV422,
        .image_quality = JPEG_QUALITY,
    };

    // ----- 7. Start Camera -----
    ESP_LOGI(TAG, "[STEP 7] Starting camera...");
    ret = esp_cam_ctlr_enable(cam_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "[STEP 7] cam_ctlr_enable FAILED: %s", esp_err_to_name(ret));
        return;
    }
    ret = esp_cam_ctlr_start(cam_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "[STEP 7] cam_ctlr_start FAILED: %s", esp_err_to_name(ret));
        return;
    }

    int stream_on = 1;
    ret = esp_cam_sensor_ioctl(sensor, ESP_CAM_SENSOR_IOC_S_STREAM, &stream_on);
    ESP_LOGI(TAG, "[STEP 7] Stream on: %s", esp_err_to_name(ret));

    ESP_LOGW(TAG, "========================================");
    ESP_LOGW(TAG, "  STREAMING STARTED - use cam_viewer.py");
    ESP_LOGW(TAG, "========================================");

    // Wait for logs to flush, then suppress all logging
    // (logs share the USB serial with binary JPEG data)
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_log_level_set("*", ESP_LOG_NONE);

    // CRITICAL: Disable CRLF translation on UART output
    esp_vfs_dev_uart_port_set_tx_line_endings(CONFIG_ESP_CONSOLE_UART_NUM, ESP_LINE_ENDINGS_LF);

    // CRITICAL: Boost the UART baud rate from 115200 (boot) to 2000000 (video stream)
    uart_set_baudrate(CONFIG_ESP_CONSOLE_UART_NUM, 2000000);

    // ----- 8. Main Loop -----
    while (1) {
        ret = esp_cam_ctlr_receive(cam_handle, &trans, 2000);
        if (ret != ESP_OK) {
            continue;
        }

        esp_cache_msync(frame_buffer, frame_buf_size, ESP_CACHE_MSYNC_FLAG_DIR_M2C);

        uint32_t jpeg_size = 0;
        ret = jpeg_encoder_process(jpeg_encoder, &jpeg_cfg,
                                   (const uint8_t *)frame_buffer, frame_buf_size,
                                   jpeg_out_buf, jpeg_out_buf_actual_size,
                                   &jpeg_size);
        if (ret != ESP_OK || jpeg_size == 0) {
            continue;
        }

        // Send: [HEADER 4B][LENGTH 4B LE][JPEG DATA][FOOTER 2B]
        uint8_t len_bytes[4] = {
            (uint8_t)(jpeg_size & 0xFF),
            (uint8_t)((jpeg_size >> 8) & 0xFF),
            (uint8_t)((jpeg_size >> 16) & 0xFF),
            (uint8_t)((jpeg_size >> 24) & 0xFF),
        };

        fwrite(FRAME_HEADER, 1, sizeof(FRAME_HEADER), stdout);
        fwrite(len_bytes, 1, 4, stdout);
        fwrite(jpeg_out_buf, 1, jpeg_size, stdout);
        fwrite(FRAME_FOOTER, 1, sizeof(FRAME_FOOTER), stdout);
        fflush(stdout);
    }
}
