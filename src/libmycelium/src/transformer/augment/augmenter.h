#pragma once
// Private header: used only by augmenter.cc.
// The public class declaration lives in mycelium/augmenter.h.
// data.pb.h is included here so augmenter.cc can reach it without adding
// an explicit dependency on the proto-generated include path.
#include "data.pb.h"
#include "mycelium/augmenter.h"
