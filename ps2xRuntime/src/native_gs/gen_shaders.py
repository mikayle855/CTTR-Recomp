# gen_shaders.py: shaders/*.vert|frag -> shaders.h (NUL-terminated GLSL sources kXxxVertGlsl / kXxxFragGlsl, as
# upstream's port/tools/undefined.py makes them for the GL back end). Run after changing a shader.
from pathlib import Path
here = Path(__file__).resolve().parent
out = ["/* generated from shaders/ by gen_shaders.py */\n"]
for src in sorted((here / "shaders").glob("*")):
    if src.suffix not in (".vert", ".frag"):
        continue
    name = "k" + src.stem.capitalize() + src.suffix[1:].capitalize() + "Glsl"
    data = src.read_bytes() + b"\0"
    out.append(f"static const unsigned char {name}[] = {{" + ",".join(str(b) for b in data) + "};\n")
(here / "shaders.h").write_text("".join(out))
print("shaders.h:", len(out) - 1, "shaders")
