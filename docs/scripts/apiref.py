# SPDX-License-Identifier: Apache-2.0 OR MIT
# Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
"""The API reference from the public headers (include/hdrgrading/*.h).

The headers document themselves with plain C comments: a block comment
before a declaration or a group of #defines, a comment after a struct member
or a constant. Doxygen and libclang only take doc comments (/** */, ///<),
so this parser reads the headers' own layout instead (the layout
scripts/check_style.py and clang-format keep). It fails loudly when it meets
a line it does not understand, and build.py checks that every HDRG_API
function of the headers is in the reference.

The Japanese reference shows each comment in Japanese with the English below
it. The translations are kept in docs/scripts/api_ja/<header>.json, one entry
per comment keyed by a hash of its English words (a rewrap keeps it): a
comment that does not change keeps its translation, a new or changed one comes in untranslated
(ja: null) and an entry no comment uses any more goes (docs/scripts/api_ja/
README.md).
"""

from __future__ import annotations

import argparse
import hashlib
import html
import json
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

HEADERS = ['hdrgrading.h', 'video.h', 'gpu.h', 'metadata.h', 'sidedata.h', 'plugin.h', 'filter.h',
           'codec.h']
JA_DIR = Path(__file__).resolve().parent / 'api_ja'

IDENT = re.compile(r'\b(?:hdrg|HDRG)_[A-Za-z0-9_]*(?:\.[a-z_][a-z0-9_]*)?\*?')


@dataclass
class Member:
    names: list
    decl: str
    doc: list = field(default_factory=list)  # comment lines
    section: str = ''  # a section header row before it
    section_doc: list = field(default_factory=list)
    variants: list = field(default_factory=list)  # a conditional macro's values


@dataclass
class Item:
    kind: str  # function, struct, enum, typedef, fnptr, opaque, macros, macrofn, prose
    names: list
    decl: str = ''
    doc: list = field(default_factory=list)
    members: list = field(default_factory=list)
    line: int = 0


@dataclass
class Header:
    name: str
    doc: list
    items: list


class ParseError(Exception):
    pass


def _comment_lines(text: str) -> list:
    """The lines of a block comment's text, without the markers and the * gutter."""
    body = text.strip()
    if body.startswith('/*'):
        body = body[2:]
    if body.endswith('*/'):
        body = body[:-2]

    lines = body.split('\n')
    out = []
    for i, line in enumerate(lines):
        stripped = line.strip()
        if stripped.startswith('* ') or stripped == '*':
            stripped = line.lstrip()[2:] if stripped != '*' else ''
            out.append(stripped.rstrip())
        elif i == 0:
            out.append(line.strip())
        else:
            out.append(line.rstrip())

    while out and not out[0].strip():
        out.pop(0)
    while out and not out[-1].strip():
        out.pop()

    return out


def _trailing_lines(text: str) -> list:
    """A trailing comment's lines; continuation lines keep their relative indent."""
    body = text.strip()
    body = body[2:] if body.startswith('/*') else body
    body = body[:-2] if body.endswith('*/') else body
    lines = body.split('\n')
    first = lines[0].strip()
    rest = [line.strip() for line in lines[1:]]

    return [t for t in [first] + rest if t] or ['']


def _split_trailing(code: str):
    """code -> (code before a trailing comment, the comment or '')."""
    depth = 0
    i = 0
    in_str = False
    while i < len(code):
        ch = code[i]
        if ch == '"' and (i == 0 or code[i - 1] != '\\'):
            in_str = not in_str
        if not in_str and code.startswith('/*', i):
            return code[:i].rstrip(), code[i:]
        if ch == '(':
            depth += 1
        elif ch == ')':
            depth -= 1
        i += 1

    return code.rstrip(), ''


def _norm(code: str) -> str:
    code = re.sub(r'\s+', ' ', code).strip()
    code = re.sub(r'\(\s+', '(', code)
    code = re.sub(r'\s+\)', ')', code)
    code = re.sub(r'\s+,', ',', code)

    return code


def _declarator_names(decl: str) -> list:
    """Names declared by a member or a function (pointer) declaration."""
    decl = _norm(decl).rstrip(';')
    m = re.search(r'\(\s*(?:HDRG_PLUGIN_CALL\s*)?\*\s*([A-Za-z_]\w*)\s*\)\s*\(', decl)
    if m:
        return [m.group(1)]

    m = re.match(r'.*?\b([A-Za-z_]\w*)\s*\(', decl)
    if m and not decl.startswith('typedef'):
        # a function: the identifier before the first parenthesis of the parameter list
        head = decl[:decl.index('(')]
        return [head.split()[-1].lstrip('*')]

    parts, depth, cur = [], 0, ''
    for ch in decl:
        if ch in '([':
            depth += 1
        elif ch in ')]':
            depth -= 1
        if ch == ',' and depth == 0:
            parts.append(cur)
            cur = ''
        else:
            cur += ch
    parts.append(cur)
    names = []
    for i, p in enumerate(parts):
        p = re.sub(r'\[.*?\]', '', p).strip()
        p = p.split('=')[0].strip()
        tok = re.findall(r'[A-Za-z_]\w*', p)
        if tok:
            names.append(tok[-1])

    return names


def parse_header(path: Path) -> Header:
    text = path.read_text(encoding='utf-8')
    lines = text.split('\n')
    n = len(lines)
    i = 0
    items: list = []
    file_doc: list = []
    pending: list = []
    pending_line = 0
    guard = None
    seen_spdx = False

    def flush_pending_as_prose():
        nonlocal pending
        if pending:
            items.append(Item('prose', [], doc=pending, line=pending_line))
            pending = []

    while i < n:
        raw = lines[i]
        s = raw.strip()

        if not s:
            i += 1
            continue

        if s.startswith('/*'):
            start = i
            block = raw
            while '*/' not in block:
                i += 1
                block += '\n' + lines[i]
            i += 1
            if not seen_spdx and 'SPDX-License-Identifier' in block:
                seen_spdx = True
                continue
            doc = _comment_lines(block)
            if guard is None and not file_doc:
                file_doc = doc
            else:
                flush_pending_as_prose()
                pending = doc
                pending_line = start + 1
            continue

        # joined logical line (backslash continuation)
        logical = raw
        start = i
        while logical.rstrip().endswith('\\'):
            i += 1
            logical = logical.rstrip()[:-1] + '\n' + lines[i]
        i += 1

        if s.startswith('#ifndef') and guard is None:
            guard = s.split()[1]
            continue

        if s.startswith('#include') or s.startswith('#endif') or s.startswith('#else') or \
           s.startswith('#elif') or s.startswith('#if') or s in ('extern "C" {', '}'):
            continue

        if s.startswith('#define'):
            body = logical.strip()[len('#define'):].strip()
            m = re.match(r'([A-Za-z_]\w*)(\([^)]*\))?\s*(.*)', body, re.S)
            name, args, rest = m.group(1), m.group(2), m.group(3)
            if name == guard:
                continue
            if args:
                items.append(Item('macrofn', [name], decl='#define ' + name + args + ' ...',
                                  doc=pending, line=start + 1))
                items[-1].body = rest
                pending = []
                continue
            # a trailing comment may continue on the following lines
            value, comment = _split_trailing(rest)
            while comment and '*/' not in comment:
                comment += '\n' + lines[i]
                i += 1
            entry = Member([name], _norm(value), _trailing_lines(comment) if comment else [])
            last = items[-1] if items else None
            joins = (last is not None and last.kind == 'macros' and not pending and
                     last.line_end == start)
            if joins:
                last.members.append(entry)
                last.line_end = i
            else:
                item = Item('macros', [], doc=pending, members=[entry], line=start + 1)
                item.line_end = i
                items.append(item)
                pending = []
            continue

        if s.startswith('typedef struct') and s.endswith('{') or \
           s.startswith('typedef enum') and s.endswith('{') or \
           re.match(r'struct \w+ \{$', s):
            kind = 'enum' if 'enum' in s.split('{')[0] else 'struct'
            tag = re.match(r'struct (\w+) \{$', s)
            members, i, closing = _parse_body(lines, i, kind)
            if tag:
                names = [tag.group(1)]
            else:
                names = [closing.strip().lstrip('}').rstrip(';').strip()]
            items.append(Item(kind, names, decl=s, doc=pending, members=members, line=start + 1))
            pending = []
            continue

        if s.startswith('typedef') or s.startswith('HDRG_API'):
            stmt = logical
            while ';' not in stmt:
                stmt += '\n' + lines[i]
                i += 1
            code, comment = _split_trailing(stmt)
            norm = _norm(code)
            if s.startswith('HDRG_API'):
                kind = 'function'
                names = _declarator_names(norm[len('HDRG_API'):])
            elif re.match(r'typedef struct \w+ \w+;$', norm):
                kind = 'opaque'
                names = [norm.split()[-1].rstrip(';')]
            elif '(' in norm:
                kind = 'fnptr'
                names = _declarator_names(norm)
            else:
                kind = 'typedef'
                names = [norm.split()[-1].rstrip(';')]
            doc = pending + (_trailing_lines(comment) if comment else [])
            items.append(Item(kind, names, decl=norm, doc=doc, line=start + 1))
            pending = []
            continue

        raise ParseError(f'{path}:{start + 1}: not understood: {s}')

    flush_pending_as_prose()

    return Header(path.name, file_doc, _merge_variants(items))


def _merge_variants(items: list) -> list:
    """One entry for a macro defined in each branch of an #if (HDRG_API and the like)."""
    out = []
    for item in items:
        last = out[-1] if out else None
        if item.kind == 'macros' and len(item.members) == 1 and last is not None and \
           last.kind == 'macros' and len(last.members) == 1 and \
           last.members[0].names == item.members[0].names:
            value = item.members[0].decl or '(empty)'
            if value not in last.members[0].variants:
                last.members[0].variants.append(value)
            last.doc = last.doc or item.doc
            continue
        if item.kind == 'macros':
            for member in item.members:
                member.variants = [member.decl or '(empty)']
        out.append(item)
    for item in out:
        if item.kind == 'macros':
            for member in item.members:
                if len(member.variants) > 1:
                    member.decl = ' | '.join(member.variants)
                    member.doc = member.doc or ['Platform dependent: one of these, by the build '
                                                'and the compiler.']

    return out


def _parse_body(lines, i, kind):
    """Members of a struct or enum body from line i; returns (members, next line, closing)."""
    members = []
    pending: list = []
    section = ''
    section_doc: list = []
    while True:
        raw = lines[i]
        s = raw.strip()
        if s.startswith('}'):
            return members, i + 1, s
        if not s:
            i += 1
            continue
        if s.startswith('/*'):
            block = raw
            while '*/' not in block:
                i += 1
                block += '\n' + lines[i]
            i += 1
            doc = _comment_lines(block)
            head = doc[0].strip() if doc else ''
            m = re.match(r'^---\s*(.*?)\s*---\s*(.*)$', head)
            if m:
                section = m.group(1)
                section_doc = ([m.group(2)] if m.group(2) else []) + doc[1:]
                pending = []
            else:
                pending = doc
            continue
        stmt = raw
        start = i
        i += 1
        # a struct member ends at its ';', an enum constant is one line
        while kind == 'struct' and ';' not in _split_trailing(stmt)[0]:
            stmt += '\n' + lines[i]
            i += 1
        code, comment = _split_trailing(stmt)
        while comment and '*/' not in comment:
            comment += '\n' + lines[i]
            i += 1
        decl = _norm(code)
        if kind == 'enum':
            names = [decl.split('=')[0].strip().rstrip(',')]
        else:
            names = _declarator_names(decl)
        if not names:
            raise ParseError(f'line {start + 1}: no member name in {decl}')
        doc = pending + (_trailing_lines(comment) if comment else [])
        members.append(Member(names, decl, doc, section, section_doc))
        section = ''
        section_doc = []
        pending = []


def parse_all(include_dir: Path) -> list:
    return [parse_header(include_dir / name) for name in HEADERS]


def api_functions(include_dir: Path) -> set:
    """Every HDRG_API function name, straight from the text (for the completeness check)."""
    names = set()
    for name in HEADERS:
        text = (include_dir / name).read_text(encoding='utf-8')
        for m in re.finditer(r'^HDRG_API\b([^;]*);', text, re.M | re.S):
            decl = _norm(m.group(1))
            names.add(decl[:decl.index('(')].split()[-1].lstrip('*'))

    return names


# --- the Japanese translations ---

def block_key(lines: list) -> str:
    """The key of a comment: a hash of its English words (rewrapping keeps it)."""
    return hashlib.sha256(' '.join(' '.join(lines).split()).encode('utf-8')).hexdigest()[:16]


def comment_blocks(header: Header):
    """(what it describes, lines) of every comment of a header, in the header's order."""
    if header.doc:
        yield '(header)', header.doc
    for item in header.items:
        owner = item.names[0] if item.names else (item.members[0].names[0] if item.members else '')
        if item.doc:
            yield (owner or '(prose)'), item.doc
        for member in item.members:
            if member.section:
                yield f'{owner} / {member.section} (title)', [member.section]
            if member.section_doc:
                yield f'{owner} / {member.section}', member.section_doc
            if member.doc:
                name = member.names[0]
                yield (f'{owner}.{name}' if item.kind == 'struct' else name), member.doc


def _blocks(lines: list) -> list:
    """The blank-line separated blocks of comment lines."""
    blocks, cur = [], []
    for line in lines:
        if line.strip():
            cur.append(line)
        elif cur:
            blocks.append(cur)
            cur = []
    if cur:
        blocks.append(cur)

    return blocks


def check_ja(en: list, ja) -> list:
    """What does not match between a comment and its translation (empty: it matches)."""
    if not isinstance(ja, list) or not all(isinstance(x, str) for x in ja) or not ja:
        return ['ja is not a list of lines']
    problems = []
    eb, jb = _blocks(en), _blocks(ja)
    if len(eb) != len(jb):
        problems.append(f'{len(jb)} blocks for {len(eb)} (keep the blank lines)')
    else:
        for k, (e, j) in enumerate(zip(eb, jb)):
            ei = sum(ln.lstrip().startswith('- ') for ln in e)
            ji = sum(ln.lstrip().startswith('- ') for ln in j)
            if ei != ji:
                problems.append(f'block {k + 1}: {ji} list items for {ei}')
    missing = sorted(set(IDENT.findall('\n'.join(en))) - set(IDENT.findall('\n'.join(ja))))
    if missing:
        problems.append('identifiers missing: ' + ', '.join(missing))

    return problems


class Translations:
    """The translations of the headers' comments, synced to the headers' comments."""

    def __init__(self, headers: list, directory: Path = JA_DIR):
        self.dir = directory
        self.files: dict = {}  # header -> entries in the header's order
        self.table: dict = {}  # key -> ja lines or None
        # every file's translations, keyed by the stored English again: a change of the key's form
        # and a comment moved to another header keep theirs
        known: dict = {}
        for path in sorted(directory.glob('*.json')):
            for e in json.loads(path.read_text(encoding='utf-8')):
                if e.get('ja') is not None:
                    known.setdefault(block_key(e['en']), e['ja'])
        for header in headers:
            entries, seen = [], set()
            for of, lines in comment_blocks(header):
                key = block_key(lines)
                if key in seen:
                    continue
                seen.add(key)
                entries.append({'key': key, 'of': of, 'en': lines, 'ja': known.get(key)})
                self.table[key] = known.get(key)
            self.files[header.name] = entries

    def ja(self, lines: list):
        return self.table.get(block_key(lines))

    def path(self, header: str) -> Path:
        return self.dir / f'{header}.json'

    @staticmethod
    def dump(entries: list) -> str:
        return json.dumps(entries, ensure_ascii=False, indent=1) + '\n'

    def text(self, header: str) -> str:
        return self.dump(self.files[header])

    def untranslated(self) -> list:
        return [(h, e['of']) for h, es in self.files.items() for e in es if e['ja'] is None]

    def problems(self) -> list:
        return [(h, e['of'], p) for h, es in self.files.items() for e in es
                if e['ja'] is not None for p in check_ja(e['en'], e['ja'])]


# --- rendering ---

class Linker:
    """Symbol -> (page, anchor); links identifiers in comments."""

    def __init__(self, headers: list):
        self.table: dict = {}
        for header in headers:
            page = header.name.replace('.h', '.html')
            for item in header.items:
                for name in item.names:
                    self.table.setdefault(name, (page, name))
                if item.kind in ('struct', 'enum', 'macros'):
                    owner = item.names[0] if item.names else None
                    for member in item.members:
                        for name in member.names:
                            if item.kind == 'struct' and owner:
                                self.table.setdefault(f'{owner}.{name}', (page, f'{owner}.{name}'))
                            else:
                                self.table.setdefault(name, (page, name))
        # the API table is typedef'd in plugin.h and defined in filter.h
        if 'hdrg_plugin_api' in self.table:
            self.table['hdrg_plugin_api_t'] = self.table['hdrg_plugin_api']

    def href(self, name: str, here: str):
        target = self.table.get(name)
        if target is None and name.endswith('*'):
            prefix = name[:-1]
            for key in sorted(self.table):
                if key.startswith(prefix) and '.' not in key:
                    target = self.table[key]
                    break
        if target is None:
            return None
        page, anchor = target

        return (f'#{anchor}' if page == here else f'{page}#{anchor}')

    def link_text(self, text: str, here: str) -> str:
        """HTML of text with identifiers linked (text is not escaped yet)."""
        out, last = [], 0
        for m in IDENT.finditer(text):
            out.append(html.escape(text[last:m.start()], quote=False))
            name = m.group(0)
            tail = ''
            if '.' in name and name not in self.table:
                # a member the reference does not list: link its struct alone
                name, _, member = name.partition('.')
                tail = '.' + member
            ref = self.href(name, here)
            if ref is None and name.endswith('*'):
                ref = self.href(name[:-1], here)
            esc = html.escape(name, quote=False)
            out.append(f'<a href="{ref}"><code>{esc}</code></a>' if ref else f'<code>{esc}</code>')
            out.append(html.escape(tail, quote=False))
            last = m.end()
        out.append(html.escape(text[last:], quote=False))

        return ''.join(out)


def _wide(ch: str) -> bool:
    return ord(ch) > 0x2E7F


def _join(parts: list) -> str:
    """Lines as one text: a space between them, none between Japanese."""
    out = ''
    for part in parts:
        if out and not (_wide(out[-1]) and _wide(part[0])):
            out += ' '
        out += part

    return out


def doc_html(lines: list, linker: Linker, here: str) -> str:
    """Comment lines -> paragraphs, lists and preformatted blocks."""
    if not lines:
        return ''

    blocks, cur = [], []
    for line in lines:
        if not line.strip():
            if cur:
                blocks.append(cur)
                cur = []
        else:
            cur.append(line)
    if cur:
        blocks.append(cur)

    # a lead paragraph followed by an indented table in one block: two blocks
    split = []
    for block in blocks:
        bulleted = any(ln.lstrip().startswith('- ') for ln in block)
        first = next((i for i, ln in enumerate(block) if i > 0 and ln.startswith('  ') and
                      all(x.startswith('  ') for x in block[i:])), None)
        if not bulleted and first is not None and len(block) - first >= 2:
            split.extend([block[:first], block[first:]])
        else:
            split.append(block)
    blocks = split

    out = []
    for block in blocks:
        indented = [ln for ln in block[1:] if ln.startswith('  ') and not ln.lstrip().startswith('- ')]
        if any(ln.lstrip().startswith('- ') for ln in block):
            # paragraphs and lists in turn: an item's continuation is indented, a line back at
            # the margin is a paragraph
            segments = []
            for ln in block:
                st = ln.strip()
                if ln.lstrip().startswith('- '):
                    if not segments or segments[-1][0] != 'ul':
                        segments.append(['ul', []])
                    segments[-1][1].append(st[2:])
                elif ln.startswith('  ') and segments and segments[-1][0] == 'ul':
                    segments[-1][1][-1] = _join([segments[-1][1][-1], st])
                else:
                    if not segments or segments[-1][0] != 'p':
                        segments.append(['p', []])
                    segments[-1][1].append(st)
            for kind, lines in segments:
                if kind == 'ul':
                    out.append('<ul>' + ''.join(f'<li>{linker.link_text(t, here)}</li>'
                                                for t in lines) + '</ul>')
                else:
                    out.append(f'<p>{linker.link_text(_join(lines), here)}</p>')
        elif len(block) >= 2 and (len(indented) >= max(2, len(block) // 2) or
                                  all(ln.startswith('  ') for ln in block)) and \
                re.search(r'\S {2,}\S', '\n'.join(block)):
            out.append('<pre class="doc-pre">' + linker.link_text('\n'.join(block), here) +
                       '</pre>')
        else:
            text = _join([ln.strip() for ln in block])
            out.append(f'<p>{linker.link_text(text, here)}</p>')

    return '\n'.join(out)


def _pretty_decl(decl: str) -> str:
    """A long function declaration broken after its commas, aligned to the parenthesis."""
    if len(decl) <= 96 or '(' not in decl:
        return decl
    head, args = decl.split('(', 1)
    if head.startswith('typedef') and '(*' in decl or 'HDRG_PLUGIN_CALL *' in decl:
        # typedef ret (CALL *name)(args): break the argument list
        idx = decl.index(')(') + 2 if ')(' in decl else decl.index('(') + 1
        head, args = decl[:idx], decl[idx:]
    else:
        head += '('
    parts = [p.strip() for p in args.split(',')]
    pad = ' ' * len(head)
    lines, cur = [], head
    for k, p in enumerate(parts):
        piece = p + (',' if k < len(parts) - 1 else '')
        if len(cur) + len(piece) + 1 > 96 and cur.strip() not in (head.strip(),):
            lines.append(cur.rstrip())
            cur = pad + piece
        else:
            cur += ('' if cur.endswith('(') else ' ') + piece
    lines.append(cur)

    return '\n'.join(lines)


UI = {
    'en': {'functions': 'Functions', 'types': 'Types', 'constants': 'Constants', 'macros': 'Macros',
           'members': 'Member', 'description': 'Description', 'name': 'Name', 'value': 'Value',
           'struct': 'struct', 'enum': 'enum', 'typedef': 'typedef', 'fnptr': 'callback type',
           'opaque': 'opaque handle', 'function': 'function', 'macrofn': 'macro', 'line': 'line',
           'index': 'Index', 'source': 'Source', 'contents': 'On this page',
           'header_doc': 'The header\'s own description', 'original': ''},
    'ja': {'functions': '関数', 'types': '型', 'constants': '定数', 'macros': 'マクロ',
           'members': 'メンバ', 'description': '説明', 'name': '名前', 'value': '値',
           'struct': '構造体', 'enum': '列挙', 'typedef': '型の別名', 'fnptr': 'コールバックの型',
           'opaque': '不透明なハンドル', 'function': '関数', 'macrofn': 'マクロ', 'line': '行',
           'index': '索引', 'source': 'ソース', 'contents': 'このページ',
           'header_doc': 'ヘッダ自体の説明', 'original': '原文 (ヘッダのコメント)'},
}


def render_header(header: Header, linker: Linker, lang: str, src_href: str,
                  tr: Translations = None) -> tuple:
    """(body HTML, sidebar index HTML) of one header's reference."""
    ui = UI[lang]
    here = header.name.replace('.h', '.html')
    out = []
    functions, types, constants = [], [], []

    def comment(lines: list) -> str:
        """A comment: in Japanese its translation with the English after it."""
        english = doc_html(lines, linker, here)
        ja = tr.ja(lines) if (lang == 'ja' and tr is not None and lines) else None
        if ja is None:
            return english

        return (doc_html(ja, linker, here) + f'<details class="api-en"><summary>{ui["original"]}'
                f'</summary>{english}</details>')

    out.append(f'<p class="api-src">{ui["source"]}: <a href="{src_href}"><code>'
               f'include/hdrgrading/{header.name}</code></a></p>')
    if header.doc:
        out.append(f'<section class="api-file"><h2>{ui["header_doc"]}</h2>'
                   f'{comment(header.doc)}</section>')

    for item in header.items:
        src_line = f'<a class="lineref" href="{src_href}#L{item.line}">{ui["line"]} {item.line}</a>'
        if item.kind == 'prose':
            out.append(f'<div class="api-prose">{comment(item.doc)}</div>')
            continue
        if item.kind == 'macros':
            rows = []
            for member in item.members:
                name = member.names[0]
                constants.append(name)
                rows.append(f'<tr id="{name}"><td><code>{name}</code></td>'
                            f'<td><code>{html.escape(member.decl)}</code></td>'
                            f'<td>{comment(member.doc)}</td></tr>')
            doc = comment(item.doc)
            out.append(f'<section class="api-item api-macros">{src_line}{doc}'
                       f'<table class="api-table"><thead><tr><th>{ui["name"]}</th>'
                       f'<th>{ui["value"]}</th><th>{ui["description"]}</th></tr></thead>'
                       f'<tbody>{"".join(rows)}</tbody></table></section>')
            continue

        name = item.names[0] if item.names else ''
        if item.kind == 'function':
            functions.append(name)
        elif item.kind == 'macrofn':
            constants.append(name)
        else:
            types.append(name)

        badge = ui[item.kind]
        parts = [f'<section class="api-item api-{item.kind}" id="{name}">',
                 f'<h3><code>{name}</code> <span class="badge">{badge}</span>{src_line}</h3>']
        if item.kind in ('function', 'fnptr', 'typedef', 'opaque', 'macrofn'):
            parts.append(f'<pre class="decl"><code>{html.escape(_pretty_decl(item.decl))}</code></pre>')
        parts.append(comment(item.doc))
        if item.kind in ('struct', 'enum'):
            rows = []
            for member in item.members:
                if member.section:
                    sdoc = comment(member.section_doc)
                    title = html.escape(member.section)
                    ja = tr.ja([member.section]) if (lang == 'ja' and tr is not None) else None
                    if ja:
                        title = (f'{html.escape(ja[0])} <span class="api-en-title">({title})'
                                 '</span>')
                    rows.append(f'<tr class="section"><td colspan="2"><strong>{title}'
                                f'</strong>{sdoc}</td></tr>')
                anchor = f'{name}.{member.names[0]}' if item.kind == 'struct' else member.names[0]
                if item.kind == 'enum':
                    constants.append(member.names[0])
                decl = _pretty_decl(member.decl)
                rows.append(f'<tr id="{anchor}"><td><pre class="mdecl"><code>{html.escape(decl)}'
                            f'</code></pre></td><td>{comment(member.doc)}</td></tr>')
            parts.append(f'<table class="api-table api-members"><thead><tr><th>{ui["members"]}</th>'
                         f'<th>{ui["description"]}</th></tr></thead><tbody>{"".join(rows)}'
                         '</tbody></table>')
        parts.append('</section>')
        out.append('\n'.join(parts))

    def index(title, names):
        if not names:
            return ''
        links = ''.join(f'<li><a href="#{n}"><code>{n}</code></a></li>'
                        for n in sorted(set(names), key=str.lower))
        return f'<h4>{title}</h4><ul class="api-index">{links}</ul>'

    side = (f'<nav class="api-side" aria-label="{ui["index"]}">'
            f'{index(ui["functions"], functions)}{index(ui["types"], types)}'
            f'{index(ui["constants"], constants)}</nav>')

    return '\n'.join(out), side, {'functions': functions, 'types': types, 'constants': constants}


def main() -> int:
    """Checks a translation file (a translator's check before the build)."""
    parser = argparse.ArgumentParser(description='check docs/scripts/api_ja/<header>.json')
    parser.add_argument('files', nargs='+', type=Path)
    args = parser.parse_args()
    bad = 0
    for path in args.files:
        entries = json.loads(path.read_text(encoding='utf-8'))
        for e in entries:
            if e.get('ja') is None:
                print(f'{path.name}: {e["of"]}: untranslated')
                bad += 1
                continue
            for problem in check_ja(e['en'], e['ja']):
                print(f'{path.name}: {e["of"]}: {problem}')
                bad += 1
    print(f'{bad} problem(s)')

    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
