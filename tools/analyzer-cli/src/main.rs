//! Headless harness: drives the analysis chain from a file, a synthesised
//! signal, or live hardware, and writes results as text.
//!
//! This is the Milestone 0 deliverable and the validation vehicle for
//! everything after it. It proves numerical correctness before any UI exists,
//! and exercises every crate end to end — audio backend, allocation trap,
//! capture ring, analysis engine and spectrum analyzer — so an integration
//! mistake surfaces here rather than in the app.
//!
//! Offline analysis is deliberately single-threaded and reproducible; live
//! capture runs the real threaded engine against real hardware.

mod bench;
mod live;
mod measure;
mod report;

use std::fs;
use std::io::{self, Write as _};
use std::path::{Path, PathBuf};
use std::process::ExitCode;

use analyzer_audio::{AudioBuffers, DeviceId, OfflineBackend, Source, StreamConfig};
use analyzer_dsp::{Averaging, Overlap, SpectrumAnalyzer, SpectrumConfig, WindowKind};
use analyzer_engine::{AllocTrap, SpectrumFrame, capture_ring, rt_section};

/// The allocation trap is inert unless a binary registers it. Doing so here is
/// what makes the guard around every audio callback mean anything: if one ever
/// allocates, this process dies instead of quietly glitching.
#[cfg(debug_assertions)]
#[global_allocator]
static ALLOC_TRAP: AllocTrap = AllocTrap;

const USAGE: &str = "\
analyzer-cli - headless spectrum analysis harness

USAGE:
    analyzer-cli [OPTIONS] <input.wav>
    analyzer-cli [OPTIONS] --sine <hz>
    analyzer-cli [OPTIONS] --live [seconds]
    analyzer-cli --list-devices

INPUT:
    <input.wav>          WAV file (16/24/32-bit integer or 32-bit float)
    --sine <hz>          Synthesise a sine instead of reading a file
    --live [seconds]     Capture from hardware (default 5 seconds)
    --list-devices       Show every audio device and exit
    --bench [seconds]    Measure analysis throughput and ring behaviour (default 2)

SWEPT MEASUREMENT:
    --measure <a> <b>    Deconvolve response <b> against stimulus <a>
    --measure-demo       Build a synthetic room and measure it end to end
    --gate <ms>          Gate length for the quasi-anechoic response (default 5)

ANALYSIS:
    --fft <n>            FFT size, even (default 4096)
    --window <name>      rect | hann | bh | flattop | tukey (default hann)
    --overlap <pct>      0 | 50 | 75 | 87 (default 75)
    --average <mode>     none | infinite | peak (default infinite)
    --channel <n>        Which input channel to analyse (default 0)
    --block <frames>     Callback block size (default 128)

LIVE:
    --device <uid>       Capture device UID (default: system default input)
    --no-meter           Suppress the running level meter

SYNTHESIS:
    --rate <hz>          Sample rate for --sine (default 48000)
    --seconds <s>        Duration for --sine (default 1.0)
    --amplitude <a>      Peak amplitude for --sine (default 0.5)

OUTPUT:
    --min-db <db>        Omit bins quieter than this
    --peak               Print only the loudest bin
    --out <path>         Write to a file instead of stdout
    -h, --help           This text

Levels are dBFS with 0 dBFS = full-scale sine. Phase is not emitted: a
single-channel spectrum has no phase reference, and padding the column with
zeros would be fabricating data.
";

fn main() -> ExitCode {
    match run() {
        Ok(()) => ExitCode::SUCCESS,
        Err(error) => {
            eprintln!("analyzer-cli: {error}");
            ExitCode::FAILURE
        }
    }
}

#[derive(Debug)]
struct Args {
    input: Input,
    fft: usize,
    window: WindowKind,
    overlap: Overlap,
    average: Averaging,
    channel: usize,
    block: usize,
    min_db: Option<f32>,
    peak_only: bool,
    meter: bool,
    gate_ms: f32,
    out: Option<PathBuf>,
}

impl Args {
    fn spectrum(&self, sample_rate: f32) -> SpectrumConfig {
        SpectrumConfig {
            sample_rate,
            size: self.fft,
            window: self.window,
            overlap: self.overlap,
            averaging: self.average,
        }
    }
}

#[derive(Debug)]
enum Input {
    Wav(PathBuf),
    Sine {
        hz: f64,
        rate: f64,
        seconds: f64,
        amplitude: f32,
    },
    Live {
        device: Option<String>,
        seconds: f64,
    },
    ListDevices,
    Bench {
        seconds: f64,
    },
    MeasureDemo,
    Measure {
        stimulus: PathBuf,
        response: PathBuf,
    },
}

fn run() -> Result<(), String> {
    let Some(args) = parse_args()? else {
        print!("{USAGE}");
        return Ok(());
    };

    let report = match &args.input {
        Input::ListDevices => live::list_devices()?,
        Input::Bench { seconds } => bench::run(*seconds),
        Input::MeasureDemo => measure::demo(&measure::MeasureOptions {
            gate_ms: args.gate_ms,
            fft: args.fft,
            ..measure::MeasureOptions::default()
        })?,
        Input::Measure { stimulus, response } => measure::from_files(
            stimulus,
            response,
            &measure::MeasureOptions {
                gate_ms: args.gate_ms,
                fft: args.fft,
                ..measure::MeasureOptions::default()
            },
        )?,
        Input::Live { device, seconds } => {
            let options = live::LiveOptions {
                device: device.clone(),
                seconds: *seconds,
                channel: args.channel,
                block: args.block as u32,
                // Rate is replaced with whatever the device grants.
                spectrum: args.spectrum(48_000.0),
                meter: args.meter,
            };
            let frame = live::capture(&options)?;
            render_live(&frame, &args, &options)?
        }
        _ => {
            let source = load_source(&args)?;
            if source.frames() < args.fft {
                return Err(format!(
                    "need at least {} frames for a {}-point FFT, source has {}",
                    args.fft,
                    args.fft,
                    source.frames()
                ));
            }
            if args.channel >= source.channels {
                return Err(format!(
                    "channel {} requested but the source has {}",
                    args.channel, source.channels
                ));
            }
            analyse_offline(source, &args)?
        }
    };

    match &args.out {
        Some(path) => {
            fs::write(path, &report).map_err(|e| format!("writing {}: {e}", path.display()))?;
            eprintln!("wrote {}", path.display());
        }
        None => io::stdout()
            .write_all(report.as_bytes())
            .map_err(|e| format!("writing stdout: {e}"))?,
    }
    Ok(())
}

fn render_live(
    frame: &SpectrumFrame,
    args: &Args,
    options: &live::LiveOptions,
) -> Result<String, String> {
    // Rebuild the analyzer purely to recover the window's ENBW and hop for the
    // header; it never sees a sample.
    let reference = SpectrumAnalyzer::new(args.spectrum(frame.sample_rate));
    Ok(report::render(
        frame,
        &report::Meta {
            source: options
                .device
                .clone()
                .unwrap_or_else(|| "default input (live)".into()),
            channels: 1,
            channel: args.channel,
            sample_rate: f64::from(frame.sample_rate),
            window: args.window,
            overlap: args.overlap,
            averaging: args.average,
            enbw_hz: reference.enbw_hz(),
            fft_size: reference.size(),
            hop: reference.hop(),
        },
        args.min_db,
        args.peak_only,
    ))
}

fn analyse_offline(source: Source, args: &Args) -> Result<String, String> {
    let rate = source.sample_rate;
    let channels = source.channels;
    let frames = source.frames();

    let (mut sink, mut ring) = capture_ring(channels, 8192);

    let mut backend = OfflineBackend::new(source, args.block);
    let config = StreamConfig {
        input: Some(DeviceId::new(analyzer_audio::offline::OFFLINE_DEVICE_ID)),
        output: None,
        sample_rate: rate,
        buffer_frames: args.block as u32,
        input_channels: (0..channels as u32).collect(),
        output_channels: Vec::new(),
    };

    let mut stream = backend
        .open_offline(
            &config,
            Box::new(move |buffers: &mut AudioBuffers<'_>| {
                rt_section(|| {
                    sink.write_interleaved(buffers.input());
                });
            }),
        )
        .map_err(|e| format!("opening offline stream: {e}"))?;

    let mut analyzer = SpectrumAnalyzer::new(args.spectrum(rate as f32));
    let mut interleaved = vec![0.0_f32; args.block * channels];
    let mut channel_scratch = vec![0.0_f32; args.block];

    loop {
        if stream.pump() == 0 {
            break;
        }
        while ring.frames_available() > 0 {
            let read = ring.read_interleaved(&mut interleaved);
            if read == 0 {
                break;
            }
            for (frame, slot) in channel_scratch.iter_mut().take(read).enumerate() {
                *slot = interleaved
                    .get(frame * channels + args.channel)
                    .copied()
                    .unwrap_or_default();
            }
            analyzer.push(channel_scratch.get(..read).unwrap_or_default());
        }
    }

    let overruns = ring.overruns();
    if overruns > 0 {
        return Err(format!(
            "{overruns} block(s) dropped - the measurement is invalid"
        ));
    }
    if analyzer.frames() == 0 {
        return Err("no complete frames were analysed".into());
    }

    let mut bins = vec![0.0_f32; analyzer.bins()];
    analyzer.write_db_fs(&mut bins);

    let frame = SpectrumFrame {
        sequence: 1,
        bins,
        bin_spacing_hz: analyzer.bin_spacing_hz(),
        sample_rate: rate as f32,
        frames_averaged: analyzer.frames(),
        overruns,
    };

    Ok(report::render(
        &frame,
        &report::Meta {
            source: format!("{frames} frames offline"),
            channels,
            channel: args.channel,
            sample_rate: rate,
            window: args.window,
            overlap: args.overlap,
            averaging: args.average,
            enbw_hz: analyzer.enbw_hz(),
            fft_size: analyzer.size(),
            hop: analyzer.hop(),
        },
        args.min_db,
        args.peak_only,
    ))
}

fn load_source(args: &Args) -> Result<Source, String> {
    match &args.input {
        Input::Wav(path) => read_wav(path),
        Input::Sine {
            hz,
            rate,
            seconds,
            amplitude,
        } => {
            let frames = (rate * seconds).round().max(0.0) as usize;
            let samples = (0..frames)
                .map(|n| {
                    let phase = std::f64::consts::TAU * hz * n as f64 / rate;
                    amplitude * phase.sin() as f32
                })
                .collect();
            Ok(Source::mono(samples, *rate))
        }
        Input::Live { .. }
        | Input::ListDevices
        | Input::Bench { .. }
        | Input::MeasureDemo
        | Input::Measure { .. } => Err("this mode does not load a source".into()),
    }
}

pub(crate) fn read_wav(path: &Path) -> Result<Source, String> {
    use hound::SampleFormat;

    let mut reader =
        hound::WavReader::open(path).map_err(|e| format!("opening {}: {e}", path.display()))?;
    let spec = reader.spec();
    let context = || format!("reading {}", path.display());

    // Normalise every format to f32 in -1.0..=1.0 so nothing downstream has to
    // care how the file was stored.
    let samples: Vec<f32> = match (spec.sample_format, spec.bits_per_sample) {
        (SampleFormat::Float, 32) => reader
            .samples::<f32>()
            .collect::<Result<_, _>>()
            .map_err(|e| format!("{}: {e}", context()))?,
        (SampleFormat::Int, 16) => reader
            .samples::<i16>()
            .map(|s| s.map(|v| f32::from(v) / 32_768.0))
            .collect::<Result<_, _>>()
            .map_err(|e| format!("{}: {e}", context()))?,
        (SampleFormat::Int, 24) => reader
            .samples::<i32>()
            .map(|s| s.map(|v| v as f32 / 8_388_608.0))
            .collect::<Result<_, _>>()
            .map_err(|e| format!("{}: {e}", context()))?,
        (SampleFormat::Int, 32) => reader
            .samples::<i32>()
            .map(|s| s.map(|v| v as f32 / 2_147_483_648.0))
            .collect::<Result<_, _>>()
            .map_err(|e| format!("{}: {e}", context()))?,
        (format, bits) => {
            return Err(format!(
                "unsupported WAV format: {format:?} at {bits} bits per sample"
            ));
        }
    };

    if spec.channels == 0 {
        return Err(format!("{} declares zero channels", path.display()));
    }

    Ok(Source::new(
        samples,
        usize::from(spec.channels),
        f64::from(spec.sample_rate),
    ))
}

fn parse_args() -> Result<Option<Args>, String> {
    let mut positional: Option<PathBuf> = None;
    let mut sine_hz: Option<f64> = None;
    let mut live_seconds: Option<f64> = None;
    let mut list_devices = false;
    let mut bench_seconds: Option<f64> = None;
    let mut device: Option<String> = None;
    let mut fft = 4096_usize;
    let mut window = WindowKind::Hann;
    let mut overlap = Overlap::ThreeQuarters;
    let mut average = Averaging::Infinite;
    let mut channel = 0_usize;
    let mut block = 128_usize;
    let mut rate = 48_000.0_f64;
    let mut seconds = 1.0_f64;
    let mut amplitude = 0.5_f32;
    let mut min_db: Option<f32> = None;
    let mut peak_only = false;
    let mut meter = true;
    let mut gate_ms = 5.0_f32;
    let mut measure_demo = false;
    let mut measure_pair: Option<(PathBuf, PathBuf)> = None;
    let mut out: Option<PathBuf> = None;

    let mut argv: Vec<String> = std::env::args().skip(1).collect();
    argv.reverse();

    while let Some(arg) = argv.pop() {
        let mut value = || -> Result<String, String> {
            argv.pop()
                .ok_or_else(|| format!("{arg} needs a value (try --help)"))
        };

        match arg.as_str() {
            "-h" | "--help" => return Ok(None),
            "--peak" => peak_only = true,
            "--no-meter" => meter = false,
            "--list-devices" => list_devices = true,
            "--measure-demo" => measure_demo = true,
            "--gate" => gate_ms = number(&value()?, "--gate")?,
            "--measure" => {
                let stimulus = PathBuf::from(value()?);
                let response = PathBuf::from(value()?);
                measure_pair = Some((stimulus, response));
            }
            "--bench" => {
                let duration = match argv.last() {
                    Some(next) if next.parse::<f64>().is_ok() => {
                        argv.pop().and_then(|v| v.parse().ok()).unwrap_or(2.0)
                    }
                    _ => 2.0,
                };
                bench_seconds = Some(duration);
            }
            "--live" => {
                // The duration is optional, so only consume the next token when
                // it actually looks like a number rather than another flag.
                let duration = match argv.last() {
                    Some(next) if next.parse::<f64>().is_ok() => {
                        argv.pop().and_then(|v| v.parse().ok()).unwrap_or(5.0)
                    }
                    _ => 5.0,
                };
                live_seconds = Some(duration);
            }
            "--device" => device = Some(value()?),
            "--sine" => sine_hz = Some(number(&value()?, "--sine")?),
            "--fft" => fft = number(&value()?, "--fft")?,
            "--channel" => channel = number(&value()?, "--channel")?,
            "--block" => block = number(&value()?, "--block")?,
            "--rate" => rate = number(&value()?, "--rate")?,
            "--seconds" => seconds = number(&value()?, "--seconds")?,
            "--amplitude" => amplitude = number(&value()?, "--amplitude")?,
            "--min-db" => min_db = Some(number(&value()?, "--min-db")?),
            "--out" => out = Some(PathBuf::from(value()?)),
            "--window" => {
                let name = value()?;
                window = match name.as_str() {
                    "rect" | "rectangular" => WindowKind::Rectangular,
                    "hann" => WindowKind::Hann,
                    "bh" | "blackman-harris" => WindowKind::BlackmanHarris,
                    "flattop" | "flat-top" => WindowKind::FlatTop,
                    "tukey" => WindowKind::Tukey { alpha: 0.25 },
                    other => return Err(format!("unknown window '{other}'")),
                };
            }
            "--overlap" => {
                let pct = value()?;
                overlap = match pct.as_str() {
                    "0" => Overlap::None,
                    "50" => Overlap::Half,
                    "75" => Overlap::ThreeQuarters,
                    "87" | "87.5" => Overlap::SevenEighths,
                    other => return Err(format!("unknown overlap '{other}', want 0/50/75/87")),
                };
            }
            "--average" => {
                let mode = value()?;
                average = match mode.as_str() {
                    "none" => Averaging::None,
                    "infinite" | "inf" => Averaging::Infinite,
                    "peak" => Averaging::PeakHold,
                    other => return Err(format!("unknown averaging '{other}'")),
                };
            }
            other if other.starts_with('-') => {
                return Err(format!("unknown option '{other}' (try --help)"));
            }
            other => {
                if positional.is_some() {
                    return Err("more than one input path given".into());
                }
                positional = Some(PathBuf::from(other));
            }
        }
    }

    if fft < 2 || !fft.is_multiple_of(2) {
        return Err(format!("--fft must be even and at least 2, got {fft}"));
    }
    if block == 0 {
        return Err("--block must be non-zero".into());
    }

    let selected = [
        positional.is_some(),
        sine_hz.is_some(),
        live_seconds.is_some(),
        list_devices,
        bench_seconds.is_some(),
        measure_demo,
        measure_pair.is_some(),
    ]
    .iter()
    .filter(|chosen| **chosen)
    .count();
    if selected > 1 {
        return Err("choose one of: a WAV path, --sine, --live, --list-devices, --bench".into());
    }

    let input = if list_devices {
        Input::ListDevices
    } else if measure_demo {
        Input::MeasureDemo
    } else if let Some((stimulus, response)) = measure_pair {
        Input::Measure { stimulus, response }
    } else if let Some(seconds) = bench_seconds {
        if seconds <= 0.0 {
            return Err("--bench duration must be positive".into());
        }
        Input::Bench { seconds }
    } else if let Some(duration) = live_seconds {
        if duration <= 0.0 {
            return Err("--live duration must be positive".into());
        }
        Input::Live {
            device,
            seconds: duration,
        }
    } else if let Some(path) = positional {
        Input::Wav(path)
    } else if let Some(hz) = sine_hz {
        if rate <= 0.0 {
            return Err("--rate must be positive".into());
        }
        Input::Sine {
            hz,
            rate,
            seconds,
            amplitude,
        }
    } else {
        return Err("no input given (try --help)".into());
    };

    Ok(Some(Args {
        input,
        fft,
        window,
        overlap,
        average,
        channel,
        block,
        min_db,
        peak_only,
        meter,
        gate_ms,
        out,
    }))
}

fn number<T: std::str::FromStr>(raw: &str, flag: &str) -> Result<T, String> {
    raw.parse()
        .map_err(|_| format!("{flag}: cannot parse '{raw}'"))
}
