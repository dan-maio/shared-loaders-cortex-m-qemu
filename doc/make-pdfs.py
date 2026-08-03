#!/usr/bin/env python3
"""Render the SCL markdown docs to PDF with print-quality styling.

Fixes over old rendering:
- Tables keep optimal column proportions via intelligent <colgroup> generation.
- Prevents mid-word hyphenation in code and text (no more `sec-tion` or `anonym-ous`).
- Heading page breaks fixed (no empty white-space pages from unconditional h2 breaks).
- Clean code wrapping and page break management.

Usage:  python3 make-pdfs.py            # renders all doc/*.md files
        python3 make-pdfs.py FILE.md    # renders FILE.pdf
"""
import sys
import pathlib
import markdown
import bs4
from weasyprint import HTML

EXTENSIONS = ["fenced_code", "tables", "codehilite", "toc", "sane_lists"]

CSS = """
@page {
    size: A4;
    margin: 1.6cm 1.4cm;
    @bottom-right { content: counter(page) " / " counter(pages);
                    font-family: sans-serif; font-size: 7.5pt; color: #64748b; }
    @bottom-left  { content: string(doctitle);
                    font-family: sans-serif; font-size: 7.5pt; color: #64748b; }
}

html, body {
    font-family: 'DejaVu Sans', 'Helvetica Neue', Arial, sans-serif;
    font-size: 8.5pt;
    line-height: 1.45;
    color: #1e293b;
    hyphens: none;
    -webkit-hyphens: none;
}

h1 {
    font-size: 16pt;
    border-bottom: 2px solid #2563eb;
    padding-bottom: 5px;
    margin-top: 24px;
    margin-bottom: 10px;
    color: #0f172a;
    string-set: doctitle content();
    break-after: avoid;
    page-break-after: avoid;
    break-before: page;
    page-break-before: always;
}
h1:first-of-type, body > h1:first-child {
    break-before: avoid;
    page-break-before: avoid;
    margin-top: 0;
}
h2 {
    font-size: 11.5pt;
    border-bottom: 1px solid #cbd5e1;
    padding-bottom: 4px;
    margin-top: 16px;
    margin-bottom: 8px;
    color: #1e293b;
    break-after: avoid;
    page-break-after: avoid;
    break-before: auto;
    page-break-before: auto;
}
h3 {
    font-size: 9.8pt;
    margin-top: 12px;
    margin-bottom: 4px;
    color: #334155;
    break-after: avoid;
    page-break-after: avoid;
}
h4 {
    font-size: 8.8pt;
    margin-top: 10px;
    margin-bottom: 3px;
    color: #475569;
    break-after: avoid;
    page-break-after: avoid;
}

p, li { orphans: 2; widows: 2; margin-bottom: 4px; }
li { margin-bottom: 2px; }

blockquote {
    border-left: 3.5px solid #2563eb;
    background: #f0f9ff;
    margin: 8px 0;
    padding: 6px 10px;
    color: #1e40af;
    font-size: 8.2pt;
    border-radius: 0 4px 4px 0;
}

code {
    font-family: 'DejaVu Sans Mono', monospace;
    font-size: 7.3pt;
    background: #f1f5f9;
    padding: 1px 3.5px;
    border-radius: 3px;
    color: #0f172a;
    word-break: break-word;
    overflow-wrap: break-word;
    hyphens: none;
}
pre {
    background: #f8fafc;
    border: 1px solid #e2e8f0;
    border-radius: 4px;
    padding: 8px 10px;
    margin: 8px 0;
    font-family: 'DejaVu Sans Mono', monospace;
    font-size: 7pt;
    line-height: 1.35;
    white-space: pre-wrap;
    word-break: break-word;
    overflow-wrap: break-word;
    break-inside: avoid;
    page-break-inside: avoid;
    hyphens: none;
}
pre code {
    background: none;
    padding: 0;
    font-size: inherit;
    color: inherit;
}

/* === TABLE STYLING — Sane column widths & clean layout === */

table {
    width: 100%;
    table-layout: fixed;
    border-collapse: collapse;
    margin: 10px 0;
    font-size: 7.8pt;
    line-height: 1.35;
    page-break-inside: auto;
}
thead { display: table-header-group; }
tr { page-break-inside: avoid; }
th, td {
    border: 0.5pt solid #cbd5e1;
    padding: 3.5pt 5pt;
    text-align: left;
    vertical-align: top;
    word-break: break-word;
    overflow-wrap: break-word;
    hyphens: none;
    -webkit-hyphens: none;
}
th {
    background: #f1f5f9;
    font-weight: 600;
    font-size: 7.5pt;
    color: #0f172a;
    border-bottom: 1.5pt solid #94a3b8;
}
tbody tr:nth-child(even) { background: #f8fafc; }

td code, th code {
    font-family: 'DejaVu Sans Mono', monospace;
    font-size: 7pt;
    background: #f1f5f9;
    padding: 0.5px 2.5px;
    border-radius: 2px;
    white-space: pre-wrap;
    word-break: break-word;
    overflow-wrap: break-word;
    hyphens: none;
    -webkit-hyphens: none;
}

hr { border: none; border-top: 0.75pt solid #cbd5e1; margin: 12px 0; }
a { color: #2563eb; text-decoration: none; }
"""


def compute_col_widths(num_cols: int, headers: list[str]) -> list[int]:
    """Compute optimal column width percentages based on header count & text."""
    headers_lower = [h.lower() for h in headers]

    # Explicit presets for common doc tables
    if headers_lower == ["library", "loads", "code model", "linking", "memory", "float abi", "needs heap?"]:
        return [16, 9, 13, 20, 18, 12, 12]
    if headers_lower == ["function", "line", "scope", "signature / description"]:
        return [22, 6, 15, 57]
    if headers_lower == ["method", "file:line", "signature", "description"]:
        return [20, 10, 28, 42]
    if headers_lower == ["function", "line", "scope", "description"]:
        return [22, 6, 15, 57]
    if headers_lower == ["function", "line", "description"]:
        return [22, 6, 72]
    if headers_lower in [["method", "signature", "description"], ["function", "signature", "description"]]:
        return [20, 32, 48]
    if headers_lower == ["#", "depth", "file", "options", "what it does"]:
        return [6, 14, 25, 19, 36]
    if headers_lower == ["struct", "bytes", "fields", "purpose"]:
        return [20, 9, 25, 46]

    # Heuristic calculation
    weights = []
    for h in headers_lower:
        if h == "#":
            weights.append(7)

        elif any(k in h for k in ["line", "-fpic?"]):
            weights.append(8)
        elif any(k in h for k in ["depth", "bytes", "heap?", "hardware?", "soft-float", "hard-float"]):
            weights.append(15)
        elif any(k in h for k in ["desc", "purpose", "what it", "meaning", "action", "implication", "linking", "code model"]):
            weights.append(40)
        elif any(k in h for k in ["signature", "file", "target", "options", "tested by", "where", "handler", "relocation", "role"]):
            weights.append(26)
        else:
            weights.append(22)

    total = sum(weights)
    widths = [round(w * 100 / total) for w in weights]
    diff = 100 - sum(widths)
    widths[-1] += diff
    return widths



def process_html_tables(html_str: str) -> str:
    """Inject smart <colgroup> into HTML tables before feeding to WeasyPrint."""
    soup = bs4.BeautifulSoup(html_str, "html.parser")
    for table in soup.find_all("table"):
        ths = table.find_all("th")
        if not ths:
            continue
        headers = [th.get_text(strip=True) for th in ths]
        widths = compute_col_widths(len(ths), headers)

        for cg in table.find_all("colgroup"):
            cg.decompose()

        colgroup = soup.new_tag("colgroup")
        for w in widths:
            col = soup.new_tag("col", style=f"width: {w}%;")
            colgroup.append(col)
        table.insert(0, colgroup)
    return str(soup)


def render(md_path: pathlib.Path) -> pathlib.Path:
    raw_html = markdown.markdown(md_path.read_text(), extensions=EXTENSIONS)
    processed_body = process_html_tables(raw_html)
    full_html = (f"<!DOCTYPE html><html lang='en'><head><meta charset='utf-8'>"
                 f"<style>{CSS}</style></head><body>{processed_body}</body></html>")
    out = md_path.with_suffix(".pdf")
    HTML(string=full_html).write_pdf(out)
    return out


def main() -> None:
    here = pathlib.Path(__file__).parent
    if len(sys.argv) > 1:
        targets = [pathlib.Path(a) for a in sys.argv[1:]]
    else:
        # Default: render all markdown files in doc/ directory plus README.md
        targets = sorted(list(here.glob("*.md")))
        repo_root = here.parent
        if (repo_root / "README.md").exists() and repo_root / "README.md" not in targets:
            targets.append(repo_root / "README.md")
        if (repo_root / "RUNNING_TESTS.md").exists() and repo_root / "RUNNING_TESTS.md" not in targets:
            targets.append(repo_root / "RUNNING_TESTS.md")

    for md in targets:
        if md.exists():
            out = render(md)
            print(f"  {md.name} -> {out.name}")


if __name__ == "__main__":
    main()

