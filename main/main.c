#include <stdio.h>
#include "esp_netif.h"

#include <string.h>
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "lwip/err.h"
#include "lwip/sys.h"

#include "esp_vfs.h"
#include "esp_http_server.h"
#include "esp_sntp.h"
#include "time.h"

// ----- INICIO SECCIÓN UTILIDADES -----
// Definición de la macro MIN
#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif

// Función para reemplazar '+' con ' ' en una cadena
void replace_plus_with_space(char *str) {
    for (int i = 0; str[i]; i++) {
        if (str[i] == '+') {
            str[i] = ' ';
        }
    }
}

const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = -10800;  // UTC-3 en segundos
const int   daylightOffset_sec = 0;  // No hay horario de verano

bool synchronized = false;

// ----- FIN SECCIÓN UTILIDADES -----


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
    "function startTime() {"
                     "  fetch('/time')"
                     "  .then(response => response.json())"
                     "  .then(data => {document.getElementById('timeContainer').innerHTML = data.time;"
                     "  setTimeout(startTime, 1000);})" // Actualiza la hora cada segundo
                     "  .catch(error => console.log('Error:', error));"
                     "}"
                     "  function checkTime(i) {"
                     "  if (i < 10) {i = '0' + i};"
                     "  return i;"
                     "}"
    "function submitForm(event, formId, endpoint) {"
            "event.preventDefault();"
            "var formData = new URLSearchParams();"
            "for (const pair of new FormData(document.getElementById(formId))) {"
                "formData.append(pair[0], pair[1]);"
            "}"
            "fetch(endpoint, {"
                "method: 'POST',"
                "headers: { 'Content-Type': 'application/x-www-form-urlencoded' },"
                "body: formData.toString()"
            "})"
            ".then(response => response.text())"
            ".then(data => { document.getElementById(formId + 'Response').innerHTML = data; })"
            ".catch(error => console.error('Error:', error));"
        "}"
    "</script>"

    "</head>"
    "<body onload='startTime()'>"
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
        "<div id=\"timeContainer\"></div>"
    "</div>"
    "</body>"
    "</html>";

// REQUEST HANDLERS
static esp_err_t index_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html_code, strlen(html_code));
    return ESP_OK;
}

esp_err_t time_handler(httpd_req_t *req)
{ 
    if(synchronized == false){
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    char strftime_buf[64];
    time_t now;
    struct tm timeinfo;

    time(&now);
    localtime_r(&now, &timeinfo);
    strftime(strftime_buf, sizeof(strftime_buf), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);

    char* resp_str = (char*)malloc(100);
    if (!resp_str) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    // sprintf(resp_str, "{\"time\": \"%s\"}", strftime_buf);
     snprintf(resp_str, 100, "{\"time\": \"%s\"}", strftime_buf);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp_str, strlen(resp_str));
    // printf("Rquested time: %s", resp_str);

    free(resp_str);
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
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK && strcmp((char *)ap_info.ssid, ssid) == 0) {
        httpd_resp_send(req, "Ya conectado a la red deseada", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    httpd_resp_send(req, ssid, strlen(ssid));  // Enviar SSID como respuesta
    
  // Configurar WiFi STA con los datos recibidos
    if (update_wifi_sta(ssid, password) != ESP_OK) {
        httpd_resp_send_500(req);
        ESP_LOGE("WIFI", "Fallo la configuración de WiFi STA");
        return ESP_FAIL;
    }

    ESP_LOGI("HTTP", "Conexión exitosa: %s", ssid);
    return ESP_OK;
}


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

    httpd_query_key_value(buf, "broker", broker, sizeof(broker));
    httpd_query_key_value(buf, "topic", topic, sizeof(topic));

    // Reemplazar '+' con espacios
    replace_plus_with_space(broker);
    replace_plus_with_space(topic);

    printf("Broker: %s, Topic: %s\n", broker, topic);

    httpd_resp_send(req, "Datos recibidos", HTTPD_RESP_USE_STRLEN);
    
  // Configurar broker con los datos recibidos
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

// ----- FIN SECCIÓN SERVER -----

// ----- INICIO SECCIÓN WIFI -----

// EVENT HANDLERS
static int s_retry_num = 0;
static const int EXAMPLE_ESP_MAXIMUM_RETRY = 5;

void event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
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

        case IP_EVENT_STA_GOT_IP:

            ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
            printf("Got IP: %d.%d.%d.%d\n", IP2STR(&event->ip_info.ip));
            s_retry_num = 0;
            break;

        default:
            break;
    }
}

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
    // esp_wifi_set_storage(WIFI_STORAGE_RAM);


    wifi_config_t wifi_config = {
        .sta = {
            .ssid = "Miguel",
            .password = "32554803",
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
        .ap = {
            .ssid = "MiAP",
            .ssid_len = strlen("MiAP"),
            .channel = 1,
            .password = "password123",
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA2_PSK
        },
    };

    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_start();

    // Registrar el manejador de eventos de WiFi
    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_register(WIFI_EVENT,
                                        ESP_EVENT_ANY_ID,
                                        &event_handler,
                                        NULL,
                                        &instance_any_id);

    printf("WiFi started\n");

}

// ----- FIN SECCIÓN WIFI -----

void start_webserver(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    
    // Iniciar el servidor web
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_register_uri_handler(server, &home);
        httpd_register_uri_handler(server, &redConfig);
        httpd_register_uri_handler(server, &mqttConfig);
        httpd_register_uri_handler(server, &uri_time);
    }
}


void app_main(void)
{
    wifi_init();

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, ntpServer);
    esp_sntp_init();

    setenv("TZ", "<-03>3", 1);
    tzset();

    start_webserver();
    // Create a loop that prints the time every second
     while (true) {
        time_t now;
        struct tm timeinfo = { 0 };
        char strftime_buf[64];

        time(&now);
        localtime_r(&now, &timeinfo);

        if (timeinfo.tm_year < (2024 - 1900)) {
            synchronized = false;
            printf("Time is not set yet. Connecting to WiFi and getting time over NTP.\n");
            vTaskDelay(2000 / portTICK_PERIOD_MS);
            continue;
        } else {
            synchronized = true;
            
        }

        strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);
        printf("The current date/time in Montevideo is: %s\n", strftime_buf);
        vTaskDelay(10000 / portTICK_PERIOD_MS);
    }
 

}
