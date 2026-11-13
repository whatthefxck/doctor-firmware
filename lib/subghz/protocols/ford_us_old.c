#include <lib/subghz/blocks/custom_btn_i.h>
#include <lib/subghz/blocks/generic.h>
#include <lib/subghz/registry.h>
#include "ford_us_old.h"

// FCC GQ43VT4T 315 MHz

#define TAG "FordUSOld"

static const SubGhzBlockConst subghz_protocol_ford_us_old_const = {
    .te_short = 300,
    .te_long = 600,
    .te_delta = 170,
    .min_count_bit_for_found = 48,
};

#define FORD_US_OLD_TE_SYNC     400
#define FORD_US_OLD_BIT_GAP     3500
#define FORD_US_OLD_SYNC_GAP    15600
#define FORD_US_OLD_PREAMBLE    20
#define FORD_US_OLD_TOTAL_BURST 5
#define FORD_US_OLD_UPLOAD_CAPACITY \
    (FORD_US_OLD_TOTAL_BURST * (2 * FORD_US_OLD_PREAMBLE + 2 + 2 * 48 + 2))

typedef struct SubGhzProtocolDecoderFordUSOld {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;
    uint16_t header_count;
    uint64_t serial;
    uint8_t button;
} SubGhzProtocolDecoderFordUSOld;

typedef struct SubGhzProtocolEncoderFordUSOld {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;
} SubGhzProtocolEncoderFordUSOld;

typedef enum {
    FordUSOldDecoderStepReset = 0,
    FordUSOldDecoderStepPreamble,
    FordUSOldDecoderStepMark,
    FordUSOldDecoderStepGap,
} FordUSOldDecoderStep;

void subghz_protocol_encoder_ford_us_old_free(void* context);
void subghz_protocol_encoder_ford_us_old_stop(void* context);
LevelDuration subghz_protocol_encoder_ford_us_old_yield(void* context);
uint8_t subghz_protocol_decoder_ford_us_old_get_hash_data(void* context);

const SubGhzProtocolDecoder subghz_protocol_ford_us_old_decoder = {
    .alloc = subghz_protocol_decoder_ford_us_old_alloc,
    .free = subghz_protocol_decoder_ford_us_old_free,
    .feed = subghz_protocol_decoder_ford_us_old_feed,
    .reset = subghz_protocol_decoder_ford_us_old_reset,
    .get_hash_data = subghz_protocol_decoder_ford_us_old_get_hash_data,
    .serialize = subghz_protocol_decoder_ford_us_old_serialize,
    .deserialize = subghz_protocol_decoder_ford_us_old_deserialize,
    .get_string = subghz_protocol_decoder_ford_us_old_get_string,
};

const SubGhzProtocolEncoder subghz_protocol_ford_us_old_encoder = {
    .alloc = subghz_protocol_encoder_ford_us_old_alloc,
    .free = subghz_protocol_encoder_ford_us_old_free,
    .deserialize = subghz_protocol_encoder_ford_us_old_deserialize,
    .stop = subghz_protocol_encoder_ford_us_old_stop,
    .yield = subghz_protocol_encoder_ford_us_old_yield,
};

const SubGhzProtocol subghz_protocol_ford_us_old = {
    .name = SUBGHZ_PROTOCOL_FORD_US_OLD_NAME,
    .type = SubGhzProtocolTypeStatic,
    .flag = SubGhzProtocolFlag_315 | SubGhzProtocolFlag_AM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Send,
    .decoder = &subghz_protocol_ford_us_old_decoder,
    .encoder = &subghz_protocol_ford_us_old_encoder,
};

static uint8_t ford_us_old_button(uint64_t data) {
    uint8_t btn_bits = (data >> 9) & 0x7;
    if(btn_bits == 0x4) return 0x4;
    if(btn_bits == 0x2) return 0x2;
    if(btn_bits == 0x1) return 0x1;
    return btn_bits;
}

static SubGhzProtocolStatus
    ford_us_old_verify_protocol_name(FlipperFormat* ff, const char* expected_name) {
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

void* subghz_protocol_encoder_ford_us_old_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolEncoderFordUSOld* instance = calloc(1, sizeof(SubGhzProtocolEncoderFordUSOld));
    instance->base.protocol = &subghz_protocol_ford_us_old;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->encoder.repeat = 3;
    instance->encoder.upload = malloc(FORD_US_OLD_UPLOAD_CAPACITY * sizeof(LevelDuration));
    furi_check(instance->encoder.upload);
    instance->encoder.size_upload = FORD_US_OLD_UPLOAD_CAPACITY;
    instance->encoder.is_running = false;
    instance->encoder.front = 0;
    return instance;
}

static void subghz_protocol_encoder_ford_us_old_get_upload(SubGhzProtocolEncoderFordUSOld* instance) {
    furi_check(instance);
    size_t index = 0;
    uint32_t te_short = subghz_protocol_ford_us_old_const.te_short;
    uint32_t te_long = subghz_protocol_ford_us_old_const.te_long;
    uint64_t data = instance->generic.data;
    uint8_t nbit = instance->generic.data_count_bit;
    LevelDuration* up = instance->encoder.upload;

    for(uint8_t burst = 0; burst < FORD_US_OLD_TOTAL_BURST; burst++) {
        for(int i = 0; i < FORD_US_OLD_PREAMBLE; i++) {
            up[index++] = level_duration_make(true, FORD_US_OLD_TE_SYNC);
            up[index++] = level_duration_make(false, FORD_US_OLD_BIT_GAP);
        }
        up[index++] = level_duration_make(true, FORD_US_OLD_TE_SYNC);
        up[index++] = level_duration_make(false, FORD_US_OLD_SYNC_GAP);

        for(int bit = nbit - 1; bit >= 0; bit--) {
            uint32_t mark = ((data >> bit) & 1) ? te_long : te_short;
            up[index++] = level_duration_make(true, mark);
            up[index++] =
                level_duration_make(false, (bit == 0) ? FORD_US_OLD_SYNC_GAP : FORD_US_OLD_BIT_GAP);
        }
    }

    instance->encoder.size_upload = index;
    instance->encoder.front = 0;
}

void subghz_protocol_encoder_ford_us_old_free(void* context) {
    furi_check(context);
    SubGhzProtocolEncoderFordUSOld* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

void subghz_protocol_encoder_ford_us_old_stop(void* context) {
    furi_check(context);
    SubGhzProtocolEncoderFordUSOld* instance = context;
    instance->encoder.is_running = false;
    instance->encoder.front = 0;
}

LevelDuration subghz_protocol_encoder_ford_us_old_yield(void* context) {
    furi_check(context);
    SubGhzProtocolEncoderFordUSOld* instance = context;
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
    subghz_protocol_encoder_ford_us_old_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolEncoderFordUSOld* instance = context;
    SubGhzProtocolStatus ret = SubGhzProtocolStatusError;
    instance->encoder.is_running = false;
    instance->encoder.front = 0;

    do {
        if(ford_us_old_verify_protocol_name(flipper_format, instance->base.protocol->name) !=
           SubGhzProtocolStatusOk)
            break;
        ret = subghz_block_generic_deserialize_check_count_bit(
            &instance->generic,
            flipper_format,
            subghz_protocol_ford_us_old_const.min_count_bit_for_found);
        if(ret != SubGhzProtocolStatusOk) break;

        flipper_format_rewind(flipper_format);
        uint32_t rep = 0;
        if(flipper_format_read_uint32(flipper_format, "Repeat", &rep, 1)) instance->encoder.repeat = rep;
        else instance->encoder.repeat = 3;

        subghz_protocol_encoder_ford_us_old_get_upload(instance);
        if(instance->encoder.size_upload == 0) break;
        instance->encoder.is_running = true;
        ret = SubGhzProtocolStatusOk;
    } while(false);

    return ret;
}

void* subghz_protocol_decoder_ford_us_old_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderFordUSOld* instance = malloc(sizeof(SubGhzProtocolDecoderFordUSOld));
    instance->base.protocol = &subghz_protocol_ford_us_old;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void subghz_protocol_decoder_ford_us_old_free(void* context) {
    furi_check(context);
    free(context);
}

void subghz_protocol_decoder_ford_us_old_reset(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderFordUSOld* instance = context;
    instance->decoder.parser_step = FordUSOldDecoderStepReset;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->decoder.te_last = 0;
    instance->header_count = 0;
}

uint8_t subghz_protocol_decoder_ford_us_old_get_hash_data(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderFordUSOld* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8U) + 1U);
}

static void ford_us_old_emit(SubGhzProtocolDecoderFordUSOld* instance) {
    uint8_t n = instance->decoder.decode_count_bit;
    if(n >= subghz_protocol_ford_us_old_const.min_count_bit_for_found &&
       n <= subghz_protocol_ford_us_old_const.min_count_bit_for_found + 1) {
        uint64_t data = instance->decoder.decode_data;
        if(n == subghz_protocol_ford_us_old_const.min_count_bit_for_found + 1) data >>= 1;
        if(instance->generic.data != data) {
            instance->generic.data = data;
            instance->generic.data_count_bit =
                subghz_protocol_ford_us_old_const.min_count_bit_for_found;
            instance->serial = data >> 12;
            instance->button = ford_us_old_button(data);
            instance->generic.serial = (uint32_t)(instance->serial & 0xFFFFFFFF);
            instance->generic.btn = instance->button;
            if(instance->base.callback)
                instance->base.callback(&instance->base, instance->base.context);
        }
    }
}

void subghz_protocol_decoder_ford_us_old_feed(void* context, bool level, uint32_t duration) {
    furi_check(context);
    SubGhzProtocolDecoderFordUSOld* instance = context;
    uint32_t te_short = subghz_protocol_ford_us_old_const.te_short;
    uint32_t te_long = subghz_protocol_ford_us_old_const.te_long;
    uint32_t te_delta = subghz_protocol_ford_us_old_const.te_delta;

    switch(instance->decoder.parser_step) {
    case FordUSOldDecoderStepReset:
        if(level && duration >= (te_short - te_delta) && duration <= (te_long + te_delta)) {
            instance->decoder.parser_step = FordUSOldDecoderStepPreamble;
            instance->header_count = 1;
        }
        break;

    case FordUSOldDecoderStepPreamble:
        if(level) {
            instance->header_count++;
        } else {
            if(DURATION_DIFF(duration, FORD_US_OLD_SYNC_GAP) < 3000) {
                if(instance->header_count >= 10) {
                    instance->decoder.parser_step = FordUSOldDecoderStepMark;
                    instance->decoder.decode_data = 0;
                    instance->decoder.decode_count_bit = 0;
                } else {
                    instance->decoder.parser_step = FordUSOldDecoderStepReset;
                    instance->header_count = 0;
                }
            } else if(DURATION_DIFF(duration, FORD_US_OLD_BIT_GAP) < 1500) {
            } else {
                instance->decoder.parser_step = FordUSOldDecoderStepReset;
                instance->header_count = 0;
            }
        }
        break;

    case FordUSOldDecoderStepMark:
        if(level) {
            instance->decoder.te_last = duration;
            instance->decoder.parser_step = FordUSOldDecoderStepGap;
        } else {
            instance->decoder.parser_step = FordUSOldDecoderStepReset;
            instance->header_count = 0;
        }
        break;

    case FordUSOldDecoderStepGap:
        if(!level) {
            uint8_t bit = 2;
            if(DURATION_DIFF(instance->decoder.te_last, te_short) < te_delta)
                bit = 0;
            else if(DURATION_DIFF(instance->decoder.te_last, te_long) < te_delta)
                bit = 1;
            if(bit == 2) {
                instance->decoder.parser_step = FordUSOldDecoderStepReset;
                instance->header_count = 0;
                break;
            }
            subghz_protocol_blocks_add_bit(&instance->decoder, bit);

            if(duration >= 8000) {
                ford_us_old_emit(instance);
                instance->decoder.parser_step = FordUSOldDecoderStepReset;
                instance->decoder.decode_data = 0;
                instance->decoder.decode_count_bit = 0;
                instance->header_count = 0;
            } else if(DURATION_DIFF(duration, FORD_US_OLD_BIT_GAP) < 1500) {
                instance->decoder.parser_step = FordUSOldDecoderStepMark;
            } else {
                instance->decoder.parser_step = FordUSOldDecoderStepReset;
                instance->header_count = 0;
            }
        } else {
            instance->decoder.parser_step = FordUSOldDecoderStepReset;
            instance->header_count = 0;
        }
        break;
    }
}

SubGhzProtocolStatus subghz_protocol_decoder_ford_us_old_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_check(context);
    SubGhzProtocolDecoderFordUSOld* instance = context;
    SubGhzProtocolStatus ret =
        subghz_block_generic_serialize(&instance->generic, flipper_format, preset);
    if(ret == SubGhzProtocolStatusOk) {
        uint32_t btn_w = instance->button;
        flipper_format_rewind(flipper_format);
        flipper_format_insert_or_update_uint32(flipper_format, "Btn", &btn_w, 1);
    }
    return ret;
}

SubGhzProtocolStatus
    subghz_protocol_decoder_ford_us_old_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolDecoderFordUSOld* instance = context;
    SubGhzProtocolStatus ret = subghz_block_generic_deserialize_check_count_bit(
        &instance->generic,
        flipper_format,
        subghz_protocol_ford_us_old_const.min_count_bit_for_found);
    if(ret == SubGhzProtocolStatusOk) {
        instance->serial = instance->generic.data >> 12;
        instance->button = ford_us_old_button(instance->generic.data);
        instance->generic.serial = (uint32_t)(instance->serial & 0xFFFFFFFF);
        instance->generic.btn = instance->button;
    }
    return ret;
}

void subghz_protocol_decoder_ford_us_old_get_string(void* context, FuriString* output) {
    furi_check(context);
    SubGhzProtocolDecoderFordUSOld* instance = context;

    const char* button_name = "??";
    if(instance->button == 0x4)
        button_name = "Unlock";
    else if(instance->button == 0x2)
        button_name = "Trunk";
    else if(instance->button == 0x1)
        button_name = "Lock";

    furi_string_cat_printf(
        output, "%s %ubit\r\n", instance->generic.protocol_name, instance->generic.data_count_bit);
    furi_string_cat_printf(
        output,
        "Key:%03lX%08lX\r\n",
        (unsigned long)(instance->generic.data >> 32),
        (unsigned long)(instance->generic.data & 0xFFFFFFFF));
    furi_string_cat_printf(
        output,
        "Sn:%09llX Btn:%X [%s]\r\n",
        (unsigned long long)instance->serial,
        instance->button,
        button_name);
}
