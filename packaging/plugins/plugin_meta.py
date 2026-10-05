#!/usr/bin/env python3
"""plugin_meta.py - the README cards of the optional plugins and the tables
made from them (plugins/README.md, docs/PLUGINS.md).

  python3 packaging/plugins/plugin_meta.py write
      regenerate the plugin tables of docs/PLUGINS.md and README.md, and the
      example code included in docs/PLUGINS.md (<!-- include:begin <path> -->)
  python3 packaging/plugins/plugin_meta.py check
      exit 1 when a table is stale or a plugin folder lacks what the tables
      and the packaging need (CTest test_plg_docs)
  python3 packaging/plugins/plugin_meta.py list <platform>
      "<slug> <version> <zip name>" per plugin (build-plugins.sh)
  python3 packaging/plugins/plugin_meta.py bundle <platform>
      the name of the all-plugins zip
  python3 packaging/plugins/plugin_meta.py json
      every plugin's metadata as JSON
  python3 packaging/plugins/plugin_meta.py zip <zip> <base dir> <entry>...
      a reproducible zip of the entries (folders recursively) of base dir:
      sorted names, the time stamp SOURCE_DATE_EPOCH (default 1980-01-01),
      Unix modes 0755 for folders and libraries, 0644 for the rest
  python3 packaging/plugins/plugin_meta.py sums <dir>
      <dir>/SHA256SUMS for every paintc-plugin*.zip in dir (sha256sum format)

A plugin is a folder plugins/<slug>/ with a CMakeLists.txt that calls
pc_add_plugin(<slug> ...). Its README.md starts with a card, an HTML comment
GitHub does not show:

  <!-- paintc-plugin
  name: Align Object
  version: 1.0.0
  menu: Effects > Object > Align Object
  summary: Moves the object on a layer to an edge, a corner or the center.
  original: Align Object by xod (with help from MJW)
  original-url: https://forums.getpaint.net/...
  basis: clean room
  -->

name, summary and original are required, menu is recommended; version
defaults to the plugin set version of release.conf; basis is "clean room"
or "source (<license>)" for a port of permissively licensed source (whose
license text is then LICENSE-original.txt). Without a card the values are
guessed from the text (title, first paragraph, the Credits section) and
check() warns. Python 3.7+, standard library only.
"""
import hashlib
import json
import os
import re
import sys
import time
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
PLATFORMS = ("linux-x86_64", "windows-x64")
PLATFORM_LABEL = {"linux-x86_64": "Linux x86_64", "windows-x64": "Windows x64"}

CARD_RE = re.compile(r"<!--\s*paintc-plugin[ \t]*\r?\n(.*?)-->", re.S)
TABLES = {
    # file: (begin marker id, renderer name)
    os.path.join("docs", "PLUGINS.md"): "plugin-table",
    "README.md": "plugin-downloads",
}
REQUIRED = ("name", "summary", "original")
EM_DASH, EN_DASH = chr(0x2014), chr(0x2013)
KNOWN = ("name", "version", "menu", "summary", "original", "original-url", "basis")


def release_conf():
    conf = {"version": "1.0.0", "repo": "vancevoj/paint.c", "glibc_floor": "2.31"}
    path = os.path.join(HERE, "release.conf")
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            k, v = line.split("=", 1)
            conf[k.strip()] = v.strip()
    return conf


def norm_version(v):
    """At least three numeric parts ("1.0" -> "1.0.0"); other forms unchanged."""
    parts = v.split(".")
    if all(p.isdigit() for p in parts):
        while len(parts) < 3:
            parts.append("0")
    return ".".join(parts)


# ---- text helpers -----------------------------------------------------------------
def plain(s):
    """Markdown inline text to plain text on one line."""
    s = re.sub(r"!\[[^\]]*\]\([^)]*\)", "", s)               # images
    s = re.sub(r"\[([^\]]+)\]\([^)]*\)", r"\1", s)            # links
    s = re.sub(r"(\*\*|__)(.+?)\1", r"\2", s)                 # bold
    s = re.sub(r"(?<![\w*])\*(?!\s)(.+?)\*(?!\w)", r"\1", s)  # italics
    s = s.replace("`", "")
    return " ".join(s.split())


def no_dashes(s):
    """The project writes no em or en dashes (docs/RULES.md house style)."""
    s = re.sub("\\s*" + EM_DASH + "\\s*", ", ", s)
    return s.replace(EN_DASH, "-")


def first_sentence(s):
    m = re.match(r"(.+?[.!?])(\s+[A-Z(\"]|\s*$)", s)
    return m.group(1) if m else s


def join_names(names):
    if len(names) <= 1:
        return "".join(names)
    return ", ".join(names[:-1]) + " and " + names[-1]


def section(body, pattern):
    """Text of the first ## heading matching pattern, up to the next heading."""
    m = re.search(r"^(#{2,6})\s*(" + pattern + r")\b.*$", body, re.M | re.I)
    if not m:
        return ""
    rest = body[m.end():]
    end = re.search(r"^#{1,%d}\s" % len(m.group(1)), rest, re.M)
    return rest[: end.start()] if end else rest


def guess_license(path):
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            t = f.read(4000)
    except OSError:
        return ""
    for pat, name in ((r"MIT License|Permission is hereby granted, free of charge", "MIT"),
                      (r"Apache License", "Apache-2.0"),
                      (r"Redistribution and use in source and binary forms", "BSD"),
                      (r"zlib License|This software is provided 'as-is'", "zlib"),
                      (r"unlicense|public domain|CC0", "public domain")):
        if re.search(pat, t, re.I):
            return name
    return ""


# ---- plugins -----------------------------------------------------------------------
def guess(body, folder):
    """Values for a README without a card (or with gaps in it)."""
    g = {}
    h1 = re.search(r"^#\s+(.+)$", body, re.M)
    if h1:
        name = plain(h1.group(1))
        g["name"] = re.sub(r"\s*\((?:an?\s+)?paint\.c[^)]*\)\s*$", "", name, flags=re.I).strip()
    after = body[h1.end():] if h1 else body
    for p in re.split(r"\n\s*\n", after):
        p = p.strip()
        if not p or p[0] in "#|<!>`-*" or p[:2] in ("![", "[!") or re.match(r"\d+\.\s", p):
            continue
        g["summary"] = first_sentence(plain(p))
        break
    m = re.search(r"\b((?:Effects|Adjustments)(?:\s*>\s*[^>*\n`]+?)+?)(?:\.\.\.|\u2026)?(?:\*\*|`|\.\s|\.?$|\n)",
                  body, re.M)
    if m:
        g["menu"] = " ".join(m.group(1).split()).rstrip(". ")
    cred = section(body, r"credits?|original|acknowledg\w*|attribution|thanks")
    if cred:
        quoted = re.search(r"[\"\u201c]([^\"\u201d]+)[\"\u201d]", cred)
        names = [n.strip() for n in re.findall(r"\*\*([^*]+)\*\*", cred)]
        names = [n for n in names if not quoted or n != quoted.group(1)]
        if names:
            g["original"] = (quoted.group(1) + " by " if quoted else "") + join_names(names)
        else:
            g["original"] = first_sentence(plain(cred.strip().split("\n\n")[0]))
        url = re.search(r"https?://[^\s)>\]]+", cred)
        if url:
            g["original-url"] = url.group(0).rstrip(".,;")
    lic = os.path.join(folder, "LICENSE-original.txt")
    if re.search(r"clean[- ]room", body, re.I):
        g["basis"] = "clean room"
    elif os.path.exists(lic):
        name = guess_license(lic)
        g["basis"] = "source (%s)" % name if name else "source"
    return g


def read_plugin(slug, conf):
    folder = os.path.join(ROOT, "plugins", slug)
    p = {"slug": slug, "problems": [], "warnings": []}
    readme = os.path.join(folder, "README.md")
    with open(os.path.join(folder, "CMakeLists.txt"), encoding="utf-8") as f:
        cm = re.search(r"pc_add_plugin\(\s*([^\s)]+)", f.read())
    if not cm or cm.group(1) != slug:
        p["problems"].append("CMakeLists.txt must call pc_add_plugin(%s ...): the library, the "
                             "folder and the zip share that name" % slug)
    text = ""
    if os.path.exists(readme):
        with open(readme, encoding="utf-8") as f:
            text = f.read()
        if not text.strip():
            p["problems"].append("README.md is empty")
    else:
        p["problems"].append("README.md is missing")
    card = {}
    m = CARD_RE.search(text)
    if m:
        key = None
        for line in m.group(1).splitlines():
            km = re.match(r"\s*([A-Za-z][\w-]*)\s*:\s*(.*)$", line)
            if km and km.group(1).lower() in KNOWN:
                key = km.group(1).lower()
                card[key] = km.group(2).strip()
            elif key and line.strip():
                card[key] = (card[key] + " " + line.strip()).strip()
    elif text:
        p["warnings"].append("README.md has no <!-- paintc-plugin card (plugins/README.md); "
                             "values are guessed from its text")
    body = CARD_RE.sub("", text)
    g = guess(body, folder)
    for k in KNOWN:
        v = card.get(k) or g.get(k, "")
        if k not in card and v and m and k in REQUIRED + ("menu",):
            p["warnings"].append("card has no %s: line; guessed %r" % (k, v))
        p[k] = no_dashes(v)
    for k in REQUIRED:
        if text and not p[k]:
            p["problems"].append("no %s (add a %s: line to the card)" % (k, k))
    p["name"] = p["name"] or slug
    p["version"] = p["version"] or conf["version"]
    if not re.match(r"^[0-9]+(\.[0-9]+){0,3}([-+][0-9A-Za-z.]+)?$", p["version"]):
        p["problems"].append("version %r is not like 1.2.0" % p["version"])
    p["zip_version"] = norm_version(p["version"])
    p["has_screenshot"] = os.path.exists(os.path.join(folder, "screenshot.png"))
    if not p["has_screenshot"]:
        p["warnings"].append("screenshot.png is missing (the release zip needs one)")
    p["has_license_original"] = os.path.exists(os.path.join(folder, "LICENSE-original.txt"))
    return p


def plugins(conf):
    base = os.path.join(ROOT, "plugins")
    slugs = sorted(d for d in os.listdir(base)
                   if os.path.isfile(os.path.join(base, d, "CMakeLists.txt")))
    out = []
    for s in slugs:
        if not re.match(r"^[A-Za-z0-9_.-]+$", s):
            continue
        out.append(read_plugin(s, conf))
    out.sort(key=lambda p: (p.get("name") or p["slug"]).casefold())
    return out


def zip_name(p, platform):
    return "paintc-plugin-%s-%s-%s.zip" % (p["slug"], p["zip_version"], platform)


def bundle_name(conf, platform):
    return "paintc-plugins-all-%s-%s.zip" % (norm_version(conf["version"]), platform)


def asset_url(conf, name):
    return "https://github.com/%s/releases/download/plugins-v%s/%s" % (
        conf["repo"], norm_version(conf["version"]), name)


def release_url(conf):
    return "https://github.com/%s/releases/tag/plugins-v%s" % (
        conf["repo"], norm_version(conf["version"]))


# ---- tables -------------------------------------------------------------------------
def cell(s):
    return s.replace("|", "\\|").strip()


def credit(p):
    s = cell(p["original"])
    if p["original-url"]:
        s += " ([original](%s))" % p["original-url"]
    return s


def render_plugin_table(conf, ps, link_prefix):
    """docs/PLUGINS.md: every plugin with menu, basis, version and downloads."""
    rows = ["| Plugin | Menu | What it does | Original design | Basis | Version | Download |",
            "|---|---|---|---|---|---|---|"]
    for p in ps:
        dl = " / ".join("[%s](%s)" % (PLATFORM_LABEL[pl].split()[0], asset_url(conf, zip_name(p, pl)))
                        for pl in PLATFORMS)
        rows.append("| [%s](%splugins/%s/README.md) | %s | %s | %s | %s | %s | %s |" % (
            cell(p["name"]), link_prefix, p["slug"], cell(p["menu"]), cell(p["summary"]),
            credit(p), cell(p["basis"] or "clean room"), cell(p["version"]), dl))
    rows.append("")
    rows.append("All plugins in one zip per system: [%s](%s), [%s](%s). Checksums: "
                "[SHA256SUMS](%s). Release page: [plugins-v%s](%s)." % (
                    bundle_name(conf, PLATFORMS[0]), asset_url(conf, bundle_name(conf, PLATFORMS[0])),
                    bundle_name(conf, PLATFORMS[1]), asset_url(conf, bundle_name(conf, PLATFORMS[1])),
                    asset_url(conf, "SHA256SUMS"), norm_version(conf["version"]),
                    release_url(conf)))
    return "\n".join(rows)


def render_download_table(conf, ps):
    """README.md: name, what it does, original author credit, downloads."""
    rows = ["| Plugin | What it does | Original design (credit) | %s | %s |" % (
                PLATFORM_LABEL[PLATFORMS[0]], PLATFORM_LABEL[PLATFORMS[1]]),
            "|---|---|---|---|---|"]
    for p in ps:
        rows.append("| [%s](plugins/%s/README.md) | %s | %s | [zip](%s) | [zip](%s) |" % (
            cell(p["name"]), p["slug"], cell(p["summary"]), credit(p),
            asset_url(conf, zip_name(p, PLATFORMS[0])), asset_url(conf, zip_name(p, PLATFORMS[1]))))
    rows.append("| **All of them** | Every plugin above in one zip | | [zip](%s) | [zip](%s) |" % (
        asset_url(conf, bundle_name(conf, PLATFORMS[0])),
        asset_url(conf, bundle_name(conf, PLATFORMS[1]))))
    return "\n".join(rows)


def render(marker, conf, ps):
    if marker == "plugin-table":
        return render_plugin_table(conf, ps, "../")
    return render_download_table(conf, ps)


def replace_block(text, marker, body):
    begin = "<!-- %s:begin" % marker
    end = "<!-- %s:end -->" % marker
    i = text.find(begin)
    j = text.find(end)
    if i < 0 or j < i:
        return None
    head_end = text.index("-->", i) + 3
    return text[:head_end] + "\n" + body + "\n" + text[j:]


INCLUDE_RE = re.compile(r"(<!-- include:begin ([^ ]+) -->\n)(.*?)(<!-- include:end -->)", re.S)


def replace_includes(text):
    """Refreshes <!-- include:begin <path> --> blocks: the file (relative to
    the repository root) as a fenced code block."""
    def sub(m):
        path = os.path.join(ROOT, m.group(2))
        with open(path, encoding="utf-8") as f:
            code = f.read()
        lang = "c" if path.endswith((".c", ".h")) else ""
        return m.group(1) + "```" + lang + "\n" + code.rstrip("\n") + "\n```\n" + m.group(4)
    return INCLUDE_RE.sub(sub, text)


def update(write):
    conf = release_conf()
    ps = plugins(conf)
    bad = False
    for p in ps:
        for w in p["warnings"]:
            print("plugins/%s: warning: %s" % (p["slug"], w), file=sys.stderr)
        for e in p["problems"]:
            print("plugins/%s: error: %s" % (p["slug"], e), file=sys.stderr)
            bad = True
    for rel, marker in TABLES.items():
        path = os.path.join(ROOT, rel)
        with open(path, encoding="utf-8") as f:
            text = f.read()
        new = replace_block(text, marker, render(marker, conf, ps))
        if new is not None:
            try:
                new = replace_includes(new)
            except OSError as e:
                print("%s: error: an included example is missing: %s" % (rel, e), file=sys.stderr)
                bad = True
                continue
        if new is None:
            print("%s: error: no <!-- %s:begin --> ... <!-- %s:end --> block" % (rel, marker, marker),
                  file=sys.stderr)
            bad = True
            continue
        if new != text:
            if write:
                with open(path, "w", encoding="utf-8", newline="\n") as f:
                    f.write(new)
                print("%s: plugin table updated (%d plugins)" % (rel, len(ps)))
            else:
                print("%s: error: the plugin table (or an included example) is stale; run "
                      "python3 packaging/plugins/plugin_meta.py write" % rel, file=sys.stderr)
                bad = True
    if not write and not bad:
        print("plugin tables current (%d plugins)" % len(ps))
    return 1 if bad else 0


# ---- packaging helpers ---------------------------------------------------------------
LIB_EXT = (".so", ".dll", ".dylib")


def make_zip(zpath, base, entries):
    epoch = int(os.environ.get("SOURCE_DATE_EPOCH", "315532800"))
    stamp = time.gmtime(max(epoch, 315532800))[:6]          # zip cannot go before 1980
    names = []
    for e in entries:
        top = os.path.join(base, e)
        if os.path.isdir(top):
            for d, dirs, files in os.walk(top):
                rel = os.path.relpath(d, base).replace(os.sep, "/")
                names.append(rel + "/")
                names.extend(rel + "/" + f for f in files)
        else:
            names.append(e.replace(os.sep, "/"))
    tmp = zpath + ".part"
    with zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for n in sorted(set(names)):
            info = zipfile.ZipInfo(n, stamp)
            info.create_system = 3                            # Unix modes below
            if n.endswith("/"):
                info.external_attr = (0o40755 << 16) | 0x10
                z.writestr(info, b"")
                continue
            mode = 0o755 if n.endswith(LIB_EXT) else 0o644
            info.external_attr = (0o100000 | mode) << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            with open(os.path.join(base, n), "rb") as f:
                z.writestr(info, f.read(), compresslevel=9)
    with zipfile.ZipFile(tmp) as z:
        bad = z.testzip()
        if bad:
            raise SystemExit("zip: %s is corrupt in %s" % (bad, tmp))
    os.replace(tmp, zpath)
    return 0


def write_sums(d):
    lines = []
    for n in sorted(os.listdir(d)):
        if n.startswith("paintc-plugin") and n.endswith(".zip"):
            h = hashlib.sha256()
            with open(os.path.join(d, n), "rb") as f:
                for chunk in iter(lambda: f.read(1 << 20), b""):
                    h.update(chunk)
            lines.append("%s  %s\n" % (h.hexdigest(), n))
    with open(os.path.join(d, "SHA256SUMS"), "w", encoding="ascii", newline="\n") as f:
        f.writelines(lines)
    sys.stdout.writelines(lines)
    return 0


def main(argv):
    if len(argv) < 2 or argv[1] in ("-h", "--help"):
        print(__doc__)
        return 0 if len(argv) >= 2 else 2
    cmd = argv[1]
    conf = release_conf()
    if cmd == "write":
        return update(True)
    if cmd == "check":
        return update(False)
    if cmd in ("list", "bundle"):
        if len(argv) < 3:
            print("usage: plugin_meta.py %s <platform>" % cmd, file=sys.stderr)
            return 2
        if cmd == "bundle":
            print(bundle_name(conf, argv[2]))
            return 0
        rc = 0
        for p in sorted(plugins(conf), key=lambda q: q["slug"]):
            for e in p["problems"]:
                print("plugins/%s: error: %s" % (p["slug"], e), file=sys.stderr)
                rc = 1
            print("%s %s %s" % (p["slug"], p["zip_version"], zip_name(p, argv[2])))
        return rc
    if cmd == "json":
        json.dump({"release": conf, "plugins": plugins(conf)}, sys.stdout, indent=2)
        print()
        return 0
    if cmd == "zip" and len(argv) >= 5:
        return make_zip(argv[2], argv[3], argv[4:])
    if cmd == "sums" and len(argv) == 3:
        return write_sums(argv[2])
    print("plugin_meta.py: unknown command or arguments %r (write, check, list, bundle, json, "
          "zip, sums)" % " ".join(argv[1:]), file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
