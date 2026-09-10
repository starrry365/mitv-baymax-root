import re
from collections import defaultdict

FILES = [
    r"D:\Work\WorkBuddy\.tools\tv\vendor_sepolicy.cil",
    r"D:\Work\WorkBuddy\.tools\tv\plat_sepolicy.cil",
]


def strip_comments(text):
    out = []
    for line in text.split("\n"):
        i = line.find(";")
        if i != -1:
            if line[:i].strip() == "":
                continue
            line = line[:i]
        out.append(line)
    return "\n".join(out)


def top_level_sexps(text):
    out, depth, buf = [], 0, []
    for ch in text:
        if ch == "(":
            depth += 1; buf.append(ch)
        elif ch == ")":
            depth -= 1; buf.append(ch)
            if depth <= 0:
                out.append("".join(buf)); buf = []; depth = 0
        elif depth > 0:
            buf.append(ch)
    return out


rules = []
for f in FILES:
    rules += top_level_sexps(strip_comments(open(f, encoding="utf-8", errors="replace").read()))

SRC = "misysdiagnose"
owned = defaultdict(set)

for s in rules:
    if not s.startswith("(allow "):
        continue
    one = " ".join(s.split())
    m = re.match(r"\(allow (\S+) (\S+) \((.*)\)\)", one)
    if not m:
        continue
    src, tgt, rest = m.group(1), m.group(2), m.group(3)
    if SRC not in src:
        continue
    # rest 形如: file (read write open)  或  dir (...) 
    for cls, perms in re.findall(r"(\w+) \(([^)]*)\)", rest):
        for p in perms.split():
            owned[tgt].add("%s:%s" % (cls, p))

print("=" * 72)
print("misysdiagnose 域能【写 / 执行】的目标（最有价值的部分）")
print("=" * 72)
KEY = ("write", "create", "append", "unlink", "rename", "setattr", "execute",
       "execute_no_trans", "transition", "set", "relabelto", "mount", "remount")
for tgt in sorted(owned):
    perms = owned[tgt]
    hot = sorted(p for p in perms if any(k in p for k in KEY))
    if not hot:
        continue
    tag = ""
    if any("execute" in p for p in hot):
        tag += " [可执行]"
    if any(p.endswith(":set") for p in hot):
        tag += " [可set属性]"
    if any("create" in p or ":write" in p for p in hot):
        tag += " [可写]"
    print("  %-42s %s" % (tgt, tag))
    print("        " + ", ".join(hot[:14]))

print()
print("=" * 72)
print("typetransition / 域切换相关")
print("=" * 72)
for s in rules:
    one = " ".join(s.split())
    if "typetransition" in one and ("misysdiagnose" in one or "su" in one.lower()):
        print("  " + one[:180])
