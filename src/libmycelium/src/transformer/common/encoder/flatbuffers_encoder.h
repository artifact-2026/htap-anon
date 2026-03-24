#pragma once
// Private header — used only by flatbuffers_encoder.cc.
// Public declaration lives in mycelium/flatbuffers_encoder.h.
// FlatbufPayload (from flatbuffers_parser.h) and row_generated.h are included
// here because flatbuffers_encoder.cc uses them in its implementation.
#include "mycelium/flatbuffers_encoder.h"
#include "mycelium/flatbuffers_parser.h"
#include "row_generated.h"
