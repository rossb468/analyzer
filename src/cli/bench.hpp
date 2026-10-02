// Measuring the performance targets the plan commits to.
//
// Those targets were aspirations until something measured them. This is
// deliberately not a micro-benchmark suite: nobody cares how many nanoseconds
// one FFT takes, they care whether the analysis thread keeps up with the audio
// thread and whether anything gets dropped over a long run.
//
// So the headline figure is *duty cycle* - CPU seconds spent per second of
// audio analysed. Below 1.0 the chain keeps up; the plan asks for under 0.5 on
// one performance core, leaving headroom for a machine that is also doing
// something else.

#pragma once

#include <string>

namespace analyzer::cli {

// Run every benchmark, each over `seconds` of audio, and return a report.
std::string run_bench(double seconds);

}  // namespace analyzer::cli
