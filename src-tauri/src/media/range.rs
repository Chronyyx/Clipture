#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct ByteRange {
    pub start: u64,
    pub end_inclusive: u64,
    pub total: u64,
}

impl ByteRange {
    pub fn len(self) -> u64 {
        self.end_inclusive - self.start + 1
    }

    pub fn content_range(self) -> String {
        format!("bytes {}-{}/{}", self.start, self.end_inclusive, self.total)
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum RangeError {
    EmptyResource,
    Invalid,
    MultipleRangesUnsupported,
    Unsatisfiable,
}

/// Resolves one RFC 7233 byte range. A missing header selects the entire file;
/// multiple ranges are rejected so the media server never builds multipart
/// responses or allocates based on hostile input.
pub fn resolve_range(header: Option<&str>, total: u64) -> Result<ByteRange, RangeError> {
    if total == 0 {
        return Err(RangeError::EmptyResource);
    }
    let Some(header) = header else {
        return Ok(ByteRange {
            start: 0,
            end_inclusive: total - 1,
            total,
        });
    };
    let value = header
        .trim()
        .strip_prefix("bytes=")
        .ok_or(RangeError::Invalid)?;
    if value.contains(',') {
        return Err(RangeError::MultipleRangesUnsupported);
    }
    let (start, end) = value.split_once('-').ok_or(RangeError::Invalid)?;
    let (start, end_inclusive) = if start.trim().is_empty() {
        let suffix: u64 = end.trim().parse().map_err(|_| RangeError::Invalid)?;
        if suffix == 0 {
            return Err(RangeError::Unsatisfiable);
        }
        (total.saturating_sub(suffix), total - 1)
    } else {
        let start: u64 = start.trim().parse().map_err(|_| RangeError::Invalid)?;
        if start >= total {
            return Err(RangeError::Unsatisfiable);
        }
        let end = if end.trim().is_empty() {
            total - 1
        } else {
            end.trim().parse().map_err(|_| RangeError::Invalid)?
        };
        (start, end.min(total - 1))
    };
    if end_inclusive < start {
        return Err(RangeError::Unsatisfiable);
    }
    Ok(ByteRange {
        start,
        end_inclusive,
        total,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn resolves_open_bounded_and_suffix_ranges() {
        assert_eq!(resolve_range(Some("bytes=2-5"), 10).unwrap().len(), 4);
        assert_eq!(
            resolve_range(Some("bytes=7-"), 10).unwrap(),
            ByteRange {
                start: 7,
                end_inclusive: 9,
                total: 10
            }
        );
        assert_eq!(resolve_range(Some("bytes=-3"), 10).unwrap().start, 7);
    }

    #[test]
    fn rejects_multiple_or_out_of_bounds_ranges() {
        assert_eq!(
            resolve_range(Some("bytes=0-1,4-5"), 10),
            Err(RangeError::MultipleRangesUnsupported)
        );
        assert_eq!(
            resolve_range(Some("bytes=10-"), 10),
            Err(RangeError::Unsatisfiable)
        );
    }
}
