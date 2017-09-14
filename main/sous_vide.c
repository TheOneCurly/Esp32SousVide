/* CoAP server Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <string.h>
#include <sys/socket.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event_loop.h"

#include "nvs_flash.h"

#include "coap.h"

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "ds18b20/ds18b20.h"

/* The examples use simple WiFi configuration that you can set via
   'make menuconfig'.

   If you'd rather not, just change the below entries to strings with
   the config you want - ie #define EXAMPLE_WIFI_SSID "mywifissid"
*/
#define EXAMPLE_WIFI_SSID            CONFIG_WIFI_SSID
#define EXAMPLE_WIFI_PASS            CONFIG_WIFI_PASSWORD

#define COAP_DEFAULT_TIME_SEC 5
#define COAP_DEFAULT_TIME_USEC 0

#define RELAY_PIN 5
#define TEMP_PIN 14

#define LED_PIN_R 4 
#define LED_PIN_G 16
#define LED_PIN_B 17

static const int16_t setpoint = 250;//540;

static const int16_t P = 40;
static const int16_t I = 10;
static const int16_t D = 0;

// Global static for COAP
static int16_t currentTemperature = 0;
static bool relayClosedStatus = false;

static EventGroupHandle_t wifi_event_group;

/* The event group allows multiple bits for each event,
   but we only care about one event - are we connected
   to the AP with an IP? */
const static int CONNECTED_BIT = BIT0;

const static char *TAG = "CoAP_server";

static coap_async_state_t *async = NULL;

// Global static for LED
static bool LEDRed = false;
static bool LEDGreen = true;
static bool LEDBlue = false;
static bool LEDBlink = true;

static bool LEDBlinkStatus = false;

const static int16_t LEDLowDiff = -10;
const static int16_t LEDHighDiff = 10;

static void send_async_response(coap_context_t *ctx, const coap_endpoint_t *local_if){
    coap_pdu_t *response;
    unsigned char buf[3];
    size_t size = sizeof(coap_hdr_t) + 20;
    response = coap_pdu_init(async->flags & COAP_MESSAGE_CON, COAP_RESPONSE_CODE(205), 0, size);
    response->hdr->id = coap_new_message_id(ctx);
    if (async->tokenlen)
        coap_add_token(response, async->tokenlen, async->token);
    coap_add_option(response, COAP_OPTION_CONTENT_TYPE, coap_encode_var_bytes(buf, COAP_MEDIATYPE_TEXT_PLAIN), buf);
    coap_add_data  (response, strlen(async->appdata), (unsigned char *)async->appdata);
    
    if (coap_send(ctx, local_if, &async->peer, response) == COAP_INVALID_TID) {

    }
    coap_delete_pdu(response);
    coap_async_state_t *tmp;
    coap_remove_async(ctx, async->id, &tmp);
    coap_free_async(async);
    async = NULL;
}

/*
 * The resource handler
 */
static void
sousvide_handler(coap_context_t *ctx, struct coap_resource_t *resource,
              const coap_endpoint_t *local_interface, coap_address_t *peer,
              coap_pdu_t *request, str *token, coap_pdu_t *response){
    async = coap_register_async(ctx, peer, request, COAP_ASYNC_SEPARATE | COAP_ASYNC_CONFIRM, (void*)"Online");
}

static void temp_handler(coap_context_t *ctx, struct coap_resource_t *resource,
              const coap_endpoint_t *local_interface, coap_address_t *peer,
              coap_pdu_t *request, str *token, coap_pdu_t *response){
    char* tempData = (char*)malloc(6 * sizeof(char)); // max lenth int16 is -32768
    sprintf(tempData, "%06d", currentTemperature);
    async = coap_register_async(ctx, peer, request, COAP_ASYNC_SEPARATE | COAP_ASYNC_CONFIRM | COAP_ASYNC_RELEASE_DATA, (void*)tempData);
}

static void relay_handler(coap_context_t *ctx, struct coap_resource_t *resource,
              const coap_endpoint_t *local_interface, coap_address_t *peer,
              coap_pdu_t *request, str *token, coap_pdu_t *response){
    char* relayData = (char*)malloc(1 * sizeof(char));
    sprintf(relayData, "%u", relayClosedStatus);
    async = coap_register_async(ctx, peer, request, COAP_ASYNC_SEPARATE | COAP_ASYNC_CONFIRM | COAP_ASYNC_RELEASE_DATA, (void*)relayData);
}

static void coap_thread(void *p){
    coap_context_t*  ctx = NULL;
    coap_address_t   serv_addr;
    coap_resource_t* resource = NULL;
    fd_set           readfds;
    struct timeval tv;
    int flags = 0;

    while (1) {
        /* Wait for the callback to set the CONNECTED_BIT in the
           event group.
        */
        xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT,
                            false, true, portMAX_DELAY);
        ESP_LOGI(TAG, "Connected to AP");

        /* Prepare the CoAP server socket */
        coap_address_init(&serv_addr);
        serv_addr.addr.sin.sin_family      = AF_INET;
        serv_addr.addr.sin.sin_addr.s_addr = INADDR_ANY;
        serv_addr.addr.sin.sin_port        = htons(COAP_DEFAULT_PORT);
        ctx                                = coap_new_context(&serv_addr);
        if (ctx) {
            flags = fcntl(ctx->sockfd, F_GETFL, 0);
            fcntl(ctx->sockfd, F_SETFL, flags|O_NONBLOCK);

            tv.tv_usec = COAP_DEFAULT_TIME_USEC;
            tv.tv_sec = COAP_DEFAULT_TIME_SEC;
            /* Initialize the resource */
            resource = coap_resource_init((unsigned char *)"SousVide", 8, 0);
            if (resource){
                coap_register_handler(resource, COAP_REQUEST_GET, sousvide_handler);
                coap_add_resource(ctx, resource);

                // Add temperature resource
                resource = coap_resource_init((unsigned char *)"SousVide/Temp", 13, 0);
                if(resource){
                    coap_register_handler(resource, COAP_REQUEST_GET, temp_handler);
                    coap_add_resource(ctx, resource);

                    // Add relay resource
                    resource = coap_resource_init((unsigned char *)"SousVide/Relay", 14, 0);
                    if(resource){
                        coap_register_handler(resource, COAP_REQUEST_GET, relay_handler);
                        coap_add_resource(ctx, resource);
                        
                        /*For incoming connections*/
                        for (;;) {
                            FD_ZERO(&readfds);
                            FD_CLR( ctx->sockfd, &readfds);
                            FD_SET( ctx->sockfd, &readfds);

                            int result = select( FD_SETSIZE, &readfds, 0, 0, &tv );
                            if (result > 0){
                                if (FD_ISSET( ctx->sockfd, &readfds ))
                                    coap_read(ctx);
                            } else if (result < 0){
                                break;
                            } else {
                                ESP_LOGE(TAG, "select timeout");
                            }   

                            if (async) {
                                send_async_response(ctx, ctx->endpoint);
                            }
                        }
                    }
                }
            }

            coap_free_context(ctx);
        }
    }

    vTaskDelete(NULL);
}

static esp_err_t wifi_event_handler(void *ctx, system_event_t *event){
    switch(event->event_id) {
    case SYSTEM_EVENT_STA_START:
        esp_wifi_connect();
        break;
    case SYSTEM_EVENT_STA_GOT_IP:
        xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);
        break;
    case SYSTEM_EVENT_STA_DISCONNECTED:
        /* This is a workaround as ESP32 WiFi libs don't currently
           auto-reassociate. */
        esp_wifi_connect();
        xEventGroupClearBits(wifi_event_group, CONNECTED_BIT);
        break;
    default:
        break;
    }
    return ESP_OK;
}

static void wifi_conn_init(void){
    tcpip_adapter_init();
    wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK( esp_event_loop_init(wifi_event_handler, NULL) );
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK( esp_wifi_init(&cfg) );
    ESP_ERROR_CHECK( esp_wifi_set_storage(WIFI_STORAGE_RAM) );
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = EXAMPLE_WIFI_SSID,
            .password = EXAMPLE_WIFI_PASS,
        },
    };
    ESP_ERROR_CHECK( esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK( esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );
    ESP_ERROR_CHECK( esp_wifi_start() );
}

static void setRelay(bool closed){
    gpio_set_level(RELAY_PIN, closed);
    relayClosedStatus = closed;
}

static int getTemperature(){
    int temp = DS_get_temp() * 10;
    
    // Freezing point of water
    if(temp < 0){
        temp = 0;
    }
    
    // Boiling point of water
    if(temp > 1000){
        temp = 1000;
    }

    return temp;
}

static void controlTask(void* pvParameters){
    // Close relay
    setRelay(false);

    int16_t currentError = 0;
    
    int16_t lastError = 0;
    int16_t errorIntegral = 0;
    int16_t errorDerivative = 0;

    int16_t PIDResult;
    
    while(true){
        currentTemperature =  getTemperature();
        printf("Current Temperature: %d\n", currentTemperature);

        currentError = setpoint - currentTemperature;
    
        errorIntegral += currentError;
        if(errorIntegral > 50){
            errorIntegral = 51;
        }else if(errorIntegral < -50){
            errorIntegral = -50;
        }

        errorDerivative = currentError - lastError;
        
        PIDResult = (P*currentError) + (I*errorIntegral) + (D*errorDerivative);

        if(PIDResult > 500){
            PIDResult = 500;
        }else if(PIDResult < -500){
            PIDResult = -500;
        }
        
        printf("Calculated PID: %d\n", PIDResult);

        // Set relay
        if(PIDResult > 0){
            setRelay(true);
        }else{
            setRelay(false);
        }

        // Set status LED
        if(currentError < LEDLowDiff){
            LEDRed = true;
            LEDGreen = false;
            LEDBlue = false;
            LEDBlink = false;
        }else if(currentError > LEDHighDiff){
            LEDRed = false;
            LEDGreen = false;
            LEDBlue = true;
            LEDBlink = false;
        }else{
            LEDRed = false;
            LEDGreen = true;
            LEDBlue = false;
            LEDBlink = true;
        }
     
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }

}

static void ledTask(void* pvParameters){
    while(true){

        if(LEDBlink && LEDBlinkStatus){
            gpio_set_level(LED_PIN_R, 0);
            gpio_set_level(LED_PIN_G, 0);
            gpio_set_level(LED_PIN_B, 0);          
        }else{
            gpio_set_level(LED_PIN_R, LEDRed);
            gpio_set_level(LED_PIN_G, LEDGreen);
            gpio_set_level(LED_PIN_B, LEDBlue);
        }

        LEDBlinkStatus = !LEDBlinkStatus;


        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}

void app_main(void){
    ESP_ERROR_CHECK( nvs_flash_init() );
    wifi_conn_init();

    xTaskCreate(coap_thread, "coap", 2048, NULL, 5, NULL);

    // Set relay to output gpio
    gpio_pad_select_gpio(RELAY_PIN);
    gpio_set_direction(RELAY_PIN, GPIO_MODE_OUTPUT);

    // Set LED pins to output
    gpio_pad_select_gpio(LED_PIN_R);
    gpio_set_direction(LED_PIN_R, GPIO_MODE_OUTPUT);

    gpio_pad_select_gpio(LED_PIN_G);
    gpio_set_direction(LED_PIN_G, GPIO_MODE_OUTPUT);

    gpio_pad_select_gpio(LED_PIN_B);
    gpio_set_direction(LED_PIN_B, GPIO_MODE_OUTPUT);

    // Set onewire library
    DS_init(TEMP_PIN);

    xTaskCreate(controlTask, "controlTask", 2048, NULL, 5, NULL);

    xTaskCreate(ledTask, "ledTask", 2048, NULL, 5, NULL);
}
