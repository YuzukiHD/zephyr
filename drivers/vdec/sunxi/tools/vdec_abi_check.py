#!/usr/bin/env python3
# Copyright (c) 2026 Yuzuki Tsuru
# SPDX-License-Identifier: Apache-2.0
"""
Compare the layout of the structures in ve_abi.h with the headers the prebuilt
decoder archive was built against.

usage: vdec_abi_check.py <libcedarc dir> [cross-compiler prefix]

Every struct pair listed in PAIRS is compiled once from each header set into a
table of member offsets and sizes; the two tables must be identical. The
archive tree is only read, nothing from it is copied.
"""
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
OURS = os.path.join(HERE, "..", "ve_abi.h")

# our struct -> archive typedef name
PAIRS = [
    ("ve_mem_ops", "struct ScMemOpsS"),
    ("ve_config", "VeConfig"),
    ("ve_user_iommu_param", "struct user_iommu_param"),
    ("ve_ops", "VeOpsS"),
    ("ve_stream_info", "VideoStreamInfo"),
    ("ve_vconfig", "VConfig"),
    ("ve_stream_data", "VideoStreamDataInfo"),
    ("ve_mv_info", "VIDEO_FRM_MV_INFO"),
    ("ve_frame_status", "VIDEO_FRM_STATUS_INFO"),
    ("ve_picture", "VideoPicture"),
    ("ve_fbm_buf_info", "FbmBufInfo"),
    ("ve_fbm_info", "VideoFbmInfo"),
    ("ve_fbm_node_flag", "FrameNodeFlag"),
    ("ve_fbm_node", "FrameNode"),
    ("ve_fbm_create_info", "FbmCreateInfo"),
    ("ve_fbm", "Fbm"),
    ("ve_sbm_config", "SbmConfig"),
    ("ve_sbm", "SbmInterface"),
    ("ve_decoder_perf_info", "VDecodePerformaceInfo"),
    ("ve_decoder_if", "DecoderInterface"),
    ("ve_engine", "VideoEngine"),
]


def strip(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def members(body):
    """Member names of a struct body, in order"""
    names = []
    body = re.sub(r"\{[^{}]*\}", "", body)  # nested bodies are not members
    for decl in body.split(";"):
        decl = decl.strip()
        if not decl or decl.startswith("#"):
            continue
        decl = re.sub(r"^\s*#.*$", "", decl, flags=re.M).strip()
        m = re.search(r"\(\s*\*\s*(\w+)\s*\)", decl)
        if m:
            names.append(m.group(1))
            continue
        decl = re.sub(r"\[[^\]]*\]", "", decl)
        decl = re.sub(r":\s*\d+", "", decl)
        for part in decl.split(","):
            m = re.search(r"(\w+)\s*$", part.strip())
            if m:
                names.append(m.group(1))
    return names


def struct_body(text, name):
    m = re.search(r"struct\s+" + re.escape(name) + r"\s*\{", text)
    if not m:
        return None
    i, depth = m.end(), 1
    while depth:
        depth += {"{": 1, "}": -1}.get(text[i], 0)
        i += 1
    return text[m.end():i - 1]


def preprocess(cc, flags, src):
    out = subprocess.run([cc, "-E", "-P"] + flags + ["-x", "c", "-"], input=src, text=True,
                         capture_output=True)
    if out.returncode:
        sys.exit(out.stderr)
    return out.stdout


def vendor_struct(text, ctype):
    if ctype.startswith("struct "):
        return struct_body(text, ctype.split()[1])
    # typedef struct TAG Alias;  struct TAG { ... };
    m = re.search(r"typedef\s+struct\s+(\w+)\s+" + re.escape(ctype) + r"\s*;", text)
    if m:
        body = struct_body(text, m.group(1))
        if body is not None:
            return body
    # typedef struct [TAG] { ... } ctype;
    for m in re.finditer(r"typedef\s+struct\s*(\w*)\s*\{", text):
        i, depth = m.end(), 1
        while depth:
            depth += {"{": 1, "}": -1}.get(text[i], 0)
            i += 1
        if re.match(r"\s*" + re.escape(ctype) + r"\s*;", text[i:]):
            return text[m.end():i - 1]
    return None


def table(headers, ctype_fn, pairs, side):
    lines = ["#include <stddef.h>"] + headers
    lines.append("const unsigned layout[] __attribute__((section(\".rodata\"))) = {")
    for ours, vend, ms in pairs:
        ct = ours if side == "ours" else vend
        ct = "struct " + ct if side == "ours" else ct
        lines.append("  sizeof(%s)," % ct)
        for m_ours, m_vend in ms:
            lines.append("  offsetof(%s, %s)," % (ct, m_ours if side == "ours" else m_vend))
    lines.append("};")
    return "\n".join(lines) + "\n"


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    root = sys.argv[1]
    prefix = sys.argv[2] if len(sys.argv) > 2 else os.environ.get("CROSS_COMPILE", "")
    cc = prefix + "gcc"
    arch = ["-march=rv32imafdc_zicsr", "-mabi=ilp32d"]
    inc = [os.path.join(root, d) for d in ("include", "vdecoder/include", "vdecoder",
                                            "base/include")]
    flags = ["-I" + d for d in inc]
    # the archive headers want a POSIX semaphore type the bare-metal sysroot lacks;
    # none of the compared structures contains one
    stubdir = tempfile.mkdtemp()
    open(os.path.join(stubdir, "semaphore.h"), "w").write("typedef struct { int x; } sem_t;\n")
    flags.append("-I" + stubdir)
    vhdrs = ["vdecoder.h", "veInterface.h", "fbm.h", "sbmInterface.h", "videoengine.h"]
    vinc = ["#include <%s>" % h for h in vhdrs]

    vtext = strip(preprocess(cc, arch + flags, "\n".join(vinc) + "\n"))
    otext = strip(open(OURS).read())

    pairs, bad = [], False
    for ours, vend in PAIRS:
        ob, vb = struct_body(otext, ours), vendor_struct(vtext, vend)
        if ob is None or vb is None:
            print("missing struct: %s / %s" % (ours, vend))
            bad = True
            continue
        om, vm = members(ob), members(vb)
        if len(om) != len(vm):
            print("%s: %d members, archive %s has %d" % (ours, len(om), vend, len(vm)))
            bad = True
            continue
        pairs.append((ours, vend, list(zip(om, vm))))
    if bad:
        sys.exit(1)

    with tempfile.TemporaryDirectory() as tmp:
        bins = {}
        for side, headers, extra in (("ours", ['#include "ve_abi.h"'], ["-I" + os.path.dirname(OURS)]),
                                     ("vendor", vinc, flags)):
            c = os.path.join(tmp, side + ".c")
            open(c, "w").write(table(headers, None, pairs, side))
            o = os.path.join(tmp, side + ".o")
            r = subprocess.run([cc, "-c", "-O0", "-ffreestanding"] + arch + extra + [c, "-o", o],
                               capture_output=True, text=True)
            if r.returncode:
                sys.exit(r.stderr)
            b = os.path.join(tmp, side + ".bin")
            subprocess.run([prefix + "objcopy", "-O", "binary", "-j", ".rodata", o, b], check=True)
            bins[side] = open(b, "rb").read()
        ours_t = [int.from_bytes(bins["ours"][i:i + 4], "little") for i in range(0, len(bins["ours"]), 4)]
        vend_t = [int.from_bytes(bins["vendor"][i:i + 4], "little") for i in range(0, len(bins["vendor"]), 4)]
        k, fails = 0, 0
        for ours, vend, ms in pairs:
            ok = ours_t[k] == vend_t[k]
            print("%-24s size %4d vs %4d %s" % (ours, ours_t[k], vend_t[k], "ok" if ok else "MISMATCH"))
            fails += not ok
            k += 1
            for m_ours, m_vend in ms:
                if ours_t[k] != vend_t[k]:
                    print("    %-30s offset %4d vs %4d (%s)" % (m_ours, ours_t[k], vend_t[k], m_vend))
                    fails += 1
                k += 1
        print("layout check:", "FAILED (%d)" % fails if fails else "all structures match")
        sys.exit(1 if fails else 0)


main()
