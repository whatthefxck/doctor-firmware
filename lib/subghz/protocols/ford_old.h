#pragma once

#include <furi.h>
#include <lib/subghz/protocols/base.h>
#include <lib/subghz/types.h>
#include <lib/subghz/blocks/const.h>
#include <lib/subghz/blocks/decoder.h>
#include <lib/subghz/blocks/encoder.h>
#include <lib/subghz/blocks/generic.h>
#include <lib/subghz/blocks/math.h>
#include <flipper_format/flipper_format.h>

#define SUBGHZ_PROTOCOL_FORD_OLD_NAME "Ford Old"

extern const SubGhzProtocol subghz_protocol_ford_old;

void* subghz_protocol_decoder_ford_old_alloc(SubGhzEnvironment* environment);
void subghz_protocol_decoder_ford_old_free(void* context);
void subghz_protocol_decoder_ford_old_reset(void* context);
void subghz_protocol_decoder_ford_old_feed(void* context, bool level, uint32_t duration);
SubGhzProtocolStatus subghz_protocol_decoder_ford_old_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);
SubGhzProtocolStatus
    subghz_protocol_decoder_ford_old_deserialize(void* context, FlipperFormat* flipper_format);
void subghz_protocol_decoder_ford_old_get_string(void* context, FuriString* output);

void* subghz_protocol_encoder_ford_old_alloc(SubGhzEnvironment* environment);
SubGhzProtocolStatus
    subghz_protocol_encoder_ford_old_deserialize(void* context, FlipperFormat* flipper_format);
