#!/usr/bin/env python3
"""RELEASE_NOTES.md -> RELEASE_NOTES.html and RELEASE_NOTES.pdf (headless Chrome).

Handles the Markdown the release notes use: headings, paragraphs, bullet and numbered lists,
tables, horizontal rules, **bold** and `code`. Standard library only.

    python3 RELEASE/notes_to_pdf.py RELEASE/v6.9.38/RELEASE_NOTES.md
"""
import html
import os
import re
import subprocess
import sys

CHROME = "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"

CSS = """
body { font-family: -apple-system, 'Segoe UI', Helvetica, Arial, sans-serif; font-size: 10.5pt;
       color: #1a1a1a; line-height: 1.45; margin: 0; }
h1 { font-size: 19pt; margin: 0 0 6pt; color: #0b3d63; }
h2 { font-size: 13.5pt; margin: 16pt 0 6pt; color: #0b3d63; border-bottom: 1.5px solid #0b3d63; padding-bottom: 2pt; }
h3 { font-size: 11.5pt; margin: 12pt 0 4pt; }
p { margin: 4pt 0; }
ul, ol { margin: 3pt 0 6pt; padding-left: 18pt; }
li { margin: 2pt 0; }
table { border-collapse: collapse; width: 100%; margin: 6pt 0 8pt; font-size: 9.5pt; }
th, td { border: 1px solid #b8c4ce; padding: 3pt 6pt; text-align: left; vertical-align: top; }
th { background: #e8eef3; }
code { font-family: Menlo, Consolas, monospace; font-size: 9pt; background: #f1f3f5; padding: 0 2pt; border-radius: 2pt; }
hr { border: 0; border-top: 1px solid #ccd5dc; margin: 12pt 0; }
@page { size: A4; margin: 16mm 15mm; }
"""


def inline(text):
    parts = re.split(r"(`[^`]*`)", text)
    out = []
    for part in parts:
        if part.startswith("`") and part.endswith("`") and len(part) >= 2:
            out.append("<code>" + html.escape(part[1:-1]) + "</code>")
        else:
            s = html.escape(part)
            s = re.sub(r"\*\*(.+?)\*\*", r"<strong>\1</strong>", s)
            out.append(s)
    return "".join(out)


def convert(md):
    lines = md.splitlines()
    out = []
    i = 0
    para = []

    def flush_para():
        if para:
            out.append("<p>" + inline(" ".join(para)) + "</p>")
            para.clear()

    while i < len(lines):
        line = lines[i]
        stripped = line.strip()
        if not stripped:
            flush_para()
            i += 1
            continue
        m = re.match(r"^(#{1,3})\s+(.*)$", stripped)
        if m:
            flush_para()
            n = len(m.group(1))
            out.append(f"<h{n}>{inline(m.group(2))}</h{n}>")
            i += 1
            continue
        if stripped == "---":
            flush_para()
            out.append("<hr>")
            i += 1
            continue
        if stripped.startswith("|"):
            flush_para()
            rows = []
            while i < len(lines) and lines[i].strip().startswith("|"):
                rows.append(lines[i].strip())
                i += 1
            cells = [[c.strip() for c in r.strip("|").split("|")] for r in rows]
            body = [r for r in cells[1:] if not all(re.fullmatch(r":?-+:?", c) for c in r)]
            out.append("<table><tr>" + "".join(f"<th>{inline(c)}</th>" for c in cells[0]) + "</tr>")
            for r in body:
                out.append("<tr>" + "".join(f"<td>{inline(c)}</td>" for c in r) + "</tr>")
            out.append("</table>")
            continue
        m = re.match(r"^(\s*)([-*]|\d+\.)\s+(.*)$", line)
        if m:
            flush_para()
            ordered = m.group(2)[0].isdigit()
            tag = "ol" if ordered else "ul"
            out.append(f"<{tag}>")
            while i < len(lines):
                m = re.match(r"^(\s*)([-*]|\d+\.)\s+(.*)$", lines[i])
                if not m:
                    break
                item = [m.group(3)]
                i += 1
                # Continuation lines: indented, not a new item
                while i < len(lines) and lines[i].startswith("  ") and lines[i].strip() and \
                        not re.match(r"^\s*([-*]|\d+\.)\s+", lines[i]):
                    item.append(lines[i].strip())
                    i += 1
                out.append("<li>" + inline(" ".join(item)) + "</li>")
            out.append(f"</{tag}>")
            continue
        para.append(stripped)
        i += 1
    flush_para()
    return "\n".join(out)


def main():
    md_path = os.path.abspath(sys.argv[1])
    md = open(md_path, encoding="utf-8").read()
    title = re.search(r"^#\s+(.*)$", md, re.M)
    title = title.group(1) if title else "Release notes"
    page = (f"<!doctype html><html><head><meta charset='utf-8'><title>{html.escape(title)}</title>"
            f"<style>{CSS}</style></head><body>{convert(md)}</body></html>")
    base = os.path.splitext(md_path)[0]
    open(base + ".html", "w", encoding="utf-8").write(page)
    if not os.path.exists(CHROME):
        print("Google Chrome not found: HTML written, no PDF")
        return
    subprocess.run([CHROME, "--headless=new", "--disable-gpu", "--no-pdf-header-footer",
                    f"--print-to-pdf={base}.pdf", "file://" + base + ".html"],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=120)
    print(f"Written {base}.html and {base}.pdf")


if __name__ == "__main__":
    main()
