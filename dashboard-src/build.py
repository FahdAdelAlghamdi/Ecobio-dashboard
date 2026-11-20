"""Build the dashboard into ONE file for GitHub Pages: ../index.html (the repository root).

    python build.py

template.html  page (HTML + CSS + JavaScript), with three placeholders:
models.json    the exported scikit-learn models   -> __MODELS__
mark.b64       team logo mark (webp, base64)      -> __MARK__
icon.b64       browser icon (png, base64)         -> __ICON__
"""
import pathlib

here = pathlib.Path(__file__).parent
body = (here / "template.html").read_text(encoding="utf-8")
body = body.replace("__MODELS__", (here / "models.json").read_text(encoding="utf-8").replace("</", "<\\/"))
body = body.replace("__MARK__", (here / "mark.b64").read_text().strip()).replace("__ICON__", (here / "icon.b64").read_text().strip())
for ph in ("__MODELS__", "__MARK__", "__ICON__"):
    assert ph not in body, ph

head = ('<!doctype html>\n<html lang="ar" dir="rtl">\n<head>\n<meta charset="utf-8">\n'
        '<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">\n')
title_end = body.index("</title>") + len("</title>")
doc = (head + body[:title_end] + "\n" + body[title_end:body.index('<div class="wrap"')] + "</head>\n<body>\n"
       + body[body.index('<div class="wrap"'):] + "\n</body>\n</html>\n")
doc = doc.replace("body { background: var(--bg);", "body { margin: 0; background: var(--bg);")

out = here.parent / "index.html"
out.write_text(doc, encoding="utf-8", newline="\n")
print("ok", out, len(doc.encode("utf-8")) // 1024, "KB")
