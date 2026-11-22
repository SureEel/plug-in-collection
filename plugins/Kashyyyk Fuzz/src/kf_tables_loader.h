// Kashyyyk Fuzz - the generated tables (kf_tables.inc) copied into a kf::Tables once.
#pragma once
#include "kf_dsp.h"
#include <cstring>
#include <memory>

namespace kf {
namespace loader {
#include "kf_tables.inc"
}
inline const Tables& loadTables()
{
    static std::unique_ptr<Tables> t;
    if (!t)
    {
        t.reset(new Tables());
        static_assert(sizeof(loader::kFfData) == sizeof(t->ff), "ff");
        static_assert(sizeof(loader::kMuffData) == sizeof(t->muff), "muff");
        static_assert(sizeof(loader::kOpData) == sizeof(t->op), "op");
        std::memcpy(t->ff, loader::kFfData, sizeof t->ff);
        std::memcpy(t->rinFF, loader::kRinFFData, sizeof t->rinFF);
        std::memcpy(t->ce, loader::kCeData, sizeof t->ce);
        std::memcpy(t->rinCE, loader::kRinCEData, sizeof t->rinCE);
        std::memcpy(t->muff, loader::kMuffData, sizeof t->muff);
        std::memcpy(t->rinMuff, loader::kRinMuffData, sizeof t->rinMuff);
        std::memcpy(t->di, loader::kDiData, sizeof t->di);
        std::memcpy(t->op, loader::kOpData, sizeof t->op);
        std::memcpy(t->rect, loader::kRectData, sizeof t->rect);
        std::memcpy(t->vdieFF, loader::kVdieData, sizeof t->vdieFF);
        std::memcpy(t->vdieCE, loader::kVdieData + kNumTransistors, sizeof t->vdieCE);
        std::memcpy(t->vdieMuff, loader::kVdieData + 2 * kNumTransistors, sizeof t->vdieMuff);
        static_assert(sizeof(loader::kEstData) == sizeof(t->est), "est");
        std::memcpy(t->est, loader::kEstData, sizeof t->est);
        std::memcpy(t->estTone, loader::kEstFactors, sizeof t->estTone);
        std::memcpy(t->estCap, loader::kEstFactors + kNumTones, sizeof t->estCap);
        std::memcpy(t->estCoupling, loader::kEstFactors + kNumTones + kNumCaps, sizeof t->estCoupling);
    }
    return *t;
}
} // namespace kf
