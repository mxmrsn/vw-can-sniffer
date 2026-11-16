#!/bin/bash
# Convert every tracked Markdown write-up to a PDF next to it.
# Needs pandoc and Google Chrome. Usage: tools/md_to_pdf.sh [file.md ...]  (no files: all of them)
set -e
ROOT="$(git rev-parse --show-toplevel)"
CHROME="/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"
CSS="$(mktemp -t mdpdf).css"
cat > "$CSS" <<'CSSEOF'
@page { size: Letter; margin: 18mm 16mm; }
body { font-family: -apple-system, "Helvetica Neue", Arial, sans-serif; font-size: 10.5pt; line-height: 1.45; color: #1a1a1a; max-width: none; }
h1 { font-size: 20pt; border-bottom: 1px solid #bbb; padding-bottom: 4px; }
h2 { font-size: 14pt; margin-top: 1.4em; border-bottom: 1px solid #ddd; padding-bottom: 2px; }
h3 { font-size: 11.5pt; }
h1, h2, h3 { page-break-after: avoid; }
code { font-family: Menlo, Consolas, monospace; font-size: 9pt; background: #f2f2f2; padding: 1px 3px; border-radius: 3px; }
pre { background: #f5f5f5; padding: 8px 10px; border-radius: 4px; overflow-wrap: anywhere; white-space: pre-wrap; page-break-inside: avoid; }
pre code { background: none; padding: 0; font-size: 8.2pt; }
table { border-collapse: collapse; width: 100%; margin: 0.8em 0; font-size: 9.5pt; page-break-inside: avoid; }
th, td { border: 1px solid #bbb; padding: 4px 7px; text-align: left; vertical-align: top; }
th { background: #eee; }
img { max-width: 100%; page-break-inside: avoid; }
blockquote { border-left: 3px solid #bbb; margin-left: 0; padding-left: 12px; color: #444; }
a { color: #1a4f8b; text-decoration: none; }
CSSEOF
cd "$ROOT"
{ if [ $# -gt 0 ]; then printf "%s\n" "$@"; else git ls-files '*.md'; fi; } | while read -r md; do
    dir="$(dirname "$md")"; base="$(basename "$md" .md)"
    html="$(mktemp -t mdpdf).html"
    title="$(grep -m1 '^# ' "$md" | sed 's/^# //')"
    (cd "$dir" && pandoc "$base.md" -f gfm -t html5 --standalone --embed-resources \
        --metadata pagetitle="${title:-$base}" --css "$CSS" -o "$html")
    "$CHROME" --headless=new --disable-gpu --no-pdf-header-footer \
        --print-to-pdf="$ROOT/$dir/$base.pdf" "file://$html" >/dev/null 2>&1
    rm -f "$html"
    echo "$dir/$base.pdf"
done
rm -f "$CSS"
