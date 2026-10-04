#pragma once

#include <stdbool.h>

#include "cJSON.h"
#include "esp_err.h"

// AI event descriptions with a vision LLM. When a motion_start / object_detected event has
// a snapshot, the image is sent to a vision model and the one-sentence answer is attached
// to the event (Events tab, MQTT <base>/description, HA sensor).
//
// APIs:  ollama   POST <url> (e.g. http://host:11434/api/chat)            {"message":{"content"}}
//        openai   POST <url> (e.g. Open WebUI http://host:3000/api/chat/completions,
//                 Ollama http://host:11434/v1/chat/completions)          {"choices":[{"message":{"content"}}]}
esp_err_t llm_mgr_init(void);

// "off", "idle", "busy", "error"
const char *llm_mgr_state(void);
cJSON *llm_mgr_config_json(void);
// Config for a backup; the API key only when secrets is set (spec §27).
cJSON *llm_mgr_config_export(bool secrets);
cJSON *llm_mgr_state_json(void);
esp_err_t llm_mgr_set_config(const cJSON *cfg, char *err, size_t err_len);
// Describes a fresh snapshot in the background; the result appears in the state JSON.
esp_err_t llm_mgr_test(void);
