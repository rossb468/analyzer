# plot

Display data reduction and axis transforms. Emits no pixels.

This module sits between the analysis engine and whatever draws. It answers two
questions the renderer would otherwise have to answer for itself, and answering
them here is what keeps three future renderers thin and consistent:

- **Where does a value go on screen?** `axis.hpp` maps frequency and level to
  pixels and back. The mapping lives here rather than in the UI so that cursor
  readout, hit-testing and the drawn geometry cannot disagree.
- **Which value belongs in this column?** `reduce.hpp` collapses linearly spaced
  FFT bins onto a logarithmic axis, combining where bins are dense and
  interpolating where they are sparse.

What this module deliberately does not do is rasterise. Line traces become
geometry and the waterfall becomes one column of data per frame; compositing
belongs on the GPU, because redrawing a full Retina waterfall on the CPU is
roughly what makes REW's slow.

Clients never compute a bin-to-pixel mapping themselves: they call the axis
functions here (through the C ABI, `analyzer_freq_to_x` and friends).
