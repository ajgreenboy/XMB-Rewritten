#![forbid(unsafe_code)]

#[derive(Clone, Copy, Debug)]
pub struct Options {
    pub skip_bom: bool,
    pub quoted_braces_are_structural: bool,
    pub max_token_bytes: usize,
    pub max_tokens: usize,
    pub max_entries: usize,
    pub max_depth: i32,
}

impl Default for Options {
    fn default() -> Self {
        Self {
            skip_bom: false,
            quoted_braces_are_structural: false,
            max_token_bytes: 4096,
            max_tokens: 20000,
            max_entries: 4096,
            max_depth: 8,
        }
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Entry {
    pub key: String,
    pub value: String,
    pub children: Vec<Entry>,
    pub block: bool,
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum TokenKind {
    Word,
    BraceOpen,
    BraceClose,
}

struct Token {
    text: String,
    kind: TokenKind,
}

#[inline]
fn is_space(c: u8) -> bool {
    matches!(c, b' ' | b'\t' | b'\r' | b'\n' | 0x0b | 0x0c)
}

pub fn parse(input: &[u8], options: &Options) -> Result<Vec<Entry>, String> {
    let tokens = tokenize(input, options)?;
    let mut pos = 0usize;
    let mut entries_used = 0usize;
    let mut entries = Vec::new();
    parse_entries(&tokens, &mut pos, 0, options, &mut entries, &mut entries_used)?;
    if pos != tokens.len() {
        return Err("unexpected trailing KeyValues data".to_string());
    }
    Ok(entries)
}

fn tokenize(input: &[u8], options: &Options) -> Result<Vec<Token>, String> {
    let mut tokens = Vec::new();
    let mut i = 0usize;
    if options.skip_bom && input.len() >= 3 && input[0] == 0xef && input[1] == 0xbb && input[2] == 0xbf {
        i = 3;
    }
    while i < input.len() {
        let c = input[i];
        if is_space(c) {
            i += 1;
            continue;
        }
        if c == b'/' && i + 1 < input.len() && input[i + 1] == b'/' {
            i += 2;
            while i < input.len() && input[i] != b'\n' {
                i += 1;
            }
            continue;
        }
        if c == b'{' || c == b'}' {
            tokens.push(Token {
                text: String::new(),
                kind: if c == b'{' { TokenKind::BraceOpen } else { TokenKind::BraceClose },
            });
            i += 1;
        } else if c == b'"' {
            i += 1;
            let mut raw = Vec::new();
            let mut closed = false;
            while i < input.len() {
                let current = input[i];
                i += 1;
                if current == b'"' {
                    closed = true;
                    break;
                }
                if current == b'\\' && i < input.len() {
                    let next = input[i];
                    if next == b'"' || next == b'\\' {
                        raw.push(next);
                        i += 1;
                    } else {
                        raw.push(current);
                    }
                } else {
                    raw.push(current);
                }
                if raw.len() > options.max_token_bytes {
                    return Err("KeyValues token exceeds the limit".to_string());
                }
            }
            if !closed {
                return Err("unterminated KeyValues string".to_string());
            }
            let text = String::from_utf8_lossy(&raw).into_owned();
            let mut kind = TokenKind::Word;
            if options.quoted_braces_are_structural {
                if text == "{" {
                    kind = TokenKind::BraceOpen;
                } else if text == "}" {
                    kind = TokenKind::BraceClose;
                }
            }
            tokens.push(Token { text, kind });
        } else {
            let start = i;
            while i < input.len() {
                let current = input[i];
                if is_space(current) || current == b'{' || current == b'}' {
                    break;
                }
                if current == b'/' && i + 1 < input.len() && input[i + 1] == b'/' {
                    break;
                }
                i += 1;
            }
            if start == i {
                return Err("unsupported character in KeyValues file".to_string());
            }
            let raw = &input[start..i];
            if raw.len() > options.max_token_bytes {
                return Err("KeyValues token exceeds the limit".to_string());
            }
            tokens.push(Token {
                text: String::from_utf8_lossy(raw).into_owned(),
                kind: TokenKind::Word,
            });
        }
        if tokens.len() > options.max_tokens {
            return Err("KeyValues token count exceeds the limit".to_string());
        }
    }
    Ok(tokens)
}

fn parse_entries(
    tokens: &[Token],
    pos: &mut usize,
    depth: i32,
    options: &Options,
    entries: &mut Vec<Entry>,
    entries_used: &mut usize,
) -> Result<(), String> {
    while *pos < tokens.len() {
        let token = &tokens[*pos];
        if token.kind == TokenKind::BraceClose {
            if depth == 0 {
                return Err("unexpected closing brace".to_string());
            }
            return Ok(());
        }
        if token.kind == TokenKind::BraceOpen {
            return Err("missing KeyValues key".to_string());
        }
        let key = token.text.clone();
        *pos += 1;
        if *pos >= tokens.len() {
            return Err("missing KeyValues value".to_string());
        }
        let value = &tokens[*pos];
        if value.kind == TokenKind::BraceOpen {
            *pos += 1;
            if depth + 1 > options.max_depth {
                return Err("KeyValues nesting exceeds the limit".to_string());
            }
            let mut children = Vec::new();
            parse_entries(tokens, pos, depth + 1, options, &mut children, entries_used)?;
            if *pos >= tokens.len() || tokens[*pos].kind != TokenKind::BraceClose {
                return Err("unterminated KeyValues block".to_string());
            }
            *pos += 1;
            *entries_used += 1;
            if *entries_used > options.max_entries {
                return Err("KeyValues entry count exceeds the limit".to_string());
            }
            entries.push(Entry {
                key,
                value: String::new(),
                children,
                block: true,
            });
        } else if value.kind == TokenKind::Word {
            let val_text = value.text.clone();
            *pos += 1;
            *entries_used += 1;
            if *entries_used > options.max_entries {
                return Err("KeyValues entry count exceeds the limit".to_string());
            }
            entries.push(Entry {
                key,
                value: val_text,
                children: Vec::new(),
                block: false,
            });
        } else {
            return Err("missing KeyValues value".to_string());
        }
    }
    if depth != 0 {
        return Err("unterminated KeyValues block".to_string());
    }
    Ok(())
}

pub fn encode_entries(entries: &[Entry]) -> Vec<u8> {
    let mut out = Vec::with_capacity(16 + entries.len() * 24);
    out.extend_from_slice(&(entries.len() as u32).to_le_bytes());
    for e in entries {
        encode_entry(e, &mut out);
    }
    out
}

fn encode_entry(e: &Entry, out: &mut Vec<u8>) {
    out.push(u8::from(e.block));
    let kb = e.key.as_bytes();
    out.extend_from_slice(&(kb.len() as u32).to_le_bytes());
    out.extend_from_slice(kb);
    if e.block {
        out.extend_from_slice(&(e.children.len() as u32).to_le_bytes());
        for c in &e.children {
            encode_entry(c, out);
        }
    } else {
        let vb = e.value.as_bytes();
        out.extend_from_slice(&(vb.len() as u32).to_le_bytes());
        out.extend_from_slice(vb);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_nested_vdf_and_escapes() {
        let src = br#"// comment
"users" {
    "76561198000000001" {
        "AccountName" "a\"b\\c\nd"
        MostRecent 1//inline
    }
}"#;
        let doc = parse(src, &Options::default()).expect("valid vdf");
        assert_eq!(doc.len(), 1);
        assert_eq!(doc[0].key, "users");
        assert!(doc[0].block);
        let u = &doc[0].children[0];
        assert_eq!(u.children[0].value, "a\"b\\c\\nd");
        assert_eq!(u.children[1].value, "1");
    }

    #[test]
    fn enforces_depth_and_token_limits() {
        let mut opts = Options::default();
        opts.max_depth = 1;
        let deep = br#""a" { "b" { "k" "v" } }"#;
        assert_eq!(
            parse(deep, &opts).unwrap_err(),
            "KeyValues nesting exceeds the limit"
        );
    }
}
