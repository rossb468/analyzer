//! Headless harness: drives the analysis chain from a WAV file and writes results
//! as text.
//!
//! This is the Milestone 0 deliverable. It proves numerical correctness before any
//! UI exists, which is the opposite of how this kind of project usually goes
//! wrong. It also exercises every crate in the workspace end to end — the audio
//! backend, the allocation trap, the capture ring and the spectrum analyzer — so
//! an integration mistake surfaces here rather than in the app.
//!
//! Deliberately single-threaded: the stream is pumped and the ring drained in
//! lockstep on one thread, so output is reproducible. The threaded hand-offs are
//! already covered by tests in `analyzer-engine`; a correctness harness wants
//! determinism more than realism.

use std::fmt::Write as _;
use std::fs;
use std::io::{self, Write as _};
use std::path::{Path, PathBuf};
use std::process::ExitCode;

use analyzer_audio::{AudioBuffers, DeviceId, OfflineBackend, Source, StreamConfig};
use analyzer_dsp::{Averaging, Overlap, SpectrumAnalyzer, SpectrumConfig, WindowKind};
use analyzer_engine::{AllocTrap, capture_ring, rt_section};

/// The allocation trap is inert unless a binary registers it. Doing so here is
/// what makes the guard around the callback below mean anything: if that callback
/// ever allocates, this process dies instead of quietly glitching.
#[cfg(debug_assertions)]
#[global_allocator]
static ALLOC_TRAP: AllocTrap = AllocTrap;

const USAGE: &str = "\
analyzer-cli - headless spectrum analysis harness

USAGE:
    analyzer-cli [OPTIONS] <input.wav>
    analyzer-cli [OPTIONS] --sine <hz>

INPUT:
    <input.wav>          WAV file (16/24/32-bit integer or 32-bit float)
    --sine <hz>          Synthesise a sine instead of reading a file

OPTIONS:
    --fft <n>            FFT size, even (default 4096)
    --window <name>      rect | hann | bh | flattop | tukey (default hann)
    --overlap <pct>      0 | 50 | 75 | 87 (default 75)
    --average <mode>     none | infinite | peak (default infinite)
    --channel <n>        Which input channel to analyse (default 0)
    --block <frames>     Callback block size (default 128)
    --rate <hz>          Sample rate for --sine (default 48000)
    --seconds <s>        Duration for --sine (default 1.0)
    --amplitude <a>      Peak amplitude for --sine (default 0.5)
    --min-db <db>        Omit bins quieter than this
    --peak               Print only the loudest bin
    --out <path>         Write to a file instead of stdout
    -h, --help           This text

OUTPUT:
    Tab-separated frequency and level, with '#' comment lines carrying the
    settings. Levels are dBFS with 0 dBFS = full-scale sine. Phase is not
    emitted: a single-channel spectrum has no phase reference, and padding the
    column with zeros would be fabricating data.
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
    out: Option<PathBuf>,
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
}

fn run() -> Result<(), String> {
    let Some(args) = parse_args()? else {
        print!("{USAGE}");
        return Ok(());
    };

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

    let report = analyse(source, &args)?;

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

fn analyse(source: Source, args: &Args) -> Result<String, String> {
    let rate = source.sample_rate;
    let channels = source.channels;
    let frames = source.frames();

    // Sized for worst-case scheduling latency rather than throughput. Nothing is
    // descheduled here, but a realistic capacity keeps the harness honest about
    // what the real engine will do.
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
            // The real-time path. It decides nothing and computes nothing: hand
            // the block to the ring and return. rt_section aborts the process if
            // this ever allocates.
            Box::new(move |buffers: &mut AudioBuffers<'_>| {
                rt_section(|| {
                    sink.write_interleaved(buffers.input());
                });
            }),
        )
        .map_err(|e| format!("opening offline stream: {e}"))?;

    let mut analyzer = SpectrumAnalyzer::new(SpectrumConfig {
        sample_rate: rate as f32,
        size: args.fft,
        window: args.window,
        overlap: args.overlap,
        averaging: args.average,
    });

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
            // Pick out the channel being analysed. A transfer function will need
            // two of these kept sample-aligned, which is exactly why the ring
            // carries interleaved frames rather than one queue per channel.
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
        // Never silent. A spectrum computed across dropped audio is wrong, not
        // merely noisy.
        return Err(format!(
            "{overruns} block(s) dropped - the measurement is invalid"
        ));
    }
    if analyzer.frames() == 0 {
        return Err("no complete frames were analysed".into());
    }

    let mut db = vec![0.0_f32; analyzer.bins()];
    analyzer.write_db_fs(&mut db);

    let mut out = String::with_capacity(analyzer.bins() * 24 + 640);
    let _ = writeln!(out, "# analyzer-cli spectrum");
    let _ = writeln!(out, "# sample rate: {rate} Hz");
    let _ = writeln!(out, "# source: {frames} frames, {channels} channel(s)");
    let _ = writeln!(out, "# analysed channel: {}", args.channel);
    let _ = writeln!(out, "# fft size: {}", analyzer.size());
    let _ = writeln!(
        out,
        "# window: {:?}, ENBW {:.4} Hz",
        args.window,
        analyzer.enbw_hz()
    );
    let _ = writeln!(
        out,
        "# overlap: {:.1}%, hop {} frames",
        args.overlap.fraction() * 100.0,
        analyzer.hop()
    );
    let _ = writeln!(out, "# averaging: {:?}", args.average);
    let _ = writeln!(out, "# frames averaged: {}", analyzer.frames());
    let _ = writeln!(out, "# bin spacing: {:.6} Hz", analyzer.bin_spacing_hz());
    let _ = writeln!(out, "# level reference: 0 dBFS = full-scale sine");
    let _ = writeln!(out, "# frequency_hz\tlevel_db");

    if args.peak_only {
        let peak = db
            .iter()
            .enumerate()
            .max_by(|a, b| a.1.total_cmp(b.1))
            .map(|(bin, level)| (bin, *level));
        if let Some((bin, level)) = peak {
            let _ = writeln!(out, "{:.6}\t{level:.4}", analyzer.bin_frequency(bin));
        }
        return Ok(out);
    }

    for (bin, level) in db.iter().enumerate() {
        if args.min_db.is_some_and(|floor| *level < floor) {
            continue;
        }
        let _ = writeln!(out, "{:.6}\t{level:.4}", analyzer.bin_frequency(bin));
    }

    Ok(out)
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
    }
}

fn read_wav(path: &Path) -> Result<Source, String> {
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
    let mut out: Option<PathBuf> = None;

    let mut argv = std::env::args().skip(1);
    while let Some(arg) = argv.next() {
        let mut value = || -> Result<String, String> {
            argv.next()
                .ok_or_else(|| format!("{arg} needs a value (try --help)"))
        };

        match arg.as_str() {
            "-h" | "--help" => return Ok(None),
            "--peak" => peak_only = true,
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

    let input = match (positional, sine_hz) {
        (Some(_), Some(_)) => return Err("give either a WAV path or --sine, not both".into()),
        (Some(path), None) => Input::Wav(path),
        (None, Some(hz)) => {
            if rate <= 0.0 {
                return Err("--rate must be positive".into());
            }
            Input::Sine {
                hz,
                rate,
                seconds,
                amplitude,
            }
        }
        (None, None) => return Err("no input given (try --help)".into()),
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
        out,
    }))
}

fn number<T: std::str::FromStr>(raw: &str, flag: &str) -> Result<T, String> {
    raw.parse()
        .map_err(|_| format!("{flag}: cannot parse '{raw}'"))
}
