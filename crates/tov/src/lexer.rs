use crate::diag::Diagnostic;
use crate::source::{FileId, Span};

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Tok {
    Ident,
    Int,
    Float,
    /// `123n`: a bigint literal.
    BigInt,
    /// `19.99m`: a decimal literal.
    Decimal,
    Str,
    /// `` `text` `` with no substitutions.
    TemplateFull,
    /// `` `text${ ``
    TemplateHead,
    /// `}text${`
    TemplateMiddle,
    /// `` }text` ``
    TemplateTail,

    // Keywords
    Import,
    Export,
    Function,
    Const,
    Let,
    Var,
    Type,
    Interface,
    Class,
    Return,
    If,
    Else,
    While,
    Do,
    For,
    Switch,
    Case,
    Default,
    Break,
    Continue,
    True,
    False,
    Undefined,
    Null,
    New,
    This,
    Typeof,
    Instanceof,
    Void,
    Throw,
    Try,
    Catch,
    Finally,
    Enum,
    Namespace,
    Delete,
    With,
    In,
    Async,
    Await,
    Yield,
    Super,
    Extends,
    Implements,
    Declare,
    Abstract,

    // Punctuation
    LParen,
    RParen,
    LBrace,
    RBrace,
    LBracket,
    RBracket,
    Semi,
    Comma,
    Dot,
    DotDotDot,
    Question,
    QuestionDot,
    QuestionQuestion,
    QuestionQuestionEq,
    Colon,
    Arrow,
    At,
    Hash,
    Plus,
    Minus,
    Star,
    StarStar,
    Slash,
    Percent,
    PlusPlus,
    MinusMinus,
    Eq,
    EqEq,
    EqEqEq,
    Bang,
    BangEq,
    BangEqEq,
    Lt,
    /// Always a single `>`; the parser recombines `>>`, `>=`, `>>>` from adjacent tokens
    /// so that `Map<string, Array<int>>` closes cleanly.
    Gt,
    LtEq,
    LtLt,
    LtLtEq,
    Amp,
    AmpAmp,
    AmpAmpEq,
    Pipe,
    PipePipe,
    PipePipeEq,
    Caret,
    Tilde,
    PlusEq,
    MinusEq,
    StarEq,
    StarStarEq,
    SlashEq,
    PercentEq,
    AmpEq,
    PipeEq,
    CaretEq,

    Eof,
}

impl Tok {
    pub fn describe(self) -> &'static str {
        use Tok::*;
        match self {
            Ident => "identifier",
            Int | Float => "number",
            BigInt => "bigint",
            Decimal => "decimal",
            Str => "string",
            TemplateFull | TemplateHead | TemplateMiddle | TemplateTail => "template literal",
            Eof => "end of file",
            LParen => "`(`",
            RParen => "`)`",
            LBrace => "`{`",
            RBrace => "`}`",
            LBracket => "`[`",
            RBracket => "`]`",
            Semi => "`;`",
            Comma => "`,`",
            Colon => "`:`",
            Arrow => "`=>`",
            Eq => "`=`",
            Gt => "`>`",
            Lt => "`<`",
            Dot => "`.`",
            _ => "token",
        }
    }

    pub fn is_keyword(self) -> bool {
        (self as u32) >= (Tok::Import as u32) && (self as u32) <= (Tok::Abstract as u32)
    }
}

fn keyword(s: &str) -> Option<Tok> {
    use Tok::*;
    Some(match s {
        "import" => Import,
        "export" => Export,
        "function" => Function,
        "const" => Const,
        "let" => Let,
        "var" => Var,
        "type" => Type,
        "interface" => Interface,
        "class" => Class,
        "return" => Return,
        "if" => If,
        "else" => Else,
        "while" => While,
        "do" => Do,
        "for" => For,
        "switch" => Switch,
        "case" => Case,
        "default" => Default,
        "break" => Break,
        "continue" => Continue,
        "true" => True,
        "false" => False,
        "undefined" => Undefined,
        "null" => Null,
        "new" => New,
        "this" => This,
        "typeof" => Typeof,
        "instanceof" => Instanceof,
        "void" => Void,
        "throw" => Throw,
        "try" => Try,
        "catch" => Catch,
        "finally" => Finally,
        "enum" => Enum,
        "namespace" => Namespace,
        "delete" => Delete,
        "with" => With,
        "in" => In,
        "async" => Async,
        "await" => Await,
        "yield" => Yield,
        "super" => Super,
        "extends" => Extends,
        "implements" => Implements,
        "declare" => Declare,
        "abstract" => Abstract,
        _ => return None,
    })
}

#[derive(Clone, Copy, Debug)]
pub struct Token {
    pub kind: Tok,
    pub start: u32,
    pub end: u32,
    /// A line break precedes this token.
    pub nl_before: bool,
    /// Index into `Lexed::strings` for string and template tokens.
    pub val: u32,
}

pub struct Lexed {
    pub tokens: Vec<Token>,
    pub strings: Vec<String>,
}

pub fn lex(src: &str, file: FileId, diags: &mut Vec<Diagnostic>) -> Lexed {
    let mut lx = Lexer { src: src.as_bytes(), text: src, pos: 0, file, diags, tokens: Vec::new(), strings: Vec::new(), braces: Vec::new() };
    lx.run();
    Lexed { tokens: lx.tokens, strings: lx.strings }
}

struct Lexer<'a> {
    src: &'a [u8],
    text: &'a str,
    pos: usize,
    file: FileId,
    diags: &'a mut Vec<Diagnostic>,
    tokens: Vec<Token>,
    strings: Vec<String>,
    /// For each open `{`: true if it opened a template substitution `${`.
    braces: Vec<bool>,
}

impl Lexer<'_> {
    fn peek(&self, n: usize) -> u8 {
        *self.src.get(self.pos + n).unwrap_or(&0)
    }

    fn span(&self, start: usize, end: usize) -> Span {
        Span::new(self.file, start as u32, end as u32)
    }

    fn err(&mut self, code: &'static str, start: usize, end: usize, msg: impl Into<String>) {
        let span = self.span(start, end);
        self.diags.push(Diagnostic::new(code, span, msg));
    }

    fn push(&mut self, kind: Tok, start: usize, nl: bool, val: u32) {
        self.tokens.push(Token { kind, start: start as u32, end: self.pos as u32, nl_before: nl, val });
    }

    fn run(&mut self) {
        let mut nl = false;
        loop {
            // Skip whitespace and comments.
            loop {
                match self.peek(0) {
                    b'\n' => {
                        nl = true;
                        self.pos += 1;
                    }
                    b' ' | b'\t' | b'\r' => self.pos += 1,
                    b'/' if self.peek(1) == b'/' => {
                        while self.pos < self.src.len() && self.src[self.pos] != b'\n' {
                            self.pos += 1;
                        }
                    }
                    b'/' if self.peek(1) == b'*' => {
                        let start = self.pos;
                        self.pos += 2;
                        loop {
                            if self.pos >= self.src.len() {
                                self.err("L0003", start, start + 2, "unterminated block comment");
                                break;
                            }
                            if self.src[self.pos] == b'*' && self.peek(1) == b'/' {
                                self.pos += 2;
                                break;
                            }
                            if self.src[self.pos] == b'\n' {
                                nl = true;
                            }
                            self.pos += 1;
                        }
                    }
                    0xEF if self.peek(1) == 0xBB && self.peek(2) == 0xBF => self.pos += 3,
                    _ => break,
                }
            }
            let start = self.pos;
            if self.pos >= self.src.len() {
                self.push(Tok::Eof, start, true, 0);
                return;
            }
            let c = self.src[self.pos];
            if c.is_ascii_alphabetic() || c == b'_' || c == b'$' {
                while self.pos < self.src.len() && (self.src[self.pos].is_ascii_alphanumeric() || self.src[self.pos] == b'_' || self.src[self.pos] == b'$') {
                    self.pos += 1;
                }
                let word = &self.text[start..self.pos];
                let kind = keyword(word).unwrap_or(Tok::Ident);
                self.push(kind, start, nl, 0);
            } else if c.is_ascii_digit() || (c == b'.' && self.peek(1).is_ascii_digit()) {
                self.number(start, nl);
            } else if c == b'"' || c == b'\'' {
                self.string(start, nl, c);
            } else if c == b'`' {
                self.pos += 1;
                self.template(start, nl, true);
            } else if c >= 0x80 {
                let ch = self.text[start..].chars().next().unwrap();
                self.pos += ch.len_utf8();
                self.err("L0001", start, self.pos, format!("unexpected character `{ch}`"));
                nl = false;
                continue;
            } else {
                self.punct(start, nl);
            }
            nl = false;
        }
    }

    fn number(&mut self, start: usize, nl: bool) {
        let mut float = false;
        if self.peek(0) == b'0' && matches!(self.peek(1), b'x' | b'X' | b'b' | b'B' | b'o' | b'O') {
            self.pos += 2;
            while self.peek(0).is_ascii_hexdigit() || self.peek(0) == b'_' {
                self.pos += 1;
            }
        } else {
            while self.peek(0).is_ascii_digit() || self.peek(0) == b'_' {
                self.pos += 1;
            }
            if self.peek(0) == b'.' && self.peek(1).is_ascii_digit() {
                float = true;
                self.pos += 1;
                while self.peek(0).is_ascii_digit() || self.peek(0) == b'_' {
                    self.pos += 1;
                }
            } else if self.peek(0) == b'.' && !self.peek(1).is_ascii_alphabetic() && self.peek(1) != b'.' {
                // `1.` is a float literal as in JS.
                float = true;
                self.pos += 1;
            }
            if matches!(self.peek(0), b'e' | b'E') && (self.peek(1).is_ascii_digit() || (matches!(self.peek(1), b'+' | b'-') && self.peek(2).is_ascii_digit())) {
                float = true;
                self.pos += 2;
                while self.peek(0).is_ascii_digit() {
                    self.pos += 1;
                }
            }
        }
        let hex = self.text.as_bytes()[start..self.pos].starts_with(b"0x") || self.text.as_bytes()[start..self.pos].starts_with(b"0X");
        if self.peek(0) == b'm' && !hex && !self.peek(1).is_ascii_alphanumeric() && self.peek(1) != b'_' {
            self.pos += 1;
            self.push(Tok::Decimal, start, nl, 0);
            return;
        }
        if self.peek(0) == b'n' {
            self.pos += 1;
            if float {
                self.err("L0006", start, self.pos, "a bigint literal must be an integer");
            }
            self.push(Tok::BigInt, start, nl, 0);
            return;
        }
        self.push(if float { Tok::Float } else { Tok::Int }, start, nl, 0);
    }

    /// Reads one escape sequence after `\`; returns the decoded char.
    fn escape(&mut self) -> Option<char> {
        let esc_start = self.pos - 1;
        let c = self.peek(0);
        self.pos += 1;
        Some(match c {
            b'n' => '\n',
            b't' => '\t',
            b'r' => '\r',
            b'b' => '\u{8}',
            b'f' => '\u{c}',
            b'v' => '\u{b}',
            b'0' => '\0',
            b'\\' => '\\',
            b'\'' => '\'',
            b'"' => '"',
            b'`' => '`',
            b'$' => '$',
            b'\n' => return None,
            b'x' => {
                let hex = self.text.get(self.pos..self.pos + 2).unwrap_or("");
                match u32::from_str_radix(hex, 16) {
                    Ok(v) if hex.len() == 2 => {
                        self.pos += 2;
                        char::from_u32(v).unwrap()
                    }
                    _ => {
                        self.err("L0004", esc_start, self.pos, "invalid `\\x` escape; expected two hex digits");
                        '\u{fffd}'
                    }
                }
            }
            b'u' => {
                let (digits, len) = if self.peek(0) == b'{' {
                    let end = self.text[self.pos..].find('}').map(|i| self.pos + i);
                    match end {
                        Some(end) => (&self.text[self.pos + 1..end], end + 1 - self.pos),
                        None => ("", 0),
                    }
                } else {
                    (self.text.get(self.pos..self.pos + 4).unwrap_or(""), 4)
                };
                match u32::from_str_radix(digits, 16).ok().and_then(char::from_u32) {
                    Some(ch) if !digits.is_empty() => {
                        self.pos += len;
                        ch
                    }
                    _ => {
                        self.err("L0004", esc_start, self.pos, "invalid `\\u` escape");
                        '\u{fffd}'
                    }
                }
            }
            _ => {
                let ch = self.text[self.pos - 1..].chars().next().unwrap_or('?');
                self.pos += ch.len_utf8() - 1;
                ch
            }
        })
    }

    fn string(&mut self, start: usize, nl: bool, quote: u8) {
        self.pos += 1;
        let mut value = String::new();
        loop {
            if self.pos >= self.src.len() || self.src[self.pos] == b'\n' {
                self.err("L0002", start, self.pos, "unterminated string literal");
                break;
            }
            let c = self.src[self.pos];
            if c == quote {
                self.pos += 1;
                break;
            }
            if c == b'\\' {
                self.pos += 1;
                if let Some(ch) = self.escape() {
                    value.push(ch);
                }
                continue;
            }
            let ch = self.text[self.pos..].chars().next().unwrap();
            value.push(ch);
            self.pos += ch.len_utf8();
        }
        self.strings.push(value);
        self.push(Tok::Str, start, nl, self.strings.len() as u32 - 1);
    }

    /// Scans template text after a `` ` `` or a closing `}` of a substitution.
    fn template(&mut self, start: usize, nl: bool, head: bool) {
        let mut value = String::new();
        loop {
            if self.pos >= self.src.len() {
                self.err("L0002", start, self.pos, "unterminated template literal");
                self.strings.push(value);
                let kind = if head { Tok::TemplateFull } else { Tok::TemplateTail };
                self.push(kind, start, nl, self.strings.len() as u32 - 1);
                return;
            }
            let c = self.src[self.pos];
            if c == b'`' {
                self.pos += 1;
                self.strings.push(value);
                let kind = if head { Tok::TemplateFull } else { Tok::TemplateTail };
                self.push(kind, start, nl, self.strings.len() as u32 - 1);
                return;
            }
            if c == b'$' && self.peek(1) == b'{' {
                self.pos += 2;
                self.braces.push(true);
                self.strings.push(value);
                let kind = if head { Tok::TemplateHead } else { Tok::TemplateMiddle };
                self.push(kind, start, nl, self.strings.len() as u32 - 1);
                return;
            }
            if c == b'\\' {
                self.pos += 1;
                if let Some(ch) = self.escape() {
                    value.push(ch);
                }
                continue;
            }
            let ch = self.text[self.pos..].chars().next().unwrap();
            value.push(ch);
            self.pos += ch.len_utf8();
        }
    }

    fn punct(&mut self, start: usize, nl: bool) {
        use Tok::*;
        let c = self.src[self.pos];
        let (kind, len) = match (c, self.peek(1), self.peek(2)) {
            (b'.', b'.', b'.') => (DotDotDot, 3),
            (b'?', b'?', b'=') => (QuestionQuestionEq, 3),
            (b'=', b'=', b'=') => (EqEqEq, 3),
            (b'!', b'=', b'=') => (BangEqEq, 3),
            (b'*', b'*', b'=') => (StarStarEq, 3),
            (b'<', b'<', b'=') => (LtLtEq, 3),
            (b'&', b'&', b'=') => (AmpAmpEq, 3),
            (b'|', b'|', b'=') => (PipePipeEq, 3),
            (b'?', b'.', d) if !d.is_ascii_digit() => (QuestionDot, 2),
            (b'?', b'?', _) => (QuestionQuestion, 2),
            (b'=', b'>', _) => (Arrow, 2),
            (b'=', b'=', _) => (EqEq, 2),
            (b'!', b'=', _) => (BangEq, 2),
            (b'*', b'*', _) => (StarStar, 2),
            (b'+', b'+', _) => (PlusPlus, 2),
            (b'-', b'-', _) => (MinusMinus, 2),
            (b'<', b'=', _) => (LtEq, 2),
            (b'<', b'<', _) => (LtLt, 2),
            (b'&', b'&', _) => (AmpAmp, 2),
            (b'|', b'|', _) => (PipePipe, 2),
            (b'+', b'=', _) => (PlusEq, 2),
            (b'-', b'=', _) => (MinusEq, 2),
            (b'*', b'=', _) => (StarEq, 2),
            (b'/', b'=', _) => (SlashEq, 2),
            (b'%', b'=', _) => (PercentEq, 2),
            (b'&', b'=', _) => (AmpEq, 2),
            (b'|', b'=', _) => (PipeEq, 2),
            (b'^', b'=', _) => (CaretEq, 2),
            (b'(', _, _) => (LParen, 1),
            (b')', _, _) => (RParen, 1),
            (b'{', _, _) => {
                self.braces.push(false);
                (LBrace, 1)
            }
            (b'}', _, _) => {
                if self.braces.pop() == Some(true) {
                    self.pos += 1;
                    self.template(start, nl, false);
                    return;
                }
                (RBrace, 1)
            }
            (b'[', _, _) => (LBracket, 1),
            (b']', _, _) => (RBracket, 1),
            (b';', _, _) => (Semi, 1),
            (b',', _, _) => (Comma, 1),
            (b'.', _, _) => (Dot, 1),
            (b'?', _, _) => (Question, 1),
            (b':', _, _) => (Colon, 1),
            (b'@', _, _) => (At, 1),
            (b'#', _, _) => (Hash, 1),
            (b'+', _, _) => (Plus, 1),
            (b'-', _, _) => (Minus, 1),
            (b'*', _, _) => (Star, 1),
            (b'/', _, _) => (Slash, 1),
            (b'%', _, _) => (Percent, 1),
            (b'=', _, _) => (Eq, 1),
            (b'!', _, _) => (Bang, 1),
            (b'<', _, _) => (Lt, 1),
            (b'>', _, _) => (Gt, 1),
            (b'&', _, _) => (Amp, 1),
            (b'|', _, _) => (Pipe, 1),
            (b'^', _, _) => (Caret, 1),
            (b'~', _, _) => (Tilde, 1),
            _ => {
                self.pos += 1;
                let ch = c as char;
                self.err("L0001", start, self.pos, format!("unexpected character `{ch}`"));
                return;
            }
        };
        self.pos += len;
        self.push(kind, start, nl, 0);
    }
}
