// SPDX-License-Identifier: MIT

// The decoding of the later passes of a loop in DSP-ADPCM, for hokuto_check_banks and hokuto_check_replace.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "ntdlib/sar/formats.h"
#include "ntdlib/wave/adpcm.h"

namespace hokuto::tools
{

// How a console decodes the later passes of a loop in DSP-ADPCM: its decoder's sums, and whether the header of the
// frame at the loop's start comes from the loop's context rather than from the frame.
struct LoopDecoding
{
    ntdlib::wave::DspAdpcmSum sum = ntdlib::wave::DspAdpcmSum::kWrap;
    bool context_header = false;
};

// The samples that the console decodes in a DSP-ADPCM channel's loop from the loop's second pass on, by `decoding`.
// Each such pass starts from the loop's context: the two samples before the loop's start, and a frame's predictor and
// scale.
inline std::vector<int16_t> LaterPass(std::span<const uint8_t> samples, const ntdlib::sar::DspAdpcmInfo& adpcm,
                                      uint32_t loop_start, uint32_t loop_end, const LoopDecoding& decoding)
{
    const ntdlib::wave::DspAdpcmSum sum = decoding.sum;
    std::vector<int16_t> pass;
    int32_t header = adpcm.loop_pred_scale & 0x7F;
    int32_t yn1 = adpcm.loop_yn1;
    int32_t yn2 = adpcm.loop_yn2;
    for (uint32_t i = loop_start; i < loop_end; i++)
    {
        // Each frame of 8 bytes holds a header and 14 samples of 4 bits.
        const std::size_t frame = std::size_t{i} / 14 * 8;
        const uint32_t place = i % 14;
        if (place == 0 && (i != loop_start || !decoding.context_header))
        {
            header = samples[frame];
        }
        const uint8_t pair = samples[frame + 1 + place / 2];
        const int32_t nibble = place % 2 == 0 ? pair >> 4 : pair & 0xF;
        const int32_t predictor = (header >> 4) & 7;
        const int16_t sample =
            ntdlib::wave::DspAdpcmSample(nibble >= 8 ? nibble - 16 : nibble, header & 0xF, adpcm.coefs[2 * predictor],
                                         adpcm.coefs[2 * predictor + 1], yn1, yn2, sum);
        pass.push_back(sample);
        yn2 = yn1;
        yn1 = sample;
    }

    return pass;
}

} // namespace hokuto::tools
