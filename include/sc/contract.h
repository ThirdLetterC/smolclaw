#pragma once

#include "sc/api.h"
#include "sc/string.h"

SC_BEGIN_DECLS

typedef enum sc_stability_tier {
    SC_STABILITY_EXPERIMENTAL = 0,
    SC_STABILITY_BETA,
    SC_STABILITY_STABLE
} sc_stability_tier;

typedef enum sc_attachment_delivery {
    SC_ATTACHMENT_DELIVERY_DEFAULT = 0,
    SC_ATTACHMENT_DELIVERY_PHOTO,
    SC_ATTACHMENT_DELIVERY_DOCUMENT
} sc_attachment_delivery;

enum {
    SC_CONTRACT_CAP_NONE = 0,
    SC_CONTRACT_CAP_STREAMING = 1u << 0u,
    SC_CONTRACT_CAP_TOOLS = 1u << 1u,
    SC_CONTRACT_CAP_BINARY = 1u << 2u,
    SC_CONTRACT_CAP_ASYNC = 1u << 3u,
    SC_CONTRACT_CAP_SECURE = 1u << 4u
};

bool sc_contract_name_is_valid(sc_str name);

SC_END_DECLS
