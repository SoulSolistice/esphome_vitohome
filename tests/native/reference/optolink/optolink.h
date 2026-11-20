#pragma once

// The reference engines (README.md): what the fuzz harnesses include in place
// of optolink/optolink.h when they build the reference side.

#include "constants.h"
#include "helpers.h"
#include "protocol/gwg/gwg.h"
#include "protocol/gwg/packet_gwg.h"
#include "protocol/vs1/packet_vs1.h"
#include "protocol/vs1/vs1.h"
#include "protocol/vs2/packet_vs2.h"
#include "protocol/vs2/parser_vs2.h"
#include "protocol/vs2/vs2.h"
