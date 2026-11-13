#include <lib/subghz/blocks/custom_btn_i.h>
#include <lib/subghz/blocks/generic.h>
#include <lib/subghz/registry.h>
#include "keeloq_common.h"
#include "ford_old.h"

#define TAG "FordOld"

#define FORD_OLD_MFKEY 0xA8F5DFFC8DAA5CDBULL

static const SubGhzBlockConst subghz_protocol_ford_old_const = {
    .te_short = 400,
    .te_long = 800,
    .te_delta = 140,
    .min_count_bit_for_found = 64,
};

#define FORD_OLD_PREAMBLE_PAIRS 11
#define FORD_OLD_HEADER_GAP_US  1200
#define FORD_OLD_TOTAL_BURSTS   4
#define FORD_OLD_GUARD_US       16000
#define FORD_OLD_UPLOAD_CAPACITY \
    (FORD_OLD_TOTAL_BURSTS * (2 * FORD_OLD_PREAMBLE_PAIRS + 2 + 2 * 64 + 2))

typedef struct SubGhzProtocolDecoderFordOld {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;
    uint16_t header_count;
    uint32_t serial;
    uint8_t button;
    uint32_t counter;
    uint32_t hop;
} SubGhzProtocolDecoderFordOld;

typedef struct SubGhzProtocolEncoderFordOld {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;
    uint32_t serial;
    uint8_t button;
    uint32_t counter;
} SubGhzProtocolEncoderFordOld;

typedef enum {
    FordOldDecoderStepReset = 0,
    FordOldDecoderStepCheckPreamble,
    FordOldDecoderStepSaveDuration,
    FordOldDecoderStepCheckDuration,
} FordOldDecoderStep;

void subghz_protocol_encoder_ford_old_free(void* context);
void subghz_protocol_encoder_ford_old_stop(void* context);
LevelDuration subghz_protocol_encoder_ford_old_yield(void* context);
uint8_t subghz_protocol_decoder_ford_old_get_hash_data(void* context);

const SubGhzProtocolDecoder subghz_protocol_ford_old_decoder = {
    .alloc = subghz_protocol_decoder_ford_old_alloc,
    .free = subghz_protocol_decoder_ford_old_free,
    .feed = subghz_protocol_decoder_ford_old_feed,
    .reset = subghz_protocol_decoder_ford_old_reset,
    .get_hash_data = subghz_protocol_decoder_ford_old_get_hash_data,
    .serialize = subghz_protocol_decoder_ford_old_serialize,
    .deserialize = subghz_protocol_decoder_ford_old_deserialize,
    .get_string = subghz_protocol_decoder_ford_old_get_string,
};

const SubGhzProtocolEncoder subghz_protocol_ford_old_encoder = {
    .alloc = subghz_protocol_encoder_ford_old_alloc,
    .free = subghz_protocol_encoder_ford_old_free,
    .deserialize = subghz_protocol_encoder_ford_old_deserialize,
    .stop = subghz_protocol_encoder_ford_old_stop,
    .yield = subghz_protocol_encoder_ford_old_yield,
};

const SubGhzProtocol subghz_protocol_ford_old = {
    .name = SUBGHZ_PROTOCOL_FORD_OLD_NAME,
    .type = SubGhzProtocolTypeDynamic,
    .flag = SubGhzProtocolFlag_315 | SubGhzProtocolFlag_433 | SubGhzProtocolFlag_AM |
            SubGhzProtocolFlag_Decodable | SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save |
            SubGhzProtocolFlag_Send,
    .decoder = &subghz_protocol_ford_old_decoder,
    .encoder = &subghz_protocol_ford_old_encoder,
};

static SubGhzProtocolStatus
    ford_old_verify_protocol_name(FlipperFormat* ff, const char* expected_name) {
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

static void ford_old_read_fields(
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

static uint8_t ford_old_get_btn_code(void) {
    switch(subghz_custom_btn_get()) {
    case SUBGHZ_CUSTOM_BTN_LEFT:
        return 0x1;
    case SUBGHZ_CUSTOM_BTN_UP:
        return 0x2;
    case SUBGHZ_CUSTOM_BTN_OK:
        return 0x4;
    case SUBGHZ_CUSTOM_BTN_DOWN:
        return 0x8;
    default:
        return subghz_custom_btn_get_original();
    }
}

static uint64_t ford_old_assemble(uint32_t serial, uint8_t button, uint32_t counter) {
    uint32_t plain = ((uint32_t)(button & 0xF) << 28) | ((serial & 0x3FF) << 16) |
                     (counter & 0xFFFF);
    uint32_t hop = subghz_protocol_keeloq_common_encrypt(plain, FORD_OLD_MFKEY);
    uint64_t fix = ((uint64_t)(button & 0xF) << 28) | (serial & 0x0FFFFFFF);
    return (fix << 32) | hop;
}

static void ford_old_disassemble(
    uint64_t data,
    uint32_t* serial,
    uint8_t* button,
    uint32_t* counter,
    uint32_t* hop) {
    uint32_t h = (uint32_t)(data & 0xFFFFFFFF);
    uint32_t fix = (uint32_t)(data >> 32);
    *serial = fix & 0x0FFFFFFF;
    *button = (fix >> 28) & 0xF;
    *hop = h;
    uint32_t plain = subghz_protocol_keeloq_common_decrypt(h, FORD_OLD_MFKEY);
    *counter = plain & 0xFFFF;
}

void* subghz_protocol_encoder_ford_old_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolEncoderFordOld* instance = calloc(1, sizeof(SubGhzProtocolEncoderFordOld));
    instance->base.protocol = &subghz_protocol_ford_old;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->encoder.repeat = 3;
    instance->encoder.upload = malloc(FORD_OLD_UPLOAD_CAPACITY * sizeof(LevelDuration));
    furi_check(instance->encoder.upload);
    instance->encoder.size_upload = FORD_OLD_UPLOAD_CAPACITY;
    instance->encoder.is_running = false;
    instance->encoder.front = 0;
    return instance;
}

static void subghz_protocol_encoder_ford_old_get_upload(SubGhzProtocolEncoderFordOld* instance) {
    furi_check(instance);
    size_t index = 0;
    uint32_t te_short = subghz_protocol_ford_old_const.te_short;
    uint32_t te_long = subghz_protocol_ford_old_const.te_long;

    uint64_t data = ford_old_assemble(instance->serial, instance->button, instance->counter);
    instance->generic.data = data;
    instance->generic.data_count_bit = 64;

    LevelDuration* up = instance->encoder.upload;

    for(uint8_t burst = 0; burst < FORD_OLD_TOTAL_BURSTS; burst++) {
        for(int i = 0; i < FORD_OLD_PREAMBLE_PAIRS; i++) {
            up[index++] = level_duration_make(true, te_short);
            up[index++] = level_duration_make(false, te_short);
        }
        up[index++] = level_duration_make(true, te_short);
        up[index++] = level_duration_make(false, FORD_OLD_HEADER_GAP_US);

        for(int bit = 63; bit >= 0; bit--) {
            if((data >> bit) & 1) {
                up[index++] = level_duration_make(true, te_short);
                up[index++] = level_duration_make(false, te_long);
            } else {
                up[index++] = level_duration_make(true, te_long);
                up[index++] = level_duration_make(false, te_short);
            }
        }
        up[index++] = level_duration_make(true, te_short);
        up[index++] = level_duration_make(false, FORD_OLD_GUARD_US);
    }

    instance->encoder.size_upload = index;
    instance->encoder.front = 0;
}

void subghz_protocol_encoder_ford_old_free(void* context) {
    furi_check(context);
    SubGhzProtocolEncoderFordOld* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

void subghz_protocol_encoder_ford_old_stop(void* context) {
    furi_check(context);
    SubGhzProtocolEncoderFordOld* instance = context;
    instance->encoder.is_running = false;
    instance->encoder.front = 0;
}

LevelDuration subghz_protocol_encoder_ford_old_yield(void* context) {
    furi_check(context);
    SubGhzProtocolEncoderFordOld* instance = context;
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

SubGhzProtocolStatus
    subghz_protocol_encoder_ford_old_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolEncoderFordOld* instance = context;
    SubGhzProtocolStatus ret = SubGhzProtocolStatusError;

    instance->encoder.is_running = false;
    instance->encoder.front = 0;

    do {
        if(ford_old_verify_protocol_name(flipper_format, instance->base.protocol->name) !=
           SubGhzProtocolStatusOk)
            break;
        ret = subghz_block_generic_deserialize(&instance->generic, flipper_format);
        if(ret != SubGhzProtocolStatusOk) break;

        uint32_t serial = instance->generic.serial;
        uint32_t btn = instance->generic.btn;
        uint32_t cnt = instance->generic.cnt;
        ford_old_read_fields(flipper_format, &serial, &btn, &cnt);

        if(!subghz_custom_btn_has_original()) subghz_custom_btn_set_original((uint8_t)btn);
        btn = ford_old_get_btn_code();
        subghz_block_generic_global_button_override_get((uint8_t*)&btn);
        if(!subghz_block_generic_global_counter_override_get(&cnt))
            cnt += furi_hal_subghz_get_rolling_counter_mult();

        instance->serial = serial;
        instance->button = (uint8_t)btn;
        instance->counter = cnt;
        instance->generic.serial = serial;
        instance->generic.btn = instance->button;
        instance->generic.cnt = cnt;

        instance->encoder.repeat = 3;
        flipper_format_rewind(flipper_format);
        uint32_t rep = 0;
        if(flipper_format_read_uint32(flipper_format, "Repeat", &rep, 1)) instance->encoder.repeat = rep;

        subghz_protocol_encoder_ford_old_get_upload(instance);
        if(instance->encoder.size_upload == 0) break;

        flipper_format_rewind(flipper_format);
        uint32_t serial_w = instance->serial;
        flipper_format_insert_or_update_uint32(flipper_format, "Serial", &serial_w, 1);
        uint32_t btn_w = instance->button;
        flipper_format_insert_or_update_uint32(flipper_format, "Btn", &btn_w, 1);
        uint32_t cnt_w = instance->counter;
        flipper_format_insert_or_update_uint32(flipper_format, "Cnt", &cnt_w, 1);
        subghz_block_generic_global.current_cnt = instance->counter;

        instance->encoder.is_running = true;
        ret = SubGhzProtocolStatusOk;
    } while(false);

    return ret;
}

void* subghz_protocol_decoder_ford_old_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderFordOld* instance = malloc(sizeof(SubGhzProtocolDecoderFordOld));
    instance->base.protocol = &subghz_protocol_ford_old;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void subghz_protocol_decoder_ford_old_free(void* context) {
    furi_check(context);
    free(context);
}

void subghz_protocol_decoder_ford_old_reset(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderFordOld* instance = context;
    instance->decoder.parser_step = FordOldDecoderStepReset;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->decoder.te_last = 0;
    instance->header_count = 0;
}

uint8_t subghz_protocol_decoder_ford_old_get_hash_data(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderFordOld* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8U) + 1U);
}

void subghz_protocol_decoder_ford_old_feed(void* context, bool level, uint32_t duration) {
    furi_check(context);
    SubGhzProtocolDecoderFordOld* instance = context;
    uint32_t te_short = subghz_protocol_ford_old_const.te_short;
    uint32_t te_long = subghz_protocol_ford_old_const.te_long;
    uint32_t te_delta = subghz_protocol_ford_old_const.te_delta;

    switch(instance->decoder.parser_step) {
    case FordOldDecoderStepReset:
        if(level && DURATION_DIFF(duration, te_short) < te_delta) {
            instance->decoder.parser_step = FordOldDecoderStepCheckPreamble;
            instance->header_count++;
        }
        break;

    case FordOldDecoderStepCheckPreamble:
        if(!level) {
            if(DURATION_DIFF(duration, te_short) < te_delta) {
                instance->decoder.parser_step = FordOldDecoderStepReset;
            } else if(
                (instance->header_count > 6) &&
                DURATION_DIFF(duration, FORD_OLD_HEADER_GAP_US) < te_delta * 3) {
                instance->decoder.parser_step = FordOldDecoderStepSaveDuration;
                instance->decoder.decode_data = 0;
                instance->decoder.decode_count_bit = 0;
            } else {
                instance->decoder.parser_step = FordOldDecoderStepReset;
                instance->header_count = 0;
            }
        } else {
            instance->decoder.parser_step = FordOldDecoderStepReset;
            instance->header_count = 0;
        }
        break;

    case FordOldDecoderStepSaveDuration:
        if(level) {
            instance->decoder.te_last = duration;
            instance->decoder.parser_step = FordOldDecoderStepCheckDuration;
        }
        break;

    case FordOldDecoderStepCheckDuration:
        if(!level) {
            if(duration >= ((uint32_t)te_short * 2 + te_delta * 3)) {
                instance->decoder.parser_step = FordOldDecoderStepReset;
                if(instance->decoder.decode_count_bit ==
                   subghz_protocol_ford_old_const.min_count_bit_for_found) {
                    if(instance->generic.data != instance->decoder.decode_data) {
                        instance->generic.data = instance->decoder.decode_data;
                        instance->generic.data_count_bit = instance->decoder.decode_count_bit;
                        ford_old_disassemble(
                            instance->generic.data,
                            &instance->serial,
                            &instance->button,
                            &instance->counter,
                            &instance->hop);
                        instance->generic.serial = instance->serial;
                        instance->generic.btn = instance->button;
                        instance->generic.cnt = instance->counter;
                        if(instance->base.callback)
                            instance->base.callback(&instance->base, instance->base.context);
                    }
                }
                instance->decoder.decode_data = 0;
                instance->decoder.decode_count_bit = 0;
                instance->header_count = 0;
            } else if(
                (DURATION_DIFF(instance->decoder.te_last, te_short) < te_delta) &&
                (DURATION_DIFF(duration, te_long) < te_delta * 2)) {
                subghz_protocol_blocks_add_bit(&instance->decoder, 1);
                instance->decoder.parser_step = FordOldDecoderStepSaveDuration;
            } else if(
                (DURATION_DIFF(instance->decoder.te_last, te_long) < te_delta * 2) &&
                (DURATION_DIFF(duration, te_short) < te_delta)) {
                subghz_protocol_blocks_add_bit(&instance->decoder, 0);
                instance->decoder.parser_step = FordOldDecoderStepSaveDuration;
            } else {
                instance->decoder.parser_step = FordOldDecoderStepReset;
                instance->header_count = 0;
            }
        }
        break;
    }
}

SubGhzProtocolStatus subghz_protocol_decoder_ford_old_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_check(context);
    SubGhzProtocolDecoderFordOld* instance = context;
    SubGhzProtocolStatus ret =
        subghz_block_generic_serialize(&instance->generic, flipper_format, preset);
    if(ret == SubGhzProtocolStatusOk) {
        uint32_t serial_w = instance->serial;
        flipper_format_rewind(flipper_format);
        flipper_format_insert_or_update_uint32(flipper_format, "Serial", &serial_w, 1);
        uint32_t btn_w = instance->button;
        flipper_format_rewind(flipper_format);
        flipper_format_insert_or_update_uint32(flipper_format, "Btn", &btn_w, 1);
        uint32_t cnt_w = instance->counter;
        flipper_format_rewind(flipper_format);
        flipper_format_insert_or_update_uint32(flipper_format, "Cnt", &cnt_w, 1);
    }
    return ret;
}

SubGhzProtocolStatus
    subghz_protocol_decoder_ford_old_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolDecoderFordOld* instance = context;
    SubGhzProtocolStatus ret = subghz_block_generic_deserialize_check_count_bit(
        &instance->generic,
        flipper_format,
        subghz_protocol_ford_old_const.min_count_bit_for_found);
    if(ret == SubGhzProtocolStatusOk) {
        ford_old_disassemble(
            instance->generic.data,
            &instance->serial,
            &instance->button,
            &instance->counter,
            &instance->hop);
        instance->generic.serial = instance->serial;
        instance->generic.btn = instance->button;
        instance->generic.cnt = instance->counter;
    }
    return ret;
}

void subghz_protocol_decoder_ford_old_get_string(void* context, FuriString* output) {
    furi_check(context);
    SubGhzProtocolDecoderFordOld* instance = context;

    subghz_block_generic_global.btn_is_available = true;
    subghz_block_generic_global.cnt_is_available = true;
    subghz_block_generic_global.btn_length_bit = 4;
    subghz_block_generic_global.cnt_length_bit = 16;
    subghz_block_generic_global.current_btn = instance->button;
    subghz_block_generic_global.current_cnt = instance->counter;
    if(!subghz_custom_btn_has_original()) subghz_custom_btn_set_original(instance->button);
    subghz_custom_btn_set_max(4);

    furi_string_cat_printf(
        output, "%s %ubit\r\n", instance->generic.protocol_name, instance->generic.data_count_bit);
    furi_string_cat_printf(
        output,
        "Key:%08lX%08lX\r\n",
        (unsigned long)(instance->generic.data >> 32),
        (unsigned long)(instance->generic.data & 0xFFFFFFFF));
    furi_string_cat_printf(
        output,
        "Sn:%07lX Btn:%X\r\n",
        (unsigned long)instance->serial,
        instance->button);
    furi_string_cat_printf(
        output, "Hop:%08lX Cnt:%04lX\r\n", (unsigned long)instance->hop, (unsigned long)instance->counter);
}
