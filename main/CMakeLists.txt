set(srcs "main.c"
                            "auth_mgr.c"
                            "backup.c"
                            "mqtt_mgr.c"
                            "app_config.c"
                            "camera_mgr.c"
                            "console_cmds.c"
                            "event_mgr.c"
                            "motion_mgr.c"
                            "status_led.c"
                            "sd_mgr.c"
                            "sysmon.c"
                            "web_server.c"
                            "wifi_mgr.c"
)
# Optional modules (Kconfig "ESP32 Camera board" -> optional features)
if(CONFIG_FEATURE_AI)
    list(APPEND srcs "ai_mgr.c")
endif()
if(CONFIG_FEATURE_PERSON)
    list(APPEND srcs "person_mgr.cc" "models/person_detect_model_data.cc")
endif()
if(CONFIG_FEATURE_LLM)
    list(APPEND srcs "llm_mgr.c")
endif()
if(CONFIG_FEATURE_STATS)
    list(APPEND srcs "stats_mgr.c")
endif()

# The web UI is stored gzip-compressed (~101 KB -> ~29 KB of flash) and served with
# Content-Encoding: gzip; tools/gzip_web.py (PlatformIO pre-build script) creates index.html.gz.
idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "."
                       EMBED_FILES "web/index.html.gz")
