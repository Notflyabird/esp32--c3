idf_component_register(
    SRCS
        "main.c"
        "audio_input.c"
        "scorekeeper.c"
        "speech_recognition.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES driver esp_driver_i2s
)
