//! The on-disk measurement container.
//!
//! A text header followed by raw little-endian `f64`. Both halves are chosen
//! deliberately.
//!
//! **The header is plain text**, which means `head -20 measurement.anlz` tells
//! you what a file is without any tooling, and an unknown key is ignored rather
//! than fatal — so a file written by a newer version still loads. It also means
//! zero serialisation dependencies for something that has to stay readable for
//! years.
//!
//! **The data is raw little-endian `f64`**, contiguous after a known offset,
//! which makes it memory-mappable. A long impulse response is megabytes, and a
//! format that requires parsing every value to reach the end is a format that
//! gets slow exactly when measurements get interesting.
//!
//! ```text
//! ANLZ1
//! name: Living room, left
//! sample_rate: 48000
//! kind: impulse_response
//! points: 65536
//! time_zero_samples: 1234.5
//! spl_offset_db: 134.2
//! data_offset: 214
//! ---
//! <points × 8 bytes, little-endian f64>
//! ```
//!
//! Complex data is stored interleaved as real, imaginary pairs, so `points`
//! counts complex values and the block is `points × 16` bytes.

use std::fmt::Write as _;
use std::io;

use crate::measurement::{Complex64, Measurement, MeasurementData, MeasurementId, References};

/// First line of every file. The digit is the format version.
pub const MAGIC: &str = "ANLZ1";

/// Line separating the header from the binary block.
const SEPARATOR: &str = "---";

/// Why a file could not be read.
#[derive(Debug)]
pub enum FormatError {
    /// Not one of our files.
    BadMagic(String),
    /// The header ended without a separator.
    MissingSeparator,
    /// A required key was absent.
    MissingField(&'static str),
    /// A value did not parse.
    BadValue {
        /// Which key.
        field: String,
        /// What it said.
        value: String,
    },
    /// The binary block was shorter than the header promised.
    Truncated {
        /// Bytes the header said to expect.
        expected: usize,
        /// Bytes actually present.
        found: usize,
    },
    /// An unrecognised measurement kind.
    UnknownKind(String),
    /// The underlying reader or writer failed.
    Io(io::Error),
}

impl std::fmt::Display for FormatError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            FormatError::BadMagic(found) => {
                write!(f, "not an analyzer measurement file (magic was {found:?})")
            }
            FormatError::MissingSeparator => write!(f, "header has no '{SEPARATOR}' separator"),
            FormatError::MissingField(name) => write!(f, "header is missing '{name}'"),
            FormatError::BadValue { field, value } => {
                write!(f, "cannot parse '{field}' from {value:?}")
            }
            FormatError::Truncated { expected, found } => {
                write!(
                    f,
                    "data block truncated: expected {expected} bytes, found {found}"
                )
            }
            FormatError::UnknownKind(kind) => write!(f, "unknown measurement kind {kind:?}"),
            FormatError::Io(error) => write!(f, "{error}"),
        }
    }
}

impl std::error::Error for FormatError {}

impl From<io::Error> for FormatError {
    fn from(error: io::Error) -> Self {
        FormatError::Io(error)
    }
}

/// Serialise a measurement.
pub fn write(measurement: &Measurement) -> Vec<u8> {
    let mut header = String::new();
    let _ = writeln!(header, "{MAGIC}");
    // Newlines in free text would break the line-oriented header, so they are
    // escaped rather than rejected - a user should be able to write a paragraph
    // of notes without the file becoming unreadable.
    let _ = writeln!(header, "name: {}", escape(&measurement.name));
    let _ = writeln!(header, "notes: {}", escape(&measurement.notes));
    let _ = writeln!(header, "id: {}", measurement.id.0);
    let _ = writeln!(header, "captured_at: {}", measurement.captured_at);
    let _ = writeln!(header, "sample_rate: {}", measurement.sample_rate);
    let _ = writeln!(header, "channels: {}", measurement.channels);
    let _ = writeln!(header, "kind: {}", measurement.data.kind());
    let _ = writeln!(header, "points: {}", measurement.data.len());

    match &measurement.data {
        MeasurementData::Spectrum { bin_spacing_hz, .. }
        | MeasurementData::TransferFunction { bin_spacing_hz, .. } => {
            let _ = writeln!(header, "bin_spacing_hz: {bin_spacing_hz}");
        }
        MeasurementData::ImpulseResponse {
            time_zero_samples, ..
        } => {
            let _ = writeln!(header, "time_zero_samples: {time_zero_samples}");
        }
    }

    let references = &measurement.references;
    write_optional(&mut header, "spl_offset_db", references.spl_offset_db);
    write_optional(
        &mut header,
        "full_scale_input_volts",
        references.full_scale_input_volts,
    );
    write_optional(
        &mut header,
        "full_scale_output_volts",
        references.full_scale_output_volts,
    );
    write_optional(
        &mut header,
        "reference_resistance_ohms",
        references.reference_resistance_ohms,
    );
    write_optional(
        &mut header,
        "propagation_delay_seconds",
        references.propagation_delay_seconds,
    );

    let _ = writeln!(header, "{SEPARATOR}");

    let mut out = header.into_bytes();
    match &measurement.data {
        MeasurementData::Spectrum { bins, .. } => push_complex(&mut out, bins),
        MeasurementData::TransferFunction {
            bins, coherence, ..
        } => {
            push_complex(&mut out, bins);
            push_real(&mut out, coherence);
        }
        MeasurementData::ImpulseResponse { samples, .. } => push_real(&mut out, samples),
    }
    out
}

/// Deserialise a measurement.
///
/// # Errors
///
/// Returns [`FormatError`] for a wrong magic, a missing or unparseable required
/// field, an unknown kind, or a data block shorter than the header promised.
pub fn read(bytes: &[u8]) -> Result<Measurement, FormatError> {
    let separator = find_separator(bytes).ok_or(FormatError::MissingSeparator)?;
    let header_text = String::from_utf8_lossy(bytes.get(..separator.0).unwrap_or(&[]));
    let data = bytes.get(separator.1..).unwrap_or(&[]);

    let mut lines = header_text.lines();
    let magic = lines.next().unwrap_or("").trim();
    if magic != MAGIC {
        return Err(FormatError::BadMagic(magic.to_owned()));
    }

    let mut fields: Vec<(String, String)> = Vec::new();
    for line in lines {
        let Some((key, value)) = line.split_once(':') else {
            // Unknown shapes are skipped rather than fatal: a file from a newer
            // version should still load whatever this version understands.
            continue;
        };
        fields.push((key.trim().to_owned(), value.trim().to_owned()));
    }

    let get = |name: &str| -> Option<&str> {
        fields
            .iter()
            .find(|(key, _)| key == name)
            .map(|(_, value)| value.as_str())
    };
    let number = |name: &'static str| -> Result<f64, FormatError> {
        let raw = get(name).ok_or(FormatError::MissingField(name))?;
        raw.parse().map_err(|_| FormatError::BadValue {
            field: name.to_owned(),
            value: raw.to_owned(),
        })
    };
    let optional = |name: &str| -> Option<f64> { get(name).and_then(|raw| raw.parse().ok()) };

    let kind = get("kind")
        .ok_or(FormatError::MissingField("kind"))?
        .to_owned();
    let points = number("points")? as usize;
    let sample_rate = number("sample_rate")?;

    let payload = match kind.as_str() {
        "spectrum" => MeasurementData::Spectrum {
            bins: read_complex(data, points)?,
            bin_spacing_hz: number("bin_spacing_hz")?,
        },
        "impulse_response" => MeasurementData::ImpulseResponse {
            samples: read_real(data, points, 0)?,
            time_zero_samples: number("time_zero_samples")?,
        },
        "transfer_function" => MeasurementData::TransferFunction {
            bins: read_complex(data, points)?,
            coherence: read_real(data, points, points * 16)?,
            bin_spacing_hz: number("bin_spacing_hz")?,
        },
        other => return Err(FormatError::UnknownKind(other.to_owned())),
    };

    Ok(Measurement {
        id: MeasurementId(optional("id").unwrap_or(0.0) as u64),
        name: unescape(get("name").unwrap_or("")),
        notes: unescape(get("notes").unwrap_or("")),
        captured_at: optional("captured_at").unwrap_or(0.0) as i64,
        sample_rate,
        channels: optional("channels").unwrap_or(1.0) as usize,
        references: References {
            spl_offset_db: optional("spl_offset_db"),
            full_scale_input_volts: optional("full_scale_input_volts"),
            full_scale_output_volts: optional("full_scale_output_volts"),
            reference_resistance_ohms: optional("reference_resistance_ohms"),
            propagation_delay_seconds: optional("propagation_delay_seconds"),
        },
        data: payload,
    })
}

fn write_optional(header: &mut String, name: &str, value: Option<f64>) {
    if let Some(value) = value {
        let _ = writeln!(header, "{name}: {value}");
    }
    // Absent means unknown. Writing a placeholder would turn "not measured"
    // into "measured as zero" on the next read.
}

/// Byte range of the separator line: where the header ends and data begins.
fn find_separator(bytes: &[u8]) -> Option<(usize, usize)> {
    let needle = b"\n---\n";
    bytes
        .windows(needle.len())
        .position(|window| window == needle)
        .map(|index| (index + 1, index + needle.len()))
}

fn push_real(out: &mut Vec<u8>, values: &[f64]) {
    out.reserve(values.len() * 8);
    for value in values {
        out.extend_from_slice(&value.to_le_bytes());
    }
}

fn push_complex(out: &mut Vec<u8>, values: &[Complex64]) {
    out.reserve(values.len() * 16);
    for value in values {
        out.extend_from_slice(&value.re.to_le_bytes());
        out.extend_from_slice(&value.im.to_le_bytes());
    }
}

fn read_real(data: &[u8], count: usize, offset: usize) -> Result<Vec<f64>, FormatError> {
    let needed = offset + count * 8;
    if data.len() < needed {
        return Err(FormatError::Truncated {
            expected: needed,
            found: data.len(),
        });
    }
    Ok(data
        .get(offset..needed)
        .unwrap_or(&[])
        .chunks_exact(8)
        .filter_map(|chunk| chunk.try_into().ok())
        .map(f64::from_le_bytes)
        .collect())
}

fn read_complex(data: &[u8], count: usize) -> Result<Vec<Complex64>, FormatError> {
    let needed = count * 16;
    if data.len() < needed {
        return Err(FormatError::Truncated {
            expected: needed,
            found: data.len(),
        });
    }
    Ok(data
        .get(..needed)
        .unwrap_or(&[])
        .chunks_exact(16)
        .filter_map(|chunk| {
            let re = chunk.get(..8)?.try_into().ok().map(f64::from_le_bytes)?;
            let im = chunk.get(8..)?.try_into().ok().map(f64::from_le_bytes)?;
            Some(Complex64::new(re, im))
        })
        .collect())
}

fn escape(text: &str) -> String {
    text.replace('\\', "\\\\").replace('\n', "\\n")
}

fn unescape(text: &str) -> String {
    let mut out = String::with_capacity(text.len());
    let mut chars = text.chars();
    while let Some(c) = chars.next() {
        if c != '\\' {
            out.push(c);
            continue;
        }
        match chars.next() {
            Some('n') => out.push('\n'),
            Some('\\') => out.push('\\'),
            Some(other) => {
                out.push('\\');
                out.push(other);
            }
            None => out.push('\\'),
        }
    }
    out
}

#[cfg(test)]
#[allow(clippy::indexing_slicing, clippy::unwrap_used)]
mod tests {
    use super::*;

    fn spectrum_measurement() -> Measurement {
        let mut m = Measurement::new(
            MeasurementId(7),
            "Living room, left",
            48_000.0,
            MeasurementData::Spectrum {
                bins: (0..64)
                    .map(|k| Complex64::new(k as f64 * 0.1, -(k as f64) * 0.05))
                    .collect(),
                bin_spacing_hz: 11.71875,
            },
        );
        m.notes = "Mic at listening position".into();
        m.captured_at = 1_753_800_000;
        m.channels = 2;
        m.references = References {
            spl_offset_db: Some(134.25),
            full_scale_input_volts: Some(1.23),
            full_scale_output_volts: None,
            reference_resistance_ohms: None,
            propagation_delay_seconds: Some(0.0143),
        };
        m
    }

    #[test]
    fn a_spectrum_round_trips_exactly() {
        let original = spectrum_measurement();
        let restored = read(&write(&original)).unwrap();
        assert_eq!(restored, original);
    }

    #[test]
    fn an_impulse_response_round_trips_exactly() {
        let mut original = Measurement::new(
            MeasurementId(3),
            "IR",
            96_000.0,
            MeasurementData::ImpulseResponse {
                samples: (0..1000).map(|n| (n as f64 * 0.01).sin()).collect(),
                time_zero_samples: 412.375,
            },
        );
        original.references.propagation_delay_seconds = Some(0.0043);

        let restored = read(&write(&original)).unwrap();
        assert_eq!(restored, original);
    }

    #[test]
    fn a_transfer_function_round_trips_with_its_coherence() {
        let original = Measurement::new(
            MeasurementId(9),
            "TF",
            44_100.0,
            MeasurementData::TransferFunction {
                bins: (0..32)
                    .map(|k| Complex64::new(1.0, k as f64 * 0.01))
                    .collect(),
                coherence: (0..32).map(|k| k as f64 / 32.0).collect(),
                bin_spacing_hz: 5.38,
            },
        );
        let restored = read(&write(&original)).unwrap();
        assert_eq!(restored, original);
    }

    /// f64 storage exists so repeated round trips do not accumulate error.
    #[test]
    fn repeated_round_trips_do_not_drift() {
        let mut current = spectrum_measurement();
        for _ in 0..20 {
            current = read(&write(&current)).unwrap();
        }
        assert_eq!(current, spectrum_measurement());
    }

    /// The point of a text header: a human can identify a file without tooling.
    #[test]
    fn the_header_is_readable_text() {
        let bytes = write(&spectrum_measurement());
        let text = String::from_utf8_lossy(&bytes[..200]);
        assert!(text.starts_with("ANLZ1\n"));
        assert!(text.contains("name: Living room, left"));
        assert!(text.contains("sample_rate: 48000"));
        assert!(text.contains("kind: spectrum"));
    }

    /// A file from a newer version must still load whatever this version knows.
    #[test]
    fn unknown_header_keys_are_ignored() {
        let bytes = write(&spectrum_measurement());
        let text = String::from_utf8_lossy(&bytes).into_owned();
        let (header, rest) = text.split_once("---\n").unwrap();
        let patched = format!("{header}future_field: 42\nanother: hello\n---\n{rest}");

        let restored = read(patched.as_bytes()).unwrap();
        assert_eq!(restored.name, "Living room, left");
    }

    /// Absent must stay absent. Writing a placeholder would turn "not measured"
    /// into "measured as zero" on the next read.
    #[test]
    fn unknown_references_do_not_become_zero() {
        let m = Measurement::new(
            MeasurementId(1),
            "bare",
            48_000.0,
            MeasurementData::ImpulseResponse {
                samples: vec![1.0, 2.0],
                time_zero_samples: 0.0,
            },
        );
        assert!(m.references.is_empty());

        let restored = read(&write(&m)).unwrap();
        assert!(
            restored.references.is_empty(),
            "got {:?}",
            restored.references
        );
        assert_eq!(restored.references.spl_offset_db, None);
    }

    #[test]
    fn a_zero_offset_survives_as_zero_not_none() {
        let mut m = spectrum_measurement();
        m.references.spl_offset_db = Some(0.0);
        let restored = read(&write(&m)).unwrap();
        assert_eq!(restored.references.spl_offset_db, Some(0.0));
    }

    #[test]
    fn newlines_in_notes_survive() {
        let mut m = spectrum_measurement();
        m.notes = "line one\nline two\\with a backslash".into();
        let restored = read(&write(&m)).unwrap();
        assert_eq!(restored.notes, m.notes);
    }

    #[test]
    fn a_foreign_file_is_rejected_by_magic() {
        let result = read(b"RIFF....\n---\n");
        assert!(matches!(result, Err(FormatError::BadMagic(_))));
    }

    #[test]
    fn a_header_without_a_separator_is_rejected() {
        let result = read(b"ANLZ1\nname: x\n");
        assert!(matches!(result, Err(FormatError::MissingSeparator)));
    }

    #[test]
    fn a_truncated_data_block_is_detected() {
        let bytes = write(&spectrum_measurement());
        let short = &bytes[..bytes.len() - 100];
        let result = read(short);
        assert!(
            matches!(result, Err(FormatError::Truncated { .. })),
            "got {result:?}"
        );
    }

    #[test]
    fn a_missing_required_field_is_reported_by_name() {
        let bytes = write(&spectrum_measurement());
        let text = String::from_utf8_lossy(&bytes).into_owned();
        let stripped: String = text
            .lines()
            .filter(|line| !line.starts_with("sample_rate:"))
            .map(|line| format!("{line}\n"))
            .collect();

        match read(stripped.as_bytes()) {
            Err(FormatError::MissingField(name)) => assert_eq!(name, "sample_rate"),
            other => panic!("expected a missing field error, got {other:?}"),
        }
    }

    #[test]
    fn an_unknown_kind_is_reported() {
        let text = "ANLZ1\nkind: hologram\npoints: 0\nsample_rate: 48000\n---\n";
        assert!(matches!(
            read(text.as_bytes()),
            Err(FormatError::UnknownKind(_))
        ));
    }

    #[test]
    fn an_empty_measurement_round_trips() {
        let m = Measurement::new(
            MeasurementId(0),
            "",
            48_000.0,
            MeasurementData::Spectrum {
                bins: Vec::new(),
                bin_spacing_hz: 1.0,
            },
        );
        assert_eq!(read(&write(&m)).unwrap(), m);
    }

    /// The data block must start on a known offset and be contiguous, which is
    /// what allows a large impulse response to be memory-mapped.
    #[test]
    fn the_data_block_is_contiguous_after_the_separator() {
        let m = spectrum_measurement();
        let bytes = write(&m);
        let (_, start) = find_separator(&bytes).unwrap();
        assert_eq!(
            bytes.len() - start,
            64 * 16,
            "64 complex values, 16 bytes each"
        );
    }
}
