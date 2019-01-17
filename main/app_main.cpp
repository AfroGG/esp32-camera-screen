/* ESPRESSIF MIT License
 *
 * Copyright (c) 2018 <ESPRESSIF SYSTEMS (SHANGHAI) PTE LTD>
 *
 * Permission is hereby granted for use on all ESPRESSIF SYSTEMS products, in which case,
 * it is free of charge, to any person obtaining a copy of this software and associated
 * documentation files (the "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the Software is furnished
 * to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or
 * substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "app_main.h"
#include "esp_partition.h"

#include "esp_log.h"
#include "image_util.h"
#include "fb_gfx.h"

#include "app_screen.h"
#include "esp_log.h"

#include "app_sd.h"
#include "app_sensor.h"

static const char *TAG = "[main]";

en_fsm_state g_state = WAIT_FOR_WAKEUP;
int g_is_enrolling = 0;
int g_is_deleting = 0;


extern CEspLcd *tft;
static struct bme280_dev dev;

#if 1


#define FACE_COLOR_WHITE  0x00FFFFFF
#define FACE_COLOR_BLACK  0x00000000
#define FACE_COLOR_RED    0x000000FF
#define FACE_COLOR_GREEN  0x0000FF00
#define FACE_COLOR_BLUE   0x00FF0000
#define FACE_COLOR_YELLOW (FACE_COLOR_RED | FACE_COLOR_GREEN)
#define FACE_COLOR_CYAN   (FACE_COLOR_BLUE | FACE_COLOR_GREEN)
#define FACE_COLOR_PURPLE (FACE_COLOR_BLUE | FACE_COLOR_RED)

#define ENROLL_CONFIRM_TIMES    3
#define FACE_ID_SAVE_NUMBER     10


face_id_list st_face_list = {0};
dl_matrix3du_t *aligned_face = NULL;

static const char *number_suffix(int32_t number)
{
    uint8_t n = number % 10;

    if (n == 0)
        return "zero";
    else if (n == 1)
        return "st";
    else if (n == 2)
        return "nd";
    else if (n == 3)
        return "rd";
    else
        return "th";
}

static void rgb_print(dl_matrix3du_t *image_matrix, uint32_t color, const char *str)
{
    fb_data_t fb;
    fb.width = image_matrix->w;
    fb.height = image_matrix->h;
    fb.data = image_matrix->item;
    fb.bytes_per_pixel = 3;
    fb.format = FB_BGR888;
    fb_gfx_print(&fb, (fb.width - (strlen(str) * 14)) / 2, 10, color, str);
}

static int rgb_printf(dl_matrix3du_t *image_matrix, uint32_t color, const char *format, ...)
{
    char loc_buf[64];
    char *temp = loc_buf;
    int len;
    va_list arg;
    va_list copy;
    va_start(arg, format);
    va_copy(copy, arg);
    len = vsnprintf(loc_buf, sizeof(loc_buf), format, arg);
    va_end(copy);
    if (len >= sizeof(loc_buf)) {
        temp = (char *)malloc(len + 1);
        if (temp == NULL) {
            return 0;
        }
    }
    vsnprintf(temp, len + 1, format, arg);
    va_end(arg);
    rgb_print(image_matrix, color, temp);
    if (len > 64) {
        free(temp);
    }
    return len;
}

static void draw_face_boxes(dl_matrix3du_t *image_matrix, box_array_t *boxes)
{
    int x, y, w, h, i;
    uint32_t color = FACE_COLOR_YELLOW;
    fb_data_t fb;
    fb.width = image_matrix->w;
    fb.height = image_matrix->h;
    fb.data = image_matrix->item;
    fb.bytes_per_pixel = 3;
    fb.format = FB_BGR888;
    for (i = 0; i < boxes->len; i++) {
        // rectangle box
        x = (int)boxes->box[i].box_p[0];
        y = (int)boxes->box[i].box_p[1];
        w = (int)boxes->box[i].box_p[2] - x + 1;
        h = (int)boxes->box[i].box_p[3] - y + 1;
        fb_gfx_drawFastHLine(&fb, x, y, w, color);
        fb_gfx_drawFastHLine(&fb, x, y + h - 1, w, color);
        fb_gfx_drawFastVLine(&fb, x, y, h, color);
        fb_gfx_drawFastVLine(&fb, x + w - 1, y, h, color);
    }
}

#endif



static void facenet_stream(void)
{
    int face_id = -1;
    esp_err_t res = ESP_OK;
    g_state = START_DETECT;
    camera_fb_t *fb = NULL;
    size_t _jpg_buf_len = 0;
    uint8_t *_jpg_buf = NULL;
    dl_matrix3du_t *image_matrix = NULL;


    mtmn_config_t mtmn_config = mtmn_init_config();

    ESP_LOGI(TAG, "Get count %d\n", st_face_list.count);

    while (true) {
        // update fsm state
        if (g_is_enrolling) {
            g_state = START_ENROLL;
        } else if (g_is_deleting) {
            g_is_deleting = 0;
            g_state = START_DELETE;
        } else if (g_state != START_ENROLL) {
            if (st_face_list.count == 0)
                g_state = START_DETECT;
            else
                g_state = START_RECOGNITION;
        }

        ESP_LOGD(TAG, "State: %d, head:%d, tail:%d, count:%d", g_state, st_face_list.head, st_face_list.tail, st_face_list.count);
        // exec event
        if (g_state == START_DELETE) {
            uint8_t left = delete_face_id_in_flash(&st_face_list);
            ESP_LOGW(TAG, "%d ID Left", left);
            g_state = START_DETECT;
            continue;
        }

        // Detection Start
        fb = esp_camera_fb_get();
        if (!fb) {
            ESP_LOGE(TAG, "Camera capture failed");
            res = ESP_FAIL;
            break;
        }

        image_matrix = dl_matrix3du_alloc(1, fb->width, fb->height, 3);
        if (!image_matrix) {
            ESP_LOGE(TAG, "dl_matrix3du_alloc failed");
            res = ESP_FAIL;
            break;
        }

        if (!fmt2rgb888(fb->buf, fb->len, fb->format, image_matrix->item)) {
            ESP_LOGW(TAG, "fmt2rgb888 failed");
        }

        box_array_t *net_boxes = face_detect(image_matrix, &mtmn_config);
        if (net_boxes) {
            ESP_LOGI(TAG, "g_state : %u ", g_state);
            if ((g_state == START_ENROLL || g_state == START_RECOGNITION)
                    && (align_face(net_boxes, image_matrix, aligned_face) == ESP_OK)) {
                if (g_state == START_ENROLL) {
                    rgb_print(image_matrix, FACE_COLOR_YELLOW, "START ENROLLING");
                    ESP_LOGD(TAG, "START ENROLLING");

                    int left_sample_face = enroll_face_id_to_flash(&st_face_list, aligned_face);
                    ESP_LOGD(TAG, "Face ID %d Enrollment: Taken the %d%s sample",
                             st_face_list.tail,
                             ENROLL_CONFIRM_TIMES - left_sample_face,
                             number_suffix(ENROLL_CONFIRM_TIMES - left_sample_face));
                    rgb_printf(image_matrix, FACE_COLOR_CYAN, "\nThe %u%s sample",
                               ENROLL_CONFIRM_TIMES - left_sample_face,
                               number_suffix(ENROLL_CONFIRM_TIMES - left_sample_face));

                    if (left_sample_face == 0) {
                        ESP_LOGI(TAG, "Enrolled Face ID: %d", st_face_list.tail);
                        rgb_printf(image_matrix, FACE_COLOR_CYAN, "\n\nEnrolled Face ID: %d", st_face_list.tail);
                        g_is_enrolling = 0;
                        g_state = START_RECOGNITION;
                    }
                } else {
                    face_id = recognize_face(&st_face_list, aligned_face);

                    if (face_id >= 0) {
                        rgb_printf(image_matrix, FACE_COLOR_GREEN, "Hello ID %u", face_id);
                        ESP_LOGI(TAG, "Hello ID %u", face_id);
                    } else {
                        rgb_print(image_matrix, FACE_COLOR_RED, "\nWHO?");
                        ESP_LOGI(TAG, "Who ? ");
                    }
                }
            }
            draw_face_boxes(image_matrix, net_boxes);
            free(net_boxes->box);
            free(net_boxes->landmark);
            free(net_boxes);

            if (!fmt2jpg(image_matrix->item, fb->width * fb->height * 3, fb->width, fb->height, PIXFORMAT_RGB888, 90, &_jpg_buf, &_jpg_buf_len)) {
                ESP_LOGE(TAG, "fmt2jpg failed");
                dl_matrix3du_free(image_matrix);
                res = ESP_FAIL;
            }
            esp_camera_fb_return(fb);
            fb = NULL;
        } else {
            _jpg_buf = fb->buf;
            _jpg_buf_len = fb->len;
        }
        dl_matrix3du_free(image_matrix);
        TFT_jpg_image(CENTER, CENTER, 0, -1, NULL, _jpg_buf, _jpg_buf_len);

        if (fb) {
            esp_camera_fb_return(fb);
            fb = NULL;
            _jpg_buf = NULL;
        } else if (_jpg_buf) {
            free(_jpg_buf);
            _jpg_buf = NULL;
        }
        if (res != ESP_OK) {
            break;
        }
    }
    g_state = WAIT_FOR_WAKEUP;
}



void test_camera()
{
    while (1) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) {
            ESP_LOGE(TAG, "Camera capture failed");
        } else {
            TFT_jpg_image(CENTER, CENTER, 0, -1, NULL, fb->buf, fb->len);
            // tft->drawBitmapnotswap(0, 0, (const uint16_t *)fb->buf, (int16_t)fb->width, (int16_t)fb->height);
            esp_camera_fb_return(fb);
            fb = NULL;
        }
    }
}

void app_lcd_task(void *pvParameters)
{
    test_camera();
    // facenet_stream();
}

void show_data()
{
    char buff[512];
    struct bme280_data comp_data;

    while (1) {

        int8_t rslt = bme280_get_sensor_data(BME280_ALL, &comp_data, &dev);
        if (rslt != BME280_OK) {
            return;
        }

        if (g_state != WAIT_FOR_WAKEUP) {
            app_sensor_deinit(&dev);
            return;
        }

        tft->fillScreen(0);
        snprintf(buff, sizeof(buff), "Temp    :%0.2f", comp_data.temperature);
        tft->drawString(buff, 0, 30);
        snprintf(buff, sizeof(buff), "Pressure:%0.2f", comp_data.pressure);
        tft->drawString(buff, 0, 60);
        snprintf(buff, sizeof(buff), "Humidity:%0.2f", comp_data.humidity);
        tft->drawString(buff, 0, 90);
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}

extern "C"  void app_main()
{
    char buff[128];
    bool isInitBus;

    ESP_LOGI("esp-eye", "Version "VERSION);

    isInitBus = app_sd_init();

    app_lcd_init(!isInitBus);

    // if (isInitBus) {
    //     FILE stream;
    //     sdmmc_card_t card
    //     sdmmc_card_print_info(NULL, &card);
    //     snprintf(buff, sizeof(buff), "Type:%s", card.cid.name);
    // } else {
    //     tft->drawString("SDCard Mount Fail", 0, 90);
    // }

    app_camera_init();

    app_speech_wakeup_init();

    g_state = WAIT_FOR_WAKEUP;

    app_sensor_init(&dev);

    show_data();

    vTaskDelay(3000 / portTICK_PERIOD_MS);

    tft->drawString("Please say 'Hi LeXin' to the board", 0, 30);

    while (g_state == WAIT_FOR_WAKEUP)
        vTaskDelay(1000 / portTICK_PERIOD_MS);

    app_wifi_init();

    xTaskCreatePinnedToCore(app_lcd_task, "app_lcd_task", 4096, NULL, 4, NULL, 0);

}