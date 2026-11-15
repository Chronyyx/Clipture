//! Free-text messages are the one place a path or name could slip into a
//! record (ADR 0035, "Scrubbing"). Every structured field is allowlisted
//! instead; this only runs on messages.

const MAXIMUM_CHARS: usize = 300;

pub struct Scrubber {
    /// The Windows user and computer names, replaced wherever they appear.
    names: Vec<(String, &'static str)>,
}

impl Scrubber {
    pub fn from_environment() -> Self {
        let mut names = Vec::new();
        for (variable, placeholder) in [("USERNAME", "<user>"), ("COMPUTERNAME", "<host>")] {
            if let Ok(value) = std::env::var(variable) {
                names.push((value, placeholder));
            }
        }
        Self::with_names(names)
    }

    pub fn with_names(names: Vec<(String, &'static str)>) -> Self {
        Self { names: names.into_iter().filter(|(name, _)| name.chars().count() >= 3).collect() }
    }

    pub fn scrub(&self, message: &str) -> String {
        let mut text = replace_paths(message);
        text = text.split(' ').map(scrub_word).collect::<Vec<_>>().join(" ");
        for (name, placeholder) in &self.names {
            text = replace_ignoring_case(&text, name, placeholder);
        }
        if text.chars().count() > MAXIMUM_CHARS {
            text = text.chars().take(MAXIMUM_CHARS - 1).collect::<String>() + "…";
        }
        text
    }
}

/// `C:\…`, `C:/…` and `\\server\…` up to a quote, bracket, line end, or a
/// `: ` / `, ` that ends the path in a sentence. Paths may contain spaces.
fn replace_paths(text: &str) -> String {
    let chars: Vec<char> = text.chars().collect();
    let mut out = String::with_capacity(text.len());
    let mut index = 0;
    while index < chars.len() {
        let drive = index + 2 < chars.len()
            && chars[index].is_ascii_alphabetic()
            && chars[index + 1] == ':'
            && matches!(chars[index + 2], '\\' | '/')
            && (index == 0 || !chars[index - 1].is_alphanumeric());
        let unc = index + 1 < chars.len() && chars[index] == '\\' && chars[index + 1] == '\\';
        if drive || unc {
            let mut end = index + 2;
            while end < chars.len() {
                let c = chars[end];
                let ends_sentence = matches!(c, ':' | ',' | ';') && chars.get(end + 1).is_none_or(|next| *next == ' ');
                if matches!(c, '"' | '\'' | '<' | '>' | '|' | '\n' | '\r' | ')' | ']' | '`') || ends_sentence {
                    break;
                }
                end += 1;
            }
            out.push_str("<path>");
            index = end;
        } else {
            out.push(chars[index]);
            index += 1;
        }
    }
    out
}

fn scrub_word(word: &str) -> String {
    let core = word.trim_matches(|c: char| matches!(c, '(' | ')' | '[' | ']' | ',' | ';' | '.' | '"' | '\''));
    if core.is_empty() {
        return word.to_string();
    }
    let replacement = if is_email(core) {
        Some("<email>")
    } else if is_ipv4(core) || is_ipv6(core) {
        Some("<ip>")
    } else {
        None
    };
    match replacement {
        Some(placeholder) => word.replacen(core, placeholder, 1),
        None => word.to_string(),
    }
}

fn is_email(word: &str) -> bool {
    let Some((local, domain)) = word.split_once('@') else { return false };
    !local.is_empty() && domain.contains('.') && !domain.starts_with('.') && !domain.ends_with('.')
}

fn is_ipv4(word: &str) -> bool {
    let address = word.rsplit_once(':').filter(|(_, port)| port.chars().all(|c| c.is_ascii_digit())).map_or(word, |(host, _)| host);
    let parts: Vec<_> = address.split('.').collect();
    parts.len() == 4 && parts.iter().all(|part| !part.is_empty() && part.len() <= 3 && part.chars().all(|c| c.is_ascii_digit()))
}

fn is_ipv6(word: &str) -> bool {
    let address = word.trim_start_matches('[').split(']').next().unwrap_or_default();
    address.matches(':').count() >= 2 && address.chars().all(|c| c.is_ascii_hexdigit() || c == ':') && address.chars().any(|c| c.is_ascii_hexdigit())
}

fn replace_ignoring_case(text: &str, needle: &str, replacement: &str) -> String {
    let lower_text = text.to_lowercase();
    let lower_needle = needle.to_lowercase();
    if lower_text.len() != text.len() || lower_needle.is_empty() {
        // Lowercasing changed byte offsets (rare scripts): fall back to exact matches.
        return text.replace(needle, replacement);
    }
    let mut out = String::with_capacity(text.len());
    let mut last = 0;
    for (start, _) in lower_text.match_indices(&lower_needle) {
        out.push_str(&text[last..start]);
        out.push_str(replacement);
        last = start + needle.len();
    }
    out.push_str(&text[last..]);
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    fn scrubber() -> Scrubber {
        Scrubber::with_names(vec![("aidav".into(), "<user>"), ("DESKTOP-ABC1".into(), "<host>")])
    }

    #[test]
    fn removes_paths_including_spaces() {
        let s = scrubber();
        assert_eq!(s.scrub(r"could not open C:\Users\aidav\Videos\My Clips\a.mp4: access denied"), "could not open <path>: access denied");
        assert_eq!(s.scrub(r"moved 'D:/Clips/x y.mp4' to \\nas\share\clips"), "moved '<path>' to <path>");
        assert_eq!(s.scrub("ratio 16:9 stays"), "ratio 16:9 stays");
    }

    #[test]
    fn removes_names_addresses_and_emails() {
        let s = scrubber();
        assert_eq!(s.scrub("user AIDAV on desktop-abc1 failed"), "user <user> on <host> failed");
        assert_eq!(s.scrub("connect 192.168.1.20:8787 refused (fe80::1c2b:3a)"), "connect <ip> refused (<ip>)");
        assert_eq!(s.scrub("mail someone@example.com, then"), "mail <email>, then");
        assert_eq!(s.scrub("version 1.9.0 at 12:30"), "version 1.9.0 at 12:30");
    }

    #[test]
    fn cuts_long_messages() {
        let long = "x".repeat(400);
        let scrubbed = scrubber().scrub(&long);
        assert_eq!(scrubbed.chars().count(), 300);
        assert!(scrubbed.ends_with('…'));
    }
}
