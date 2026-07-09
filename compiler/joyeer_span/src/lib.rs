//! Source positions, spans, and a minimal source map for Joyeer.
//!
//! This is the shared leaf crate that higher layers (`joyeer_ast`,
//! `joyeer_errors`, `joyeer_parse`, ...) depend on. It has no dependencies of
//! its own, mirroring `rustc_span`'s role at the bottom of the crate graph.

/// A byte offset into a source file.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Debug)]
pub struct BytePos(pub u32);

/// A half-open byte range `[lo, hi)` within a source file.
#[derive(Clone, Copy, PartialEq, Eq, Hash, Debug)]
pub struct Span {
    pub lo: BytePos,
    pub hi: BytePos,
}

impl Span {
    /// A placeholder span used where no real location is available.
    pub const DUMMY: Span = Span {
        lo: BytePos(0),
        hi: BytePos(0),
    };

    /// Builds a span from raw byte offsets.
    pub fn new(lo: u32, hi: u32) -> Span {
        Span {
            lo: BytePos(lo),
            hi: BytePos(hi),
        }
    }

    /// The smallest span covering both `self` and `other`.
    pub fn to(self, other: Span) -> Span {
        Span {
            lo: BytePos(self.lo.0.min(other.lo.0)),
            hi: BytePos(self.hi.0.max(other.hi.0)),
        }
    }

    /// The low byte offset.
    pub fn lo(self) -> u32 {
        self.lo.0
    }

    /// The high byte offset.
    pub fn hi(self) -> u32 {
        self.hi.0
    }
}

/// A single in-memory source file plus a precomputed line table.
pub struct SourceFile {
    /// Display name (usually the path the source was read from).
    pub name: String,
    /// The full source text.
    pub src: String,
    /// Byte offset of the start of each line.
    line_starts: Vec<u32>,
}

impl SourceFile {
    /// Creates a source file, precomputing line-start offsets.
    pub fn new(name: impl Into<String>, src: impl Into<String>) -> SourceFile {
        let name = name.into();
        let src = src.into();
        let mut line_starts = vec![0u32];
        for (i, byte) in src.bytes().enumerate() {
            if byte == b'\n' {
                line_starts.push(i as u32 + 1);
            }
        }
        SourceFile {
            name,
            src,
            line_starts,
        }
    }

    /// The 1-based `(line, column)` of a byte position. Columns count Unicode
    /// scalar values, not bytes.
    pub fn location(&self, pos: BytePos) -> (usize, usize) {
        let offset = pos.0;
        let line_idx = match self.line_starts.binary_search(&offset) {
            Ok(idx) => idx,
            Err(idx) => idx.saturating_sub(1),
        };
        let line_start = self.line_starts[line_idx] as usize;
        let end = (offset as usize).min(self.src.len());
        let column = self
            .src
            .get(line_start..end)
            .map(|s| s.chars().count())
            .unwrap_or(0);
        (line_idx + 1, column + 1)
    }

    /// The source text covered by `span`.
    pub fn snippet(&self, span: Span) -> &str {
        self.src
            .get(span.lo.0 as usize..span.hi.0 as usize)
            .unwrap_or("")
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn location_tracks_lines_and_columns() {
        let sf = SourceFile::new("t.joyeer", "let x = 1\nprint(x)\n");
        assert_eq!(sf.location(BytePos(0)), (1, 1));
        assert_eq!(sf.location(BytePos(4)), (1, 5));
        // First byte of the second line ("print").
        assert_eq!(sf.location(BytePos(10)), (2, 1));
    }

    #[test]
    fn snippet_and_merge() {
        let sf = SourceFile::new("t.joyeer", "abcdef");
        let a = Span::new(0, 3);
        let b = Span::new(4, 6);
        assert_eq!(sf.snippet(a), "abc");
        assert_eq!(sf.snippet(a.to(b)), "abcdef");
    }
}
