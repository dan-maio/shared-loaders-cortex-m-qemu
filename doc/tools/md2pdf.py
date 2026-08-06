#!/usr/bin/env python3
"""markdown -> PDF, matching the pipeline that made the existing files.

The existing files record Producer: WeasyPrint 69.0 -- the version this renders
with -- and carry no title metadata, so the original was a plain HTML render
rather than a pandoc standalone document. No CSS survives anywhere in the tree,
so the page geometry below was recovered by matching page counts against the
existing PDFs: 1.5cm/9pt/1.32 reproduces README.pdf at 18 pages and
Full-Manual-SCL.pdf at 68, exactly, and scl-build.pdf within one page.
"""
import sys, pathlib, markdown, weasyprint

CSS = """
@page { size: A4; margin: 1.5cm; }
body { font-family: "DejaVu Serif", serif; font-size: 9pt; line-height: 1.32; }
h1, h2, h3, h4 { font-family: "DejaVu Sans", sans-serif; page-break-after: avoid; }
h1 { font-size: 19pt; } h2 { font-size: 15pt; } h3 { font-size: 12pt; }
code, pre { font-family: "DejaVu Sans Mono", monospace; font-size: 7.5pt; }
pre { background: #f6f6f6; padding: .5em; border-radius: 3px;
      white-space: pre-wrap; word-wrap: break-word; page-break-inside: avoid; }
code { background: #f2f2f2; padding: .05em .25em; }
pre code { background: none; padding: 0; }
table { border-collapse: collapse; width: 100%; font-size: 8pt;
        page-break-inside: avoid; }
th, td { border: 1px solid #bbb; padding: .25em .4em; text-align: left;
         vertical-align: top; }
th { background: #eee; }
img { max-width: 100%; }
blockquote { border-left: 3px solid #ccc; margin-left: 0; padding-left: .8em;
             color: #444; }
"""

def convert(src: pathlib.Path, out: pathlib.Path) -> None:
    html = markdown.markdown(
        src.read_text(),
        extensions=['tables', 'fenced_code', 'toc', 'sane_lists',
                    'pymdownx.superfences'],
    )
    doc = f'<!doctype html><meta charset="utf-8"><style>{CSS}</style>{html}'
    weasyprint.HTML(string=doc, base_url=str(src.parent)).write_pdf(str(out))

if __name__ == '__main__':
    s = pathlib.Path(sys.argv[1])
    convert(s, pathlib.Path(sys.argv[2] if len(sys.argv) > 2 else s.with_suffix('.pdf')))
    print(f'wrote {sys.argv[2] if len(sys.argv) > 2 else s.with_suffix(".pdf")}')

# Usage:
#   python3 doc/tools/md2pdf.py doc/README.md          # -> doc/README.pdf
#   for m in $(git ls-files '*.md'); do \
#       [ -f "${m%.md}.pdf" ] && python3 doc/tools/md2pdf.py "$m"; done
#
# Needs weasyprint and markdown. Neither is packaged here, so:
#   python3 -m venv .venv && .venv/bin/pip install weasyprint markdown
#   .venv/bin/python doc/tools/md2pdf.py doc/README.md
