//! Invite links. An invite carries the full friend code (the public key), so
//! it is exactly as trustworthy as a pasted code. The name is only a
//! suggestion from whoever made the link: it is cleaned, and adding a friend
//! from a link always needs the user's confirmation.
//!
//! Forms accepted:
//! - `https://clipture.app/invite/#c=<code>&n=<name>`, which is
//!   clickable in chat apps. The fragment never reaches the web server.
//! - `clipture://add/<code>?name=<name>`, opened by Windows for the app.
//! - A bare friend code.
use url::Url;

use super::{
    node::{friend_code, parse_friend_code},
    wire,
};

/// Static handoff page served by Cloudflare Pages from `web/invite/`.
pub const INVITE_PAGE: &str = "https://clipture.app/invite/";
pub const SCHEME: &str = "clipture";
const MAX_LINK_BYTES: usize = 512;

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Invite {
    /// Normalized friend code.
    pub code: String,
    /// Suggested name, already cleaned; may be empty.
    pub name: String,
}

pub fn parse_invite(input: &str) -> Option<Invite> {
    let input = input.trim();
    if input.is_empty() || input.len() > MAX_LINK_BYTES {
        return None;
    }
    let (code, name) = if input.contains("://") {
        let url = Url::parse(input).ok()?;
        match url.scheme() {
            SCHEME => {
                if url.host_str() != Some("add") {
                    return None;
                }
                let code = url.path().trim_matches('/').to_owned();
                let name = pair(url.query_pairs(), "name");
                (code, name)
            }
            "https" => {
                let fragment = url.fragment()?;
                let pairs = url::form_urlencoded::parse(fragment.as_bytes());
                let code = pair(pairs, "c");
                let name = pair(pairs, "n");
                (code, name)
            }
            _ => return None,
        }
    } else {
        (input.to_owned(), String::new())
    };
    let id = parse_friend_code(&code).ok()?;
    Some(Invite {
        code: friend_code(&id),
        name: wire::clean_name(&name),
    })
}

/// The first launch argument that is an app invite link, if any.
pub fn invite_argument(arguments: &[String]) -> Option<Invite> {
    arguments
        .iter()
        .filter(|argument| argument.len() <= MAX_LINK_BYTES)
        .find(|argument| argument.starts_with("clipture:"))
        .and_then(|argument| parse_invite(argument))
}

/// A clickable link for chat apps.
pub fn invite_link(code: &str, name: &str) -> String {
    let mut fragment = url::form_urlencoded::Serializer::new(String::new());
    fragment.append_pair("c", code);
    if !name.is_empty() {
        fragment.append_pair("n", name);
    }
    format!("{INVITE_PAGE}#{}", fragment.finish())
}

fn pair<'a>(
    pairs: impl Iterator<Item = (std::borrow::Cow<'a, str>, std::borrow::Cow<'a, str>)>,
    key: &str,
) -> String {
    pairs
        .filter(|(name, _)| name == key)
        .map(|(_, value)| value.into_owned())
        .next()
        .unwrap_or_default()
}

#[cfg(test)]
mod tests {
    use super::*;
    use iroh::SecretKey;

    fn code() -> String {
        friend_code(&SecretKey::generate().public())
    }

    #[test]
    fn every_form_yields_the_same_friend() {
        let code = code();
        let link = invite_link(&code, "Alex Rivera");
        assert!(link.starts_with(INVITE_PAGE));
        let expected = Invite {
            code: code.clone(),
            name: "Alex Rivera".into(),
        };
        assert_eq!(parse_invite(&link), Some(expected.clone()));
        let app = format!("clipture://add/{code}?name=Alex%20Rivera");
        assert_eq!(parse_invite(&app), Some(expected));
        assert_eq!(parse_invite(&code).unwrap().code, code);
    }

    #[test]
    fn hostile_links_are_rejected_or_cleaned() {
        let code = code();
        assert!(parse_invite("clipture://delete/everything").is_none());
        assert!(parse_invite(&format!("clipture://remove/{code}")).is_none());
        assert!(parse_invite("https://example.com/#c=not-a-key").is_none());
        assert!(parse_invite(&format!("file:///C:/{code}")).is_none());
        assert!(parse_invite(&"a".repeat(MAX_LINK_BYTES + 1)).is_none());
        let noisy = format!("clipture://add/{code}?name=%07Bell%0A{}", "x".repeat(200));
        let invite = parse_invite(&noisy).unwrap();
        assert!(!invite.name.contains('\u{7}') && !invite.name.contains('\n'));
        assert!(invite.name.chars().count() <= 40);
    }

    #[test]
    fn codes_match_the_invite_page_check() {
        // web/invite/invite.js accepts exactly 52 z-base-32 characters.
        let page = include_str!("../../../web/invite/invite.js");
        assert!(page.contains("[ybndrfg8ejkmcpqxot1uwisza345h769]{52}"));
        for _ in 0..32 {
            let code = code();
            assert_eq!(code.len(), 52);
            assert!(code
                .chars()
                .all(|c| "ybndrfg8ejkmcpqxot1uwisza345h769".contains(c)));
        }
        assert!(invite_link(&code(), "x").starts_with(INVITE_PAGE));
    }

    #[test]
    fn only_app_links_in_arguments_count() {
        let code = code();
        let args = vec![
            "clipture.exe".to_owned(),
            "--hidden".to_owned(),
            format!("clipture://add/{code}"),
        ];
        assert_eq!(invite_argument(&args).unwrap().code, code);
        assert!(invite_argument(&["clipture.exe".into(), code]).is_none());
    }
}
