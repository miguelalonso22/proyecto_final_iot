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

// Función para extraer el delimitador de la cabecera Content-Type
static const char* get_boundary_from_content_type(const char* content_type) {
    const char* boundary_prefix = "boundary=";
    const char* found = strstr(content_type, boundary_prefix);
    if (found) {
        return found + strlen(boundary_prefix);
    }
    return NULL;
}

// Función para parsear datos multipart/form-data
void parse_multipart_form_data(const char* content, const char* boundary) {
    char delim[128] = "--";
    strcat(delim, boundary);  // Preparar el delimitador completo con '--' prefijado
    strcat(delim, "\r\n");

    const char* start_part = strstr(content, delim);
    while (start_part) {
        start_part += strlen(delim);
        const char* end_part = strstr(start_part, "\r\n--");
        if (!end_part) break;

        // Aquí puedes procesar la parte desde start_part hasta end_part
        // Por simplicidad, solo imprimiremos la parte
        char part_content[1024] = {0};
        strncpy(part_content, start_part, end_part - start_part);
        printf("Part: %s\n", part_content);

        start_part = strstr(end_part, delim);
    }
}

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
    "document.addEventListener('DOMContentLoaded', function() {"
    "    document.getElementById(\"myForm\").addEventListener('submit', function(event) {"
    "        event.preventDefault();"
    "        var formData = new URLSearchParams();"
    "        for (const pair of new FormData(this)) {"
    "            formData.append(pair[0], pair[1]);"
    "        }"
    "        fetch('/echo', {"
    "            method: 'POST',"
    "            headers: { 'Content-Type': 'application/x-www-form-urlencoded' },"
    "            body: formData.toString()"
    "        })"
    "        .then(response => response.text())"
    "        .then(data => { document.getElementById('responseContainer').innerHTML = data; })"
    "        .catch(error => console.error('Error:', error));"
    "    });"
    "});"
    "</script>"

    "</head>"
    "<body>"
    "<div>"
        "<h1>Bienvenido!</h1>" "<h2>Al Laboratorio 2b de Miguel Alonso, Agustina Roballo y Diego Durán </h2>"
            "<form id=\"myForm\">"
            "<b>Configuraciones de Red</b>"
            "<div>"
            "<input type=\"text\" name=\"ssid\" placeholder='SSID' maxlength='100'>" 
            "<input type=\"text\" name=\"password\" placeholder='Contraseña' maxlength='100'>"
            "</div>"
            "<b>Configuraciones MQTT</b>"
            "<div>"
            "<input type=\"text\" name=\"broker\" placeholder='Broker.address.uri' maxlength='100'>"
            "<input type=\"text\" name=\"topic\" placeholder='Topic' maxlength='100'>"
            "</div>"
            "<div>"
            "<textarea name=\"message\" placeholder='Mensaje de prueba...' maxlength='100'></textarea>"
            "</div>"
            "<div>"
                "<input type=\"submit\" value=\"Enviar\">"
            "</div>"
    // Aquí añadí el contenedor para la respuesta
            "<div id='responseContainer'> </div>"        
            "</form>"
    "</div>"
    "</body>"
    "</html>";

// REQUEST HANDLERS
static esp_err_t index_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html_code, strlen(html_code));
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

esp_err_t echo_post_handler(httpd_req_t *req) {
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
    char ssid[100], password[100], broker[100], topic[100], message[100];

    httpd_query_key_value(buf, "ssid", ssid, sizeof(ssid));
    httpd_query_key_value(buf, "password", password, sizeof(password));
    httpd_query_key_value(buf, "broker", broker, sizeof(broker));
    httpd_query_key_value(buf, "topic", topic, sizeof(topic));
    httpd_query_key_value(buf, "message", message, sizeof(message));

    // Reemplazar '+' con espacios
    replace_plus_with_space(ssid);
    replace_plus_with_space(password);
    replace_plus_with_space(broker);
    replace_plus_with_space(topic);
    replace_plus_with_space(message);

    printf("Red: %s, Password: %s, Broker: %s, Topic: %s, Mensaje: %s\n", ssid, password, broker, topic, message);

    httpd_resp_send(req, "Datos recibidos", HTTPD_RESP_USE_STRLEN);
    
  // Configurar WiFi STA con los datos recibidos
    if (update_wifi_sta(ssid, password) != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    return ESP_OK;
}

// ENDPOINTS
httpd_uri_t home = {
    .uri       = "/",
    .method    = HTTP_GET,
    .handler   = index_get_handler,
    .user_ctx  = NULL
};

httpd_uri_t echo = {
    .uri       = "/echo",
    .method    = HTTP_POST,
    .handler   = echo_post_handler,
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
                vTaskDelay(pdMS_TO_TICKS(2000)); // Esperar 2 segundos antes de reconectar
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
            printf("Got IP: %d.%d.%d.%d\n",
                   IP2STR(&event->ip_info.ip));

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
        httpd_register_uri_handler(server, &echo);
    }
}

void app_main(void)
{
    wifi_init();
    start_webserver();
}
