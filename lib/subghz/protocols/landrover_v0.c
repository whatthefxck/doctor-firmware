#include <lib/subghz/blocks/custom_btn_i.h>
#include <lib/subghz/blocks/generic.h>
#include <lib/subghz/registry.h>
#include "landrover_v0.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"

static size_t
    landrover_v0_emit_merge(LevelDuration* up, size_t i, size_t cap, bool level, uint32_t us) {
    if(i > 0 && level_duration_get_level(up[i - 1]) == level) {
        uint32_t prev = level_duration_get_duration(up[i - 1]);
        up[i - 1] = level_duration_make(level, prev + us);
        return i;
    }
    if(i < cap) up[i++] = level_duration_make(level, us);
    return i;
}

static SubGhzProtocolStatus
    landrover_v0_verify_protocol_name(FlipperFormat* ff, const char* expected_name) {
    if(!ff || !expected_name) return SubGhzProtocolStatusError;
    FuriString* tmp = furi_string_alloc();
    if(!tmp) return SubGhzProtocolStatusError;
    SubGhzProtocolStatus result = SubGhzProtocolStatusError;
    flipper_format_rewind(ff);
    if(!flipper_format_read_string(ff, "Protocol", tmp))
        result = SubGhzProtocolStatusErrorParserOthers;
    else if(subghz_protocol_names_match(furi_string_get_cstr(tmp), expected_name))
        result = SubGhzProtocolStatusOk;
    furi_string_free(tmp);
    return result;
}

static void landrover_v0_encoder_read_fields(
    FlipperFormat* ff,
    uint32_t* serial_out,
    uint32_t* btn_out,
    uint32_t* cnt_out) {
    if(!ff) return;
    uint32_t tmp = 0;
    if(serial_out) {
        flipper_format_rewind(ff);
        if(flipper_format_read_uint32(ff, "Serial", &tmp, 1)) *serial_out = tmp;
    }
    if(btn_out) {
        flipper_format_rewind(ff);
        if(flipper_format_read_uint32(ff, "Btn", &tmp, 1)) *btn_out = tmp;
    }
    if(cnt_out) {
        flipper_format_rewind(ff);
        if(flipper_format_read_uint32(ff, "Cnt", &tmp, 1)) *cnt_out = tmp;
    }
}

static uint32_t landrover_v0_encoder_read_repeat(FlipperFormat* ff, uint32_t default_repeat) {
    if(!ff) return default_repeat;
    flipper_format_rewind(ff);
    uint32_t tmp = 0;
    return flipper_format_read_uint32(ff, "Repeat", &tmp, 1) ? tmp : default_repeat;
}

static void landrover_v0_get_string_setup_editor(uint8_t btn, uint32_t cnt) {
    subghz_block_generic_global.btn_is_available = true;
    subghz_block_generic_global.cnt_is_available = true;
    subghz_block_generic_global.btn_length_bit = 8;
    subghz_block_generic_global.cnt_length_bit = 16;
    subghz_block_generic_global.current_btn = btn;
    subghz_block_generic_global.current_cnt = cnt;
    if(!subghz_custom_btn_has_original()) subghz_custom_btn_set_original(btn);
    subghz_custom_btn_set_max(3);
}
#pragma GCC diagnostic pop

#define TAG "LandRoverV0"

static const SubGhzBlockConst subghz_protocol_landrover_v0_const = {
    .te_short = 250,
    .te_long = 500,
    .te_delta = 100,
    .min_count_bit_for_found = 72,
};

#define LANDROVER_V0_PREAMBLE_PAIRS  4
#define LANDROVER_V0_GAP_US          3500
#define LANDROVER_V0_TOTAL_BURSTS    4
#define LANDROVER_V0_BITS            72
#define LANDROVER_V0_UPLOAD_CAPACITY (((LANDROVER_V0_TOTAL_BURSTS - 1U) * 185U) + 184U)

static const uint8_t landrover_v0_auth_matrix[56] = {
    0x15, 0x4B, 0xFA, 0x68, 0x33, 0xF8, 0x12, 0x30, 0xFB, 0x80, 0xFB, 0x68, 0x5A, 0x1E,
    0x12, 0x4B, 0x33, 0xF8, 0x12, 0x30, 0x9B, 0xB2, 0xFA, 0x88, 0xD3, 0x1A, 0xFB, 0x80,
    0xFB, 0x88, 0xBA, 0x68, 0x13, 0x44, 0xBB, 0x60, 0xFB, 0x68, 0x01, 0x33, 0xFB, 0x60,
    0x0C, 0x4B, 0x1B, 0x68, 0xFA, 0x68, 0x9A, 0x42, 0xE5, 0xDB, 0x0A, 0x48, 0x05, 0xF0,
};

typedef struct SubGhzProtocolDecoderLandRoverV0 {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;

    ManchesterState manchester_state;

    uint64_t data_low;
    uint64_t data_high;
    uint8_t bit_count;

    uint16_t header_count;

    uint64_t key1;
    uint8_t auth;
    uint32_t serial;
    uint8_t button;
    uint32_t count;
    uint8_t checksum;
} SubGhzProtocolDecoderLandRoverV0;

static uint8_t landrover_other_get_btn_code(void) {
    switch(subghz_custom_btn_get()) {
    case SUBGHZ_CUSTOM_BTN_UP:
        return 0x10;
    case SUBGHZ_CUSTOM_BTN_OK:
        return 0x20;
    case SUBGHZ_CUSTOM_BTN_DOWN:
        return 0x40;
    default:
        return subghz_custom_btn_get_original();
    }
}

typedef struct SubGhzProtocolEncoderLandRoverV0 {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;

    uint64_t key1;
    uint8_t auth;
    uint32_t serial;
    uint8_t button;
    uint32_t count;
    uint8_t checksum;
} SubGhzProtocolEncoderLandRoverV0;

typedef enum {
    LandRoverV0DecoderStepReset = 0,
    LandRoverV0DecoderStepPreamble,
    LandRoverV0DecoderStepPreambleCheck,
    LandRoverV0DecoderStepGap,
    LandRoverV0DecoderStepData,
} LandRoverV0DecoderStep;

static void landrover_v0_add_bit(SubGhzProtocolDecoderLandRoverV0* instance, bool bit);
static bool landrover_v0_process_data(SubGhzProtocolDecoderLandRoverV0* instance);

void subghz_protocol_encoder_landrover_v0_free(void* context);
void subghz_protocol_encoder_landrover_v0_stop(void* context);
LevelDuration subghz_protocol_encoder_landrover_v0_yield(void* context);
uint8_t subghz_protocol_decoder_landrover_v0_get_hash_data(void* context);

const SubGhzProtocolDecoder subghz_protocol_landrover_v0_decoder = {
    .alloc = subghz_protocol_decoder_landrover_v0_alloc,
    .free = subghz_protocol_decoder_landrover_v0_free,
    .feed = subghz_protocol_decoder_landrover_v0_feed,
    .reset = subghz_protocol_decoder_landrover_v0_reset,
    .get_hash_data = subghz_protocol_decoder_landrover_v0_get_hash_data,
    .serialize = subghz_protocol_decoder_landrover_v0_serialize,
    .deserialize = subghz_protocol_decoder_landrover_v0_deserialize,
    .get_string = subghz_protocol_decoder_landrover_v0_get_string,
};

const SubGhzProtocolEncoder subghz_protocol_landrover_v0_encoder = {
    .alloc = subghz_protocol_encoder_landrover_v0_alloc,
    .free = subghz_protocol_encoder_landrover_v0_free,
    .deserialize = subghz_protocol_encoder_landrover_v0_deserialize,
    .stop = subghz_protocol_encoder_landrover_v0_stop,
    .yield = subghz_protocol_encoder_landrover_v0_yield,
};

const SubGhzProtocol subghz_protocol_landrover_v0 = {
    .name = SUBGHZ_PROTOCOL_LANDROVER_V0_NAME,
    .type = SubGhzProtocolTypeDynamic,
    .flag = SubGhzProtocolFlag_315 | SubGhzProtocolFlag_433 | SubGhzProtocolFlag_AM |
            SubGhzProtocolFlag_Decodable | SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save |
            SubGhzProtocolFlag_Send,
    .decoder = &subghz_protocol_landrover_v0_decoder,
    .encoder = &subghz_protocol_landrover_v0_encoder,
};

static uint8_t landrover_v0_calculate_checksum(uint32_t serial, uint32_t count, uint8_t button) {
    uint32_t s = ((serial >> 24) & 0xFF) + ((serial >> 16) & 0xFF) + ((serial >> 8) & 0xFF) +
                 (serial & 0xFF) + (uint32_t)button + ((count >> 8) & 0xFF) + (count & 0xFF);
    return (uint8_t)(s & 0xFF);
}

static uint8_t landrover_v0_auth(const uint8_t b8[8]) {
    uint8_t res = 0, mask = 1;
    for(int row = 0; row < 8; row++) {
        uint8_t acc;
        if(row < 7) {
            acc = 0;
            for(int j = 0; j < 8; j++)
                acc ^= (uint8_t)(b8[j] & landrover_v0_auth_matrix[row * 8 + j]);
        } else {
            acc = res;
        }
        if(subghz_protocol_blocks_parity8(acc)) res = (uint8_t)(res + mask);
        mask = (uint8_t)(mask << 1);
    }
    return res;
}

static void decode_landrover_v0(
    uint64_t key1,
    uint32_t* serial,
    uint8_t* button,
    uint32_t* count,
    uint8_t* checksum) {
    uint8_t b[8];
    for(int i = 0; i < 8; i++) b[i] = (uint8_t)(key1 >> (56 - i * 8));

    uint8_t parity = subghz_protocol_blocks_parity8(b[7]);
    if(parity) {
        uint8_t k = b[6];
        for(int i = 0; i <= 5; i++) b[i] ^= k;
    } else {
        uint8_t k = b[5];
        for(int i = 0; i <= 4; i++) b[i] ^= k;
        b[6] ^= k;
    }
    uint8_t t = (uint8_t)((b[5] & 0xAA) | (b[6] & 0x55));
    b[6] = (uint8_t)((b[5] & 0x55) | (b[6] & 0xAA));
    b[5] = t;

    *serial = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
    *button = b[4];
    *count = ((uint32_t)b[5] << 8) | b[6];
    if(checksum) *checksum = b[7];
}

static void encode_landrover_v0(
    uint32_t serial,
    uint8_t button,
    uint32_t count,
    uint64_t* key1,
    uint8_t* auth) {
    if(!key1) return;
    uint8_t b[8];
    b[0] = (serial >> 24) & 0xFF;
    b[1] = (serial >> 16) & 0xFF;
    b[2] = (serial >> 8) & 0xFF;
    b[3] = serial & 0xFF;
    b[4] = button;
    b[5] = (count >> 8) & 0xFF;
    b[6] = count & 0xFF;
    b[7] = landrover_v0_calculate_checksum(serial, count, button);

    uint8_t t = (uint8_t)((b[5] & 0xAA) | (b[6] & 0x55));
    b[6] = (uint8_t)((b[5] & 0x55) | (b[6] & 0xAA));
    b[5] = t;
    uint8_t parity = subghz_protocol_blocks_parity8(b[7]);
    if(parity) {
        uint8_t k = b[6];
        for(int i = 0; i <= 5; i++) b[i] ^= k;
    } else {
        uint8_t k = b[5];
        for(int i = 0; i <= 4; i++) b[i] ^= k;
        b[6] ^= k;
    }

    uint64_t v = 0;
    for(int i = 0; i < 8; i++) v = (v << 8) | b[i];
    *key1 = v;
    if(auth) *auth = landrover_v0_auth(b);
}

void* subghz_protocol_encoder_landrover_v0_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolEncoderLandRoverV0* instance =
        calloc(1, sizeof(SubGhzProtocolEncoderLandRoverV0));
    instance->base.protocol = &subghz_protocol_landrover_v0;
    instance->generic.protocol_name = instance->base.protocol->name;

    instance->encoder.repeat = 3;
    instance->encoder.upload = malloc(LANDROVER_V0_UPLOAD_CAPACITY * sizeof(LevelDuration));
    furi_check(instance->encoder.upload);
    instance->encoder.size_upload = LANDROVER_V0_UPLOAD_CAPACITY;
    instance->encoder.is_running = false;
    instance->encoder.front = 0;
    return instance;
}

static void
    subghz_protocol_encoder_landrover_v0_get_upload(SubGhzProtocolEncoderLandRoverV0* instance) {
    furi_check(instance);
    size_t index = 0;

    uint32_t te_short = subghz_protocol_landrover_v0_const.te_short;
    uint32_t te_long = subghz_protocol_landrover_v0_const.te_long;

    uint8_t bytes[9];
    for(int i = 0; i < 8; i++) bytes[i] = (uint8_t)(instance->key1 >> (56 - i * 8));
    bytes[8] = instance->auth;

#define ADD_LEVEL(lvl, dur) \
    index = landrover_v0_emit_merge(      \
        instance->encoder.upload, index, LANDROVER_V0_UPLOAD_CAPACITY, (lvl), (dur))

    for(uint8_t burst = 0; burst < LANDROVER_V0_TOTAL_BURSTS; burst++) {
        ADD_LEVEL(true, te_short);
        ADD_LEVEL(false, te_long);
        for(int i = 0; i < LANDROVER_V0_PREAMBLE_PAIRS; i++) {
            ADD_LEVEL(true, te_long);
            ADD_LEVEL(false, te_long);
        }
        ADD_LEVEL(true, te_short);
        ADD_LEVEL(false, LANDROVER_V0_GAP_US);

        for(int by = 0; by < 9; by++) {
            for(int bit = 7; bit >= 0; bit--) {
                if((bytes[by] >> bit) & 1) {
                    ADD_LEVEL(false, te_short);
                    ADD_LEVEL(true, te_short);
                } else {
                    ADD_LEVEL(true, te_short);
                    ADD_LEVEL(false, te_short);
                }
            }
        }

        if(burst < LANDROVER_V0_TOTAL_BURSTS - 1) ADD_LEVEL(false, te_long * 100);
    }
#undef ADD_LEVEL

    instance->encoder.size_upload = index;
    instance->encoder.front = 0;
}

void subghz_protocol_encoder_landrover_v0_free(void* context) {
    furi_check(context);
    SubGhzProtocolEncoderLandRoverV0* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

void subghz_protocol_encoder_landrover_v0_stop(void* context) {
    furi_check(context);
    SubGhzProtocolEncoderLandRoverV0* instance = context;
    instance->encoder.is_running = false;
    instance->encoder.front = 0;
}

LevelDuration subghz_protocol_encoder_landrover_v0_yield(void* context) {
    furi_check(context);
    SubGhzProtocolEncoderLandRoverV0* instance = context;
    if(instance->encoder.repeat == 0 || !instance->encoder.is_running ||
       instance->encoder.size_upload == 0) {
        instance->encoder.is_running = false;
        return level_duration_reset();
    }
    LevelDuration ret = instance->encoder.upload[instance->encoder.front];
    if(++instance->encoder.front == instance->encoder.size_upload) {
        if(!subghz_block_generic_global.endless_tx) instance->encoder.repeat--;
        instance->encoder.front = 0;
    }
    return ret;
}

SubGhzProtocolStatus subghz_protocol_encoder_landrover_v0_deserialize(
    void* context,
    FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolEncoderLandRoverV0* instance = context;
    SubGhzProtocolStatus ret = SubGhzProtocolStatusError;

    instance->encoder.is_running = false;
    instance->encoder.front = 0;

    do {
        if(landrover_v0_verify_protocol_name(flipper_format, instance->base.protocol->name) !=
           SubGhzProtocolStatusOk)
            break;
        ret = subghz_block_generic_deserialize(&instance->generic, flipper_format);
        if(ret != SubGhzProtocolStatusOk) break;

        uint32_t serial = instance->generic.serial;
        uint32_t btn = instance->generic.btn;
        uint32_t cnt = instance->generic.cnt;
        landrover_v0_encoder_read_fields(flipper_format, &serial, &btn, &cnt);

        if(!subghz_custom_btn_has_original()) subghz_custom_btn_set_original((uint8_t)btn);
        btn = landrover_other_get_btn_code();
        subghz_block_generic_global_button_override_get((uint8_t*)&btn);
        if(!subghz_block_generic_global_counter_override_get(&cnt))
            cnt += furi_hal_subghz_get_rolling_counter_mult();

        instance->serial = serial;
        instance->button = (uint8_t)btn;
        instance->count = cnt;
        instance->checksum =
            landrover_v0_calculate_checksum(instance->serial, instance->count, instance->button);

        encode_landrover_v0(
            instance->serial, instance->button, instance->count, &instance->key1, &instance->auth);

        instance->generic.data = instance->key1;
        instance->generic.data_count_bit = LANDROVER_V0_BITS;
        instance->generic.serial = instance->serial;
        instance->generic.btn = instance->button;
        instance->generic.cnt = instance->count;

        instance->encoder.repeat = landrover_v0_encoder_read_repeat(flipper_format, 3);
        subghz_protocol_encoder_landrover_v0_get_upload(instance);
        if(instance->encoder.size_upload == 0) break;

        flipper_format_rewind(flipper_format);
        uint32_t tmp = instance->auth;
        flipper_format_insert_or_update_uint32(flipper_format, "Auth", &tmp, 1);
        tmp = instance->checksum;
        flipper_format_insert_or_update_uint32(flipper_format, "Checksum", &tmp, 1);
        uint32_t serial_w = instance->serial;
        flipper_format_insert_or_update_uint32(flipper_format, "Serial", &serial_w, 1);
        uint32_t btn_w = instance->button;
        flipper_format_insert_or_update_uint32(flipper_format, "Btn", &btn_w, 1);
        uint32_t cnt_w = instance->count;
        flipper_format_insert_or_update_uint32(flipper_format, "Cnt", &cnt_w, 1);
        subghz_block_generic_global.current_cnt = instance->count;

        instance->encoder.is_running = true;
        ret = SubGhzProtocolStatusOk;
    } while(false);

    return ret;
}

static void landrover_v0_add_bit(SubGhzProtocolDecoderLandRoverV0* instance, bool bit) {
    uint32_t low = (uint32_t)instance->data_low;
    instance->data_low = (instance->data_low << 1) | (bit ? 1 : 0);
    instance->data_high = (instance->data_high << 1) | ((low >> 31) & 1);
    instance->bit_count++;
}

static bool landrover_v0_process_data(SubGhzProtocolDecoderLandRoverV0* instance) {
    if(instance->bit_count == 64) {
        instance->key1 = ((uint64_t)instance->data_high << 32) | (uint32_t)instance->data_low;
        instance->data_low = 0;
        instance->data_high = 0;
        return false;
    }
    if(instance->bit_count == LANDROVER_V0_BITS) {
        instance->auth = (uint8_t)(instance->data_low & 0xFF);
        decode_landrover_v0(
            instance->key1,
            &instance->serial,
            &instance->button,
            &instance->count,
            &instance->checksum);
        return true;
    }
    return false;
}

void* subghz_protocol_decoder_landrover_v0_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderLandRoverV0* instance = malloc(sizeof(SubGhzProtocolDecoderLandRoverV0));
    instance->base.protocol = &subghz_protocol_landrover_v0;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void subghz_protocol_decoder_landrover_v0_free(void* context) {
    furi_check(context);
    free(context);
}

uint8_t subghz_protocol_decoder_landrover_v0_get_hash_data(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderLandRoverV0* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8U) + 1U);
}

void subghz_protocol_decoder_landrover_v0_reset(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderLandRoverV0* instance = context;
    instance->decoder.parser_step = LandRoverV0DecoderStepReset;
    instance->decoder.te_last = 0;
    instance->manchester_state = ManchesterStateMid1;
    instance->data_low = 0;
    instance->data_high = 0;
    instance->bit_count = 0;
    instance->header_count = 0;
    instance->key1 = 0;
    instance->auth = 0;
    instance->serial = 0;
    instance->button = 0;
    instance->count = 0;
    instance->checksum = 0;
}

void subghz_protocol_decoder_landrover_v0_feed(void* context, bool level, uint32_t duration) {
    furi_check(context);
    SubGhzProtocolDecoderLandRoverV0* instance = context;

    uint32_t te_short = subghz_protocol_landrover_v0_const.te_short;
    uint32_t te_long = subghz_protocol_landrover_v0_const.te_long;
    uint32_t te_delta = subghz_protocol_landrover_v0_const.te_delta;
    uint32_t gap_threshold = LANDROVER_V0_GAP_US;

    switch(instance->decoder.parser_step) {
    case LandRoverV0DecoderStepReset:
        if(level && (DURATION_DIFF(duration, te_short) < te_delta)) {
            instance->data_low = 0;
            instance->data_high = 0;
            instance->decoder.parser_step = LandRoverV0DecoderStepPreamble;
            instance->decoder.te_last = duration;
            instance->header_count = 0;
            instance->bit_count = 0;
            manchester_advance(
                instance->manchester_state,
                ManchesterEventReset,
                &instance->manchester_state,
                NULL);
        }
        break;

    case LandRoverV0DecoderStepPreamble:
        if(!level) {
            if(DURATION_DIFF(duration, te_long) < te_delta) {
                instance->decoder.te_last = duration;
                instance->decoder.parser_step = LandRoverV0DecoderStepPreambleCheck;
            } else {
                instance->decoder.parser_step = LandRoverV0DecoderStepReset;
            }
        }
        break;

    case LandRoverV0DecoderStepPreambleCheck:
        if(level) {
            if(DURATION_DIFF(duration, te_long) < te_delta) {
                instance->header_count++;
                instance->decoder.te_last = duration;
                instance->decoder.parser_step = LandRoverV0DecoderStepPreamble;
            } else if(DURATION_DIFF(duration, te_short) < te_delta) {
                instance->decoder.parser_step = LandRoverV0DecoderStepGap;
            } else {
                instance->decoder.parser_step = LandRoverV0DecoderStepReset;
            }
        }
        break;

    case LandRoverV0DecoderStepGap:
        if(!level && (DURATION_DIFF(duration, gap_threshold) < 500)) {
            instance->data_low = 0;
            instance->data_high = 0;
            instance->bit_count = 0;
            instance->manchester_state = ManchesterStateMid1;
            instance->decoder.parser_step = LandRoverV0DecoderStepData;
        } else if(!level && duration > gap_threshold + 500) {
            instance->decoder.parser_step = LandRoverV0DecoderStepReset;
        }
        break;

    case LandRoverV0DecoderStepData: {
        ManchesterEvent event = ManchesterEventReset;
        if(DURATION_DIFF(duration, te_short) < te_delta)
            event = level ? ManchesterEventShortLow : ManchesterEventShortHigh;
        else if(DURATION_DIFF(duration, te_long) < te_delta)
            event = level ? ManchesterEventLongLow : ManchesterEventLongHigh;

        if(event == ManchesterEventReset) {
            instance->decoder.parser_step = LandRoverV0DecoderStepReset;
            break;
        }

        bool data_bit;
        if(manchester_advance(
               instance->manchester_state, event, &instance->manchester_state, &data_bit)) {
            landrover_v0_add_bit(instance, data_bit);

            if(landrover_v0_process_data(instance)) {
                instance->generic.data = instance->key1;
                instance->generic.data_count_bit = LANDROVER_V0_BITS;
                instance->generic.serial = instance->serial;
                instance->generic.btn = instance->button;
                instance->generic.cnt = instance->count;

                if(instance->base.callback)
                    instance->base.callback(&instance->base, instance->base.context);

                instance->data_low = 0;
                instance->data_high = 0;
                instance->bit_count = 0;
                instance->decoder.parser_step = LandRoverV0DecoderStepReset;
            }
        }
        instance->decoder.te_last = duration;
        break;
    }
    }
}

SubGhzProtocolStatus subghz_protocol_decoder_landrover_v0_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_check(context);
    SubGhzProtocolDecoderLandRoverV0* instance = context;

    SubGhzProtocolStatus ret =
        subghz_block_generic_serialize(&instance->generic, flipper_format, preset);
    if(ret == SubGhzProtocolStatusOk) {
        uint32_t tmp = instance->auth;
        flipper_format_write_uint32(flipper_format, "Auth", &tmp, 1);
        tmp = instance->checksum;
        flipper_format_write_uint32(flipper_format, "Checksum", &tmp, 1);
        uint32_t serial_w = instance->serial;
        flipper_format_rewind(flipper_format);
        flipper_format_insert_or_update_uint32(flipper_format, "Serial", &serial_w, 1);
        uint32_t btn_w = instance->button;
        flipper_format_rewind(flipper_format);
        flipper_format_insert_or_update_uint32(flipper_format, "Btn", &btn_w, 1);
        uint32_t cnt_w = instance->count;
        flipper_format_rewind(flipper_format);
        flipper_format_insert_or_update_uint32(flipper_format, "Cnt", &cnt_w, 1);
    }
    return ret;
}

SubGhzProtocolStatus subghz_protocol_decoder_landrover_v0_deserialize(
    void* context,
    FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolDecoderLandRoverV0* instance = context;

    SubGhzProtocolStatus ret = subghz_block_generic_deserialize_check_count_bit(
        &instance->generic,
        flipper_format,
        subghz_protocol_landrover_v0_const.min_count_bit_for_found);

    if(ret == SubGhzProtocolStatusOk) {
        instance->key1 = instance->generic.data;
        uint32_t tmp = 0;
        flipper_format_rewind(flipper_format);
        if(flipper_format_read_uint32(flipper_format, "Auth", &tmp, 1)) instance->auth = tmp & 0xFF;
        flipper_format_rewind(flipper_format);
        if(flipper_format_read_uint32(flipper_format, "Checksum", &tmp, 1))
            instance->checksum = tmp & 0xFF;

        uint32_t serial = instance->serial;
        uint32_t btn = instance->button;
        uint32_t cnt = instance->count;
        landrover_v0_encoder_read_fields(flipper_format, &serial, &btn, &cnt);
        instance->serial = serial;
        instance->button = (uint8_t)btn;
        instance->count = cnt;
        instance->generic.serial = instance->serial;
        instance->generic.btn = instance->button;
        instance->generic.cnt = instance->count;
    }
    return ret;
}

void subghz_protocol_decoder_landrover_v0_get_string(void* context, FuriString* output) {
    furi_check(context);
    SubGhzProtocolDecoderLandRoverV0* instance = context;
    landrover_v0_get_string_setup_editor(instance->button, instance->count);

    uint8_t onair[8];
    for(int i = 0; i < 8; i++) onair[i] = (uint8_t)(instance->key1 >> (56 - i * 8));
    bool auth_ok = (landrover_v0_auth(onair) == instance->auth);
    bool csum_ok =
        (landrover_v0_calculate_checksum(instance->serial, instance->count, instance->button) ==
         instance->checksum);

    const char* button_name = "??";
    if(instance->button == 0x10)
        button_name = "Lock";
    else if(instance->button == 0x20)
        button_name = "Unlock";
    else if(instance->button == 0x40)
        button_name = "Boot";

    furi_string_cat_printf(
        output, "%s %ubit\r\n", instance->generic.protocol_name, instance->generic.data_count_bit);
    furi_string_cat_printf(
        output,
        "Key:%08lX%08lX A:%02X\r\n",
        (unsigned long)(instance->key1 >> 32),
        (unsigned long)(instance->key1 & 0xFFFFFFFF),
        instance->auth);
    furi_string_cat_printf(
        output,
        "Sn:%08lX Btn:%02X [%s]\r\n",
        (unsigned long)instance->serial,
        instance->button,
        button_name);
    furi_string_cat_printf(
        output,
        "Cnt:%04lX Sum:%02X [%s] Auth[%s]\r\n",
        (unsigned long)instance->count,
        instance->checksum,
        csum_ok ? "OK" : "ERR",
        auth_ok ? "OK" : "ERR");
}
