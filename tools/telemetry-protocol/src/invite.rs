use base64::{engine::general_purpose::URL_SAFE_NO_PAD, Engine};
use std::fmt;

const PREFIX: &str = "ctel1";
pub const TOKEN_LENGTH: usize = 16;

/// `ctel1.<base64url(server URL)>.<base64url(token)>`: where to enroll and the
/// single-use token that allows it.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Invite {
    pub server: String,
    pub token: [u8; TOKEN_LENGTH],
}

#[derive(Debug, PartialEq, Eq)]
pub enum InviteError {
    Format,
    Server,
}

impl fmt::Display for InviteError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(match self {
            Self::Format => "That is not a Clipture telemetry invite code.",
            Self::Server => "The invite's server address must use https.",
        })
    }
}

impl std::error::Error for InviteError {}

impl Invite {
    pub fn encode(&self) -> String {
        format!(
            "{PREFIX}.{}.{}",
            URL_SAFE_NO_PAD.encode(self.server.as_bytes()),
            URL_SAFE_NO_PAD.encode(self.token)
        )
    }

    pub fn parse(code: &str) -> Result<Self, InviteError> {
        let mut parts = code.trim().split('.');
        let (Some(PREFIX), Some(server), Some(token), None) = (parts.next(), parts.next(), parts.next(), parts.next()) else {
            return Err(InviteError::Format);
        };
        let server = URL_SAFE_NO_PAD
            .decode(server)
            .ok()
            .and_then(|bytes| String::from_utf8(bytes).ok())
            .ok_or(InviteError::Format)?;
        let token: [u8; TOKEN_LENGTH] = URL_SAFE_NO_PAD
            .decode(token)
            .ok()
            .and_then(|bytes| bytes.try_into().ok())
            .ok_or(InviteError::Format)?;
        let server = normalized_server(&server)?;
        Ok(Self { server, token })
    }

    pub fn token_text(&self) -> String {
        URL_SAFE_NO_PAD.encode(self.token)
    }
}

/// `https://host[:port][/base]` without a trailing slash. Plain `http` only for
/// loopback, which development and the sandbox test use.
pub fn normalized_server(server: &str) -> Result<String, InviteError> {
    let server = server.trim().trim_end_matches('/');
    let rest = if let Some(rest) = server.strip_prefix("https://") {
        rest
    } else if let Some(rest) = server.strip_prefix("http://") {
        let host = rest.split(['/', '?', '#']).next().unwrap_or_default();
        let host = host.rsplit_once(':').filter(|(_, port)| port.chars().all(|c| c.is_ascii_digit())).map_or(host, |(name, _)| name);
        if !matches!(host, "127.0.0.1" | "localhost" | "[::1]") {
            return Err(InviteError::Server);
        }
        rest
    } else {
        return Err(InviteError::Server);
    };
    let host = rest.split(['/', '?', '#']).next().unwrap_or_default();
    if host.is_empty() || rest.contains(['?', '#', ' ', '@']) {
        return Err(InviteError::Server);
    }
    Ok(server.to_string())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn round_trips() {
        let invite = Invite { server: "https://clips.example.net/telemetry".into(), token: [7; TOKEN_LENGTH] };
        let code = invite.encode();
        assert!(code.starts_with("ctel1."));
        assert_eq!(Invite::parse(&format!("  {code}\n")).unwrap(), invite);
    }

    #[test]
    fn rejects_malformed_codes() {
        for code in ["", "ctel1", "ctel1.a.b.c", "ctel2.aHR0cHM6Ly94.AAAAAAAAAAAAAAAAAAAAAA", "ctel1.!!.AAAAAAAAAAAAAAAAAAAAAA", "ctel1.aHR0cHM6Ly94.AAAA"] {
            assert_eq!(Invite::parse(code), Err(InviteError::Format), "{code}");
        }
    }

    #[test]
    fn https_only_except_loopback() {
        assert_eq!(normalized_server("https://a.example/").unwrap(), "https://a.example");
        assert_eq!(normalized_server("http://127.0.0.1:8787").unwrap(), "http://127.0.0.1:8787");
        assert_eq!(normalized_server("http://localhost:8787/x").unwrap(), "http://localhost:8787/x");
        for bad in ["http://a.example", "http://127.0.0.1.evil.example", "ftp://a", "https://", "https://u@a.example", "https://a.example/?q"] {
            assert_eq!(normalized_server(bad), Err(InviteError::Server), "{bad}");
        }
    }
}
