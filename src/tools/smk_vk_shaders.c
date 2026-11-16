#include <stdint.h>

const uint32_t smk_vk_vert_spv[] =
#include "smk_vk_vert_spv.inc"
;
const unsigned long smk_vk_vert_spv_len = sizeof(smk_vk_vert_spv);

const uint32_t smk_vk_frag_spv[] =
#include "smk_vk_frag_spv.inc"
;
const unsigned long smk_vk_frag_spv_len = sizeof(smk_vk_frag_spv);
