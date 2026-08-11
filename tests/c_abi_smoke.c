#include "local_ai.h"

#include <stddef.h>

static void LOCALAI_CALL discard_text(const char * text, size_t byte_count, void * user_data) {
    (void) text;
    (void) byte_count;
    (void) user_data;
}

int main(void) {
    if ((local_ai_get_api_version() >> 16u) != LOCAL_AI_API_VERSION_MAJOR) {
        return 1;
    }

    LocalAIConfig config = {0};
    config.struct_size = (uint32_t) sizeof(config);
    config.gpu_layers = -1;
    config.flags = LOCAL_AI_CONFIG_SEQUENTIAL_MODELS;
    LocalAIEngine * engine = NULL;
    if (local_ai_create(&config, &engine) != LOCAL_AI_OK || engine == NULL) {
        return 2;
    }
    LocalAIMemoryInfo memory = {0};
    memory.struct_size = (uint32_t) sizeof(memory);
    if (local_ai_get_memory_info(engine, &memory) != LOCAL_AI_OK) {
        local_ai_destroy(engine);
        return 3;
    }
    local_ai_cancel(engine);
    local_ai_trim_memory(engine);
    (void) discard_text;
    local_ai_destroy(engine);
    return 0;
}
