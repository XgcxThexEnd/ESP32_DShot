#pragma once
#define IDF_VER "test"
typedef struct { char project_name[32], version[32], idf_ver[32]; } esp_app_desc_t;
const esp_app_desc_t *esp_app_get_description(void);
