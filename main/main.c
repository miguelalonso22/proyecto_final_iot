#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>

#include "esp_netif.h"

#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"

#include "lwip/err.h"
#include "lwip/sys.h"

#include "esp_vfs.h"
#include "esp_http_server.h"
#include "esp_sntp.h"
#include "time.h"

#include "mqtt_client.h"

#include "esp_heap_caps.h"
#include "esp_spiffs.h"
#include "freertos/event_groups.h"
#include "i2c_bus.h"
#include "driver/rmt.h"

#include "led_strip.h"
#include "touch.h"
#include "audio.h"
#include "es8311.h"
#include "board.h"

// ----- PROTOTIPOS DE FUNCIONES -----

// Wifi
void wifi_init(void);
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);
static esp_err_t update_wifi_sta(const char* ssid, const char* pass);

// NTP
void set_ntp(void);
static time_t get_time(void);
void synchronize_time(void *pvParameters);


// Server
// esp_err_t index_get_handler(httpd_req_t *req);
// esp_err_t red_post_handler(httpd_req_t *req);
// esp_err_t mqtt_post_handler(httpd_req_t *req);
// esp_err_t time_handler(httpd_req_t *req);
void start_webserver(void);

// Utilidades
void replace_plus_with_space(char *str);

// MQTT
static void mqtt_app_start(void);
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data);

// ----- FIN PROTOTIPOS DE FUNCIONES -----


// ----- INICIO SECCIÓN UTILIDADES -----
// Definición de la macro MIN
#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif

#define LOG_CAPACITY 20

// Estructura de las instrucciones
typedef enum {
    PLAY,
    STOP,
    NEXT,
    PREV
} CommandType;

// Estructura de las entradas del logger
typedef struct {
    CommandType command;
    time_t timestamp;
} LogEntry;

// Estructura del logger
typedef struct {
    LogEntry entries[LOG_CAPACITY];  // Almacenará la instrucción "next"-"prev"-"play"-"stop" y la hora
    int head;
    int tail;
    int count;
} Logger;

Logger logger ;
QueueHandle_t instructionQueue;

void init_logger() {
    logger.head = 0;
    logger.tail = 0;
    logger.count = 0;
}

// Función para formatear la hora (time_t a string)
// El tiempo se almacena como un valor en segundos desde el Unix Epoch (1 de enero de 1970).
static char* format_time(time_t timestamp, char* buffer, size_t buffer_size) {
    struct tm timeinfo;
    localtime_r(&timestamp, &timeinfo);
    strftime(buffer, buffer_size, "%c", &timeinfo);
    return buffer;
}

// Función para convertir el CommandType a string para impresión
const char* commandToString(CommandType command) {
    switch (command) {
        case PLAY:
            return "PLAY";
        case STOP:
            return "STOP";
        case NEXT:
            return "NEXT";
        case PREV:
            return "PREV";
        default:
            return "UNKNOWN";
    }
}

// Función para imprimir el contenido del logger
void print_logger(Logger *logger) {
    printf("Logger Entries:\n");
    if (logger->count == 0) {
        printf("No entries to display.\n");
        return;
    }

int index = logger->head;
    for (int i = 0; i < logger->count; i++) {
        char time_str[64];
        format_time(logger->entries[index].timestamp, time_str, sizeof(time_str));
        printf("Entry %d: %s at %s\n", i + 1, commandToString(logger->entries[index].command), time_str);
        index = (index + 1) % LOG_CAPACITY;
    }
}
// Función para agregar una instrucción al logger
void log_instruction(CommandType instruction, time_t timestamp) {
    if (logger.count < LOG_CAPACITY) {
        logger.entries[logger.tail].timestamp = timestamp;
        logger.entries[logger.tail].command = instruction;
        logger.tail = (logger.tail + 1) % LOG_CAPACITY;
        logger.count++;
    } else {
        // Overwrite the oldest entry
        logger.entries[logger.tail].timestamp = timestamp;
        logger.entries[logger.tail].command = instruction;
        logger.tail = (logger.tail + 1) % LOG_CAPACITY;
        logger.head = (logger.head + 1) % LOG_CAPACITY;
    }
}


// enum {
//     AUDIO_STOP = 0,
//     AUDIO_PLAY,
//     AUDIO_NEXT,
//     AUDIO_LAST
// };

// Tarea que lee y ejecuta instrucciones de la queue
void instructionTask(void *pvParameters) {
    CommandType comando;
    while (1) {
        if (xQueueReceive(instructionQueue, &comando, portMAX_DELAY) == pdPASS) {
            switch (comando) {
                case PLAY:
                    printf("Ejecutando instrucción: PLAY\n");
                    log_instruction(PLAY, get_time());
                    play_flag = AUDIO_PLAY;
                    break;
                case STOP:
                    printf("Ejecutando instrucción: STOP\n");
                    log_instruction(STOP, get_time());
                    play_flag = AUDIO_STOP;

                    // logInstruction(&logger, "STOP");
                    break;
                case NEXT:
                    printf("Ejecutando instrucción: NEXT\n");
                    log_instruction(NEXT, get_time());
            print_logger(&logger);
                    play_flag = AUDIO_NEXT;
                    // logInstruction(&logger, "NEXT");
                    break;
                case PREV:
                    printf("Ejecutando instrucción: PREV\n");
                    log_instruction(PREV, get_time());
                    play_flag = AUDIO_LAST;
                    // logInstruction(&logger, "PREV");
                    break;
                default:
                    printf("Instrucción desconocida\n");
                    break;
            }
        }
    }
}

static const char *TAG = "mqtt";

static void log_error_if_nonzero(const char *message, int error_code)
{
    if (error_code != 0) {
        ESP_LOGE(TAG, "Last error %s: 0x%x", message, error_code);
    }
}

// Variables globales para almacenar la configuración del broker
static char *brokerUri = "";
static char *topico = "";

// Función para reemplazar '+' con ' ' en una cadena
void replace_plus_with_space(char *str) {
    for (int i = 0; str[i]; i++) {
        if (str[i] == '+') {
            str[i] = ' ';
        }
    }
}

// Configuración de NTP
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = -10800;  // UTC-3 en segundos
const int   daylightOffset_sec = 0;  // No hay horario de verano

// Variable para almacenar la hora formateada
static char strftime_buf[64];

bool synchronized = false;

// ----- FIN SECCIÓN UTILIDADES -----

// ----- INICIO SECCIÓN AUDIO -----

// static const char *TAG = "audio";

uint8_t mac[16];
led_strip_t *strip;


esp_err_t touch_audio_rmt_init(uint8_t gpio_num, int led_number, uint8_t rmt_channel)
{
    ESP_LOGI(TAG, "Initializing WS2812");
    rmt_config_t config = RMT_DEFAULT_CONFIG_TX(gpio_num, rmt_channel);

    /*!< set counter clock to 40MHz */
    config.clk_div = 2;

    ESP_ERROR_CHECK(rmt_config(&config));
    ESP_ERROR_CHECK(rmt_driver_install(config.channel, 0, 0));

    led_strip_config_t strip_config = LED_STRIP_DEFAULT_CONFIG(led_number, (led_strip_dev_t)config.channel);
    strip = led_strip_new_rmt_ws2812(&strip_config);

    if (!strip) {
        ESP_LOGE(TAG, "install WS2812 driver failed");
        return ESP_FAIL;
    }

    /*!< Clear LED strip (turn off all LEDs) */
    ESP_ERROR_CHECK(strip->clear(strip, 100));
    /*!< Show simple rainbow chasing pattern */

    return ESP_OK;
}

esp_err_t spiffs_init(void)
{
    esp_err_t ret = ESP_OK;
    ESP_LOGI(TAG, "Initializing SPIFFS");

    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = NULL,
        .max_files = 5,
        .format_if_mount_failed = true
    };

    /*!< Use settings defined above to initialize and mount SPIFFS filesystem. */
    /*!< Note: esp_vfs_spiffs_register is an all-in-one convenience function. */
    ret = esp_vfs_spiffs_register(&conf);

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount or format filesystem");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed to find SPIFFS partition");
        } else {
            ESP_LOGE(TAG, "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
        }

        return ret;
    }

    size_t total = 0, used = 0;
    ret = esp_spiffs_info(NULL, &total, &used);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get SPIFFS partition information (%s)", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Partition size: total: %d, used: %d", total, used);
    }

    /*!< Open renamed file for reading */
    ESP_LOGI(TAG, "Reading file");
    FILE *f = fopen("/spiffs/spiffs.txt", "r");

    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open file for reading");
        return ESP_FAIL;
    }

    char line[64];
    fgets(line, sizeof(line), f);
    fclose(f);
    /*!< strip newline */
    char *pos = strchr(line, '\n');

    if (pos) {
        *pos = '\0';
    }

    ESP_LOGI(TAG, "Read from file: '%s'", line);

    return ESP_OK;
}
// ----- FIN SECCIÓN AUDIO -----

// ----- INICIO SECCIÓN SERVER -----

// Código HTML
 static const char* html_code = 
    "<!DOCTYPE html>"
    "<html>"
    "<head>"
    "<meta charset=\"UTF-8\">"
    "<title>Proyecto Final IOT</title>"
    "<style>"
    "body { font-family: Arial; display: flex; justify-content: center; align-items: center; height: 100vh; }"
    "</style>"
    "<script>"
    "let lastTime = \"\";" // Variable para almacenar la última hora recibida del servidor
    "function fetchTime() {"
    "    fetch('/time')"
    "    .then(response => response.json())"
    "    .then(data => {"
    "        lastTime = data.time;"
    "        document.getElementById('timeContainer').innerHTML = lastTime;"
    "        updateTime();"
    "    })"
    "    .catch(error => console.log('Error:', error));"
    "}"
    "function updateTime() {"
    "    if (lastTime) {"
    "        let currentTime = new Date(lastTime);"
    "        currentTime.setSeconds(currentTime.getSeconds() + 1);"
    "        lastTime = currentTime.toString();"
    "        document.getElementById('timeContainer').innerHTML = currentTime.toLocaleString();"
    "        setTimeout(updateTime, 1000);"
    "    }"
    "}"
    "function submitForm(event, formId, endpoint) {"
    "    event.preventDefault();"
    "    var formData = new URLSearchParams();"
    "    for (const pair of new FormData(document.getElementById(formId))) {"
    "        formData.append(pair[0], pair[1]);"
    "    }"
    "    fetch(endpoint, {"
    "        method: 'POST',"
    "        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },"
    "        body: formData.toString()"
    "    })"
    "    .then(response => response.text())"
    "    .then(data => { document.getElementById(formId + 'Response').innerHTML = data; })"
    // ".then(response => window.location.reload())"
    "    .catch(error => console.error('Error:', error));"
    "}"
    "function playSong() {"
    "    fetch('/playSong')"
    "    .then(response => response.text())"
    "    .then(data => { console.log(data); })"
    "    .catch(error => console.error('Error:', error));"
    "}"
    "function stopSong() {"
    "    fetch('/stopSong')"
    "    .then(response => response.text())"
    "    .then(data => { console.log(data); })"
    "    .catch(error => console.error('Error:', error));"
    "}"
    "function nextSong() {"
    "    fetch('/nextSong')"
    "    .then(response => response.text())"
    "    .then(data => { console.log(data); })"
    "    .catch(error => console.error('Error:', error));"
    "}"
    "function prevSong() {"
    "    fetch('/prevSong')"
    "    .then(response => response.text())"
    "    .then(data => { console.log(data); })"
    "    .catch(error => console.error('Error:', error));"
    "}"

    "</script>"

    "</head>"
    "<body>"
    "<div>"
        "<h1>Bienvenido!</h1>" "<h2>Al Proyecto Final IOT de Miguel Alonso, Agustina Roballo y Diego Durán </h2>"
        "<h3>Configuraciones de Red</h3>"
        "<form id=\"formRed\" onsubmit=\"submitForm(event, 'formRed', '/redConfig')\">"
            "SSID: <input type=\"text\" name=\"ssid\" placeholder='SSID' maxlength='100'>" 
            "Password: <input type=\"text\" name=\"password\" placeholder='Contraseña' maxlength='100'>"
            "<div>"
                "<input type=\"submit\" value=\"Actualizar Red\">"
            "</div>"
        "</form>"
        "<div id=\"formRedResponse\"></div>"
        "<h3>Configuraciones MQTT</h3>"
        "<form id=\"formMQTT\" onsubmit=\"submitForm(event, 'formMQTT', '/mqttConfig')\">"
            "<input type=\"text\" name=\"broker\" placeholder='Broker.address.uri' maxlength='100'>"
            "<input type=\"text\" name=\"topic\" placeholder='Topic' maxlength='100'>"
            "<div>"
                "<input type=\"submit\" value=\"Actualizar Broker\">"
            "</div>"
        "</form>"
        "<div id=\"formMQTTResponse\"></div>"
        "<h3>Hora Actual</h3>"
        "<button onclick=\"fetchTime()\">Sincronizar</button>"
        "<div id=\"timeContainer\"></div>"
        "<h3>Reproductor de Música</h3>"
        "<div>"
        "<button onClick=prevSong()>Anterior</button>"
        "<button onClick=playSong()>Reproducir</button>"
        "<button onClick=stopSong()>Stop</button>"
        "<button onClick=nextSong()>Siguiente</button>"
        // "<button onClick=playPause()>Play/Pause</button>"
        "</div>"

    "</div>"
    "</body>"
    "</html>";

// ----- HANDLERS -----

// REQUEST HANDLERS
static esp_err_t index_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html_code, strlen(html_code));
    return ESP_OK;
}

esp_err_t time_handler(httpd_req_t *req)
{ 
    time_t timestamp = get_time();
    if (timestamp == (time_t)0) {
        printf("Time is not set yet\n");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    // Pasar la hora de time_t a string
    char time_str[64];
    format_time(timestamp, time_str, sizeof(time_str));

    char resp_str[100];
    snprintf(resp_str, sizeof(resp_str), "{\"time\": \"%s\"}", time_str);
 
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp_str, strlen(resp_str));
    printf("Rquested time: %s", resp_str);

    return ESP_OK;
}

// Función para configurar el STA con los datos recibidos
static esp_err_t update_wifi_sta(const char* ssid, const char* pass) {
    if (!ssid || !pass) {
        ESP_LOGE("WIFI", "SSID or Password is NULL");
        return ESP_FAIL;
    }

    // Desconectar y detener WiFi antes de reconfigurar
    esp_wifi_disconnect();
    esp_wifi_stop();
    vTaskDelay(pdMS_TO_TICKS(100)); // Pequeña pausa para asegurar que el WiFi se detiene completamente

    wifi_config_t sta_config = {};
    strncpy((char*)sta_config.sta.ssid, ssid, sizeof(sta_config.sta.ssid));
    strncpy((char*)sta_config.sta.password, pass, sizeof(sta_config.sta.password));
    sta_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_LOGI("WIFI", "Configurando STA SSID: %s, PASS: %s", ssid, pass);

    // Iniciar WiFi y aplicar la nueva configuración
    esp_err_t result = esp_wifi_start();
    if (result != ESP_OK) {
        ESP_LOGE("WIFI", "Failed to start WiFi: %s", esp_err_to_name(result));
        return result;
    }

    result = esp_wifi_set_config(ESP_IF_WIFI_STA, &sta_config);
    if (result != ESP_OK) {
        ESP_LOGE("WIFI", "Failed to set STA config: %s", esp_err_to_name(result));
        return result;
    }

    result = esp_wifi_connect();
    if (result != ESP_OK) {
        ESP_LOGE("WIFI", "Failed to connect: %s", esp_err_to_name(result));
        return result;
    }

    return ESP_OK;
}

// Función para manejar el POST de la configuración de red
esp_err_t red_post_handler(httpd_req_t *req) {
    char buf[256] = {0};
    int ret, remaining = req->content_len;

    if (remaining > 0) {
        printf("Esperando recibir %d bytes\n", remaining);
    } else {
        printf("No hay datos para recibir.\n");
    }

    while (remaining > 0) {
        ret = httpd_req_recv(req, buf, MIN(remaining, sizeof(buf) - 1));
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            httpd_resp_send_500(req);
            printf("Error recibiendo datos: %d\n", ret);
            return ESP_FAIL;
        }
        buf[ret] = '\0';
        remaining -= ret;
    }
    printf("Datos recibidos (crudos): %s\n", buf);

    // Extraer y decodificar cada campo
    char ssid[100], password[100];

    httpd_query_key_value(buf, "ssid", ssid, sizeof(ssid));
    httpd_query_key_value(buf, "password", password, sizeof(password));

    // Reemplazar '+' con espacios
    replace_plus_with_space(ssid);
    replace_plus_with_space(password);

    printf("Red: %s, Password: %s\n", ssid, password);

    // Verificar conexión actual
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        if (strcmp((char *)ap_info.ssid, ssid) == 0) {
            ESP_LOGI("HTTP", "Ya conectado a la red deseada");
            httpd_resp_send(req, "Ya conectado a la red deseada", HTTPD_RESP_USE_STRLEN);
            return ESP_OK;
        }
    } else {
        ESP_LOGW("WIFI", "No se pudo obtener información del AP actual, intentando reconectar...");
    }

    httpd_resp_send(req, ssid, strlen(ssid));  // Enviar SSID como respuesta
    
  // Configurar WiFi STA con los datos recibidos
    if (update_wifi_sta(ssid, password) != ESP_OK) {
        httpd_resp_send_500(req);
        ESP_LOGE("WIFI", "Fallo la configuración de WiFi STA");
        return ESP_FAIL;
    }

    return ESP_OK;
}

// Función para manejar el POST de la configuración de MQTT
esp_err_t mqtt_post_handler(httpd_req_t *req) {
    char buf[256] = {0};
    int ret, remaining = req->content_len;

    if (remaining > 0) {
        printf("Esperando recibir %d bytes\n", remaining);
    } else {
        printf("No hay datos para recibir.\n");
    }

    while (remaining > 0) {
        ret = httpd_req_recv(req, buf, MIN(remaining, sizeof(buf) - 1));
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            httpd_resp_send_500(req);
            printf("Error recibiendo datos: %d\n", ret);
            return ESP_FAIL;
        }
        buf[ret] = '\0';
        remaining -= ret;
    }
    printf("Datos recibidos (crudos): %s\n", buf);

    // Extraer y decodificar cada campo
    char broker[100], topic[100];
    char mqttURL[150] = "mqtt://broker."; // "mqtt://broker.hivemq.com"
    httpd_query_key_value(buf, "broker", broker, sizeof(broker));
    httpd_query_key_value(buf, "topic", topic, sizeof(topic));

    // Reemplazar '+' con espacios
    replace_plus_with_space(broker);
    replace_plus_with_space(topic);

    strcat(mqttURL, broker);

    printf("Broker: %s, Topic: %s\n", mqttURL, topic);

    httpd_resp_send(req, "Datos recibidos", HTTPD_RESP_USE_STRLEN);
    
  // Configurar broker con los datos recibidos
    brokerUri = mqttURL;
    topico = topic;
    mqtt_app_start();
    return ESP_OK;
}

esp_err_t playSong_handler(httpd_req_t *req) {
    // play_song();
    CommandType command = PLAY;
    if (xQueueSend(instructionQueue, &command, portMAX_DELAY) != pdPASS) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_send(req, "Reproduciendo canción", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

esp_err_t stopSong_handler(httpd_req_t *req) {
    // stop_song(); 
    CommandType command = STOP;
    if (xQueueSend(instructionQueue, &command, portMAX_DELAY) != pdPASS) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_send(req, "Canción detenida", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

esp_err_t nextSong_handler(httpd_req_t *req) {
    // next_song();
    CommandType command = NEXT;
    if (xQueueSend(instructionQueue, &command, portMAX_DELAY) != pdPASS) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_send(req, "Siguiente canción", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

esp_err_t prevSong_handler(httpd_req_t *req) {
    // prev_song();
    CommandType command = PREV;
    if (xQueueSend(instructionQueue, &command, portMAX_DELAY) != pdPASS) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_send(req, "Canción anterior", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// ENDPOINTS
httpd_uri_t home = {
    .uri       = "/",
    .method    = HTTP_GET,
    .handler   = index_get_handler,
    .user_ctx  = NULL
};

httpd_uri_t redConfig = {
    .uri       = "/redConfig",
    .method    = HTTP_POST,
    .handler   = red_post_handler,
    .user_ctx  = NULL
};

httpd_uri_t mqttConfig = {
    .uri       = "/mqttConfig",
    .method    = HTTP_POST,
    .handler   = mqtt_post_handler,
    .user_ctx  = NULL
};

httpd_uri_t uri_time = {
    .uri       = "/time",
    .method    = HTTP_GET,
    .handler   = time_handler,
    .user_ctx  = NULL
};

httpd_uri_t playSong = {
    .uri       = "/playSong",
    .method    = HTTP_GET,
    .handler   = playSong_handler,
    .user_ctx  = NULL
};

httpd_uri_t stopSong = {
    .uri       = "/stopSong",
    .method    = HTTP_GET,
    .handler   = stopSong_handler,
    .user_ctx  = NULL
};

httpd_uri_t nextSong = {
    .uri       = "/nextSong",
    .method    = HTTP_GET,
    .handler   = nextSong_handler,
    .user_ctx  = NULL
};

httpd_uri_t prevSong = {
    .uri       = "/prevSong",
    .method    = HTTP_GET,
    .handler   = prevSong_handler,
    .user_ctx  = NULL
};

// ----- FIN SECCIÓN SERVER -----

// ----- INICIO SECCIÓN WIFI -----

// Variables globales para el manejo de reintentos de conexión
static int s_retry_num = 0;
static const int EXAMPLE_ESP_MAXIMUM_RETRY = 5;

// Manejador de eventos de WiFi
void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    switch(event_id) {

        case WIFI_EVENT_STA_START:
            esp_wifi_connect(); // Intentar conectar tras iniciar el WiFi
            printf("Trying to connect...\n");
            break;

        case WIFI_EVENT_STA_DISCONNECTED:
            if(s_retry_num < EXAMPLE_ESP_MAXIMUM_RETRY) {
                vTaskDelay(pdMS_TO_TICKS(2000)); 
                esp_wifi_connect(); // Reintentar conectar automáticamente
                printf("Disconnected. Trying to reconnect...\n");
                s_retry_num++;
            }else {
                printf("Connection failed. Maximum retries reached.\n");
                s_retry_num = 0; 
            }
            break;

        case IP_EVENT_STA_GOT_IP:{
            ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
            printf("Got IP: %d.%d.%d.%d\n", IP2STR(&event->ip_info.ip));
            printf("Conexión exitosa:");
            s_retry_num = 0;

            if (xTaskCreate(&synchronize_time, "synchronize_time", 4096, NULL, 5, NULL) != pdPASS) {
                printf("Failed to create task for time synchronization\n");
            }
            break;
        }
            

        default:
            break;
    }
}

// Inicializar la interfaz de red y el stack de WiFi
void wifi_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }

    // Inicializar la interfaz de red
    esp_netif_init();
    // Crear el loop de eventos por defecto
    esp_event_loop_create_default();
    // Crear WiFi AP por defecto
    esp_netif_create_default_wifi_ap();
    // Crear Wifi STA por defecto
    esp_netif_create_default_wifi_sta();

    // Inicializar el stack de WiFi
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    // Configuración específica para STA
    wifi_config_t wifi_sta_config = {
        .sta = {
            .ssid = "iPhone del Diego",
            .password = "diegoddl",
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        }
    };

    // Configuración específica para AP
    wifi_config_t wifi_ap_config = {
        .ap = {
            .ssid = "MiAP",
            .ssid_len = strlen("MiAP"),
            .channel = 1,
            .password = "password123",
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA2_PSK
        }
    };

    // Registrar el manejador de eventos de WiFi
    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_register(WIFI_EVENT,
                                        ESP_EVENT_ANY_ID,
                                        &wifi_event_handler,
                                        NULL,
                                        &instance_any_id);
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));

    // Configurar el modo APSTA
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_sta_config));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_ap_config));
    esp_wifi_start();

    printf("WiFi started\n");
}
// ----- FIN SECCIÓN WIFI -----

// ----- INICIO SECCIÓN MQTT -----
static void mqtt_app_start(void)
{
    esp_mqtt_client_config_t mqtt_cfg = {
        .uri = brokerUri,
    };
#if CONFIG_BROKER_URL_FROM_STDIN
    char line[128];

    if (strcmp(mqtt_cfg.broker.address.uri, "FROM_STDIN") == 0) {
        int count = 0;
        printf("Please enter url of mqtt broker\n");
        while (count < 128) {
            int c = fgetc(stdin);
            if (c == '\n') {
                line[count] = '\0';
                break;
            } else if (c > 0 && c < 127) {
                line[count] = c;
                ++count;
            }
            vTaskDelay(10 / portTICK_PERIOD_MS);
        }
        mqtt_cfg.broker.address.uri = line;
        printf("Broker url: %s\n", line);
    } else {
        ESP_LOGE(TAG, "Configuration mismatch: wrong broker url");
        abort();
    }
#endif /* CONFIG_BROKER_URL_FROM_STDIN */

    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    /* The last argument may be used to pass data to the event handler, in this example mqtt_event_handler */
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(client);
}

// Manejador de eventos MQTT
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%" PRIi32 "", base, event_id);
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;
    int msg_id;
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");

        msg_id = esp_mqtt_client_subscribe(client, topico, 0);
        ESP_LOGI(TAG, "sent subscribe successful, msg_id=%d", msg_id);

        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
        break;

    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
        msg_id = esp_mqtt_client_publish(client, topico, "data", 0, 0, 0);
        ESP_LOGI(TAG, "sent publish successful, msg_id=%d", msg_id);
        break;
    case MQTT_EVENT_UNSUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "MQTT_EVENT_DATA");
        printf("TOPIC=%.*s\r\n", event->topic_len, event->topic);
        printf("DATA=%.*s\r\n", event->data_len, event->data);
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            log_error_if_nonzero("reported from esp-tls", event->error_handle->esp_tls_last_esp_err);
            log_error_if_nonzero("reported from tls stack", event->error_handle->esp_tls_stack_err);
            log_error_if_nonzero("captured as transport's socket errno",  event->error_handle->esp_transport_sock_errno);
            ESP_LOGI(TAG, "Last errno string (%s)", strerror(event->error_handle->esp_transport_sock_errno));
        }
        break;
    default:
        ESP_LOGI(TAG, "Other event id:%d", event->event_id);
        break;
    }
}

// ----- FIN SECCIÓN MQTT -----

// ----- INICIO SECCIÓN NTP -----

// Función para configurar el NTP
void set_ntp(void){
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, ntpServer);
    esp_sntp_init();

    setenv("TZ", "<-03>3", 1);
    tzset();
}

// Función para obtener la hora actual
static time_t get_time(void) {
    time_t now;
    struct tm timeinfo = { 0 };

    time(&now);// Obtener el tiempo actual en segundos desde el epoch (1 de enero de 1970)
    localtime_r(&now, &timeinfo);// Formatear y almacenar el tiempo en struct timeinfo

    // Verificar si el año es menor a 2024
    if (timeinfo.tm_year < (2024 - 1900)) { // Funciona midiendo la cantidad de años desde 1990 
        synchronized = false;
        printf("Time is not set yet. Waiting for system time to be set...\n");
        vTaskDelay(2000 / portTICK_PERIOD_MS);
        return (time_t)0; // Retorna 0 si la hora no está sincronizada
    } else {
        synchronized = true;
        
    }
    // printf("The current date/time in Montevideo is: %s\n", asctime(&timeinfo));
    return now;
}

// Tarea para sincronizar la hora
void synchronize_time(void *pvParameters) {
    // Llamamos a get_time hasta obtener una hora correcta --> Sinconización
    while (!synchronized) {
        get_time();
        vTaskDelay(2000 / portTICK_PERIOD_MS);
    }
    printf("Synchronized Time\n");
    vTaskDelete(NULL);
}

// ----- FIN SECCIÓN NTP -----

// ----- INICIO SECCIÓN LOGGER -----
 // Función para guardar el logger en NVS
void save_logger_to_nvs() {
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &my_handle);
    if (err != ESP_OK) {
        printf("Error opening NVS handle: %s\n", esp_err_to_name(err));
        return;
    }

    err = nvs_set_blob(my_handle, "logger", &logger, sizeof(logger));
    if (err != ESP_OK) {
        printf("Error saving logger to NVS: %s\n", esp_err_to_name(err));
    }

    nvs_commit(my_handle);
    nvs_close(my_handle);
}

 // Función para cargar el logger desde NVS
void load_logger_from_nvs() {
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open("storage", NVS_READONLY, &my_handle);
    if (err != ESP_OK) {
        printf("Error opening NVS handle: %s\n", esp_err_to_name(err));
        return;
    }

    size_t required_size = sizeof(logger);
    err = nvs_get_blob(my_handle, "logger", &logger, &required_size);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        printf("Logger not found in NVS, initializing new logger\n");
        init_logger();
    } else if (err != ESP_OK) {
        printf("Error loading logger from NVS: %s\n", esp_err_to_name(err));
    }

    nvs_close(my_handle);
}


// ----- FIN SECCIÓN LOGGER -----

// ----- INICIO SECCIÓN SPIFFS -----

// ----- FIN SECCIÓN SPIFFS -----

// ----- INICIO SECCIÓN MAIN -----
void start_webserver(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    
    // Iniciar el servidor web
    if (httpd_start(&server, &config) == ESP_OK) { // Si el servidor se inició correctamente, registrar los manejadores de URI
        httpd_register_uri_handler(server, &home);
        httpd_register_uri_handler(server, &redConfig);
        httpd_register_uri_handler(server, &mqttConfig);
        httpd_register_uri_handler(server, &uri_time);
        httpd_register_uri_handler(server, &playSong);
        httpd_register_uri_handler(server, &stopSong);
        httpd_register_uri_handler(server, &nextSong);
        httpd_register_uri_handler(server, &prevSong);
    }
}

void app_main(void)
{
    wifi_init();
    set_ntp();
    start_webserver();

   // Inicializar la cola
    instructionQueue = xQueueCreate(10, sizeof(CommandType));
    if (instructionQueue == NULL) {
        printf("Error al crear la cola\n");
        return;
    }

   // Cargar el logger desde NVS
    load_logger_from_nvs();


    // Crear la tarea de instrucciones
    xTaskCreate(&instructionTask, "instructionTask", 2048, NULL, 5, NULL);

    spiffs_init();
    i2c_bus_init();
    touch_audio_rmt_init(CONFIG_EXAMPLE_RMT_TX_GPIO, CONFIG_EXAMPLE_STRIP_LED_NUMBER, RMT_CHANNEL_0);

    /*!< Initialize touch */
    touch_init();
    /*!< Initialize audio */
    audio_init(strip);

}
