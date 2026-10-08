"""Build only; does not launch VR, games, or install anything."""
from pathlib import Path
import subprocess,sys,re,os,json,hashlib
HERE=Path(__file__).resolve().parent
OUT=HERE.parents[1]/".build/scene"
EXPECTED_SHA="c4daa99a3efa4873d7a6e79c78c5c706cbeffd0e497cec7278b847bdc3409102"

def main():
    OUT.mkdir(parents=True,exist_ok=True)
    raw = OUT / "AudioSceneWatcher.compiler.s"
    subprocess.run([
        "clang++", "-target", "x86_64-windows-gnu", "-std=c++17", "-O1",
        "-ffreestanding", "-fno-builtin", "-fno-exceptions", "-fno-rtti",
        "-fno-stack-protector", "-fno-unwind-tables", "-fno-asynchronous-unwind-tables",
        "-mno-stack-arg-probe", "-S", HERE / "scene_watcher.cpp", "-o", raw,
    ], check=True)
    asm = re.sub(r"\b_ZL([A-Za-z0-9_]+)", r".Lprobe_\1", raw.read_text())
    lines = []
    definition = False
    for line in asm.splitlines():
        stripped = line.strip()
        if stripped.startswith(".def"):
            definition = True
            continue
        if definition:
            if stripped.startswith(".endef"): definition = False
            continue
        if stripped.startswith(".lcomm"):
            fields = stripped.split("#", 1)[0].split(None, 1)[1].split(",")
            name, size = fields[0].strip(), int(fields[1])
            alignment = int(fields[2]) if len(fields) > 2 else 8
            if alignment <= 0 or alignment & (alignment - 1):
                raise RuntimeError("Bad compiler local alignment")
            lines += [".p2align " + str(alignment.bit_length() - 1), name + ":", ".zero " + str(size)]
            continue
        if stripped.startswith(".section"):
            if any(word in stripped for word in (".rdata", ".data", ".bss", ".text")): line = "\t.text"
            else: raise RuntimeError("Unexpected compiler section: " + stripped)
        if stripped in (".data", ".bss"): line = "\t.text"
        if ".seh_" in stripped: raise RuntimeError("Unexpected unwind metadata")
        lines.append(line)
    imports = ["GetStdHandle", "WriteFile", "GetCurrentProcessId", "GetCurrentProcess", "GetProcessTimes", "GetSystemTimeAsFileTime", "CreateToolhelp32Snapshot", "Process32FirstW", "Process32NextW", "OpenProcess", "CloseHandle", "QueryFullProcessImageNameW", "WideCharToMultiByte", "LoadLibraryW", "GetProcAddress", "Sleep", "ExitProcess"]
    lines += [".text", ".p2align 3", ".globl import_descriptors", "import_descriptors:",
              " .long ilt-entry+0x1000,0,0,kernel_name-entry+0x1000,iat-entry+0x1000",
              " .long 0,0,0,0,0", 'kernel_name: .asciz "kernel32.dll"', ".p2align 3", "ilt:"]
    lines += [" .quad name_" + name + "-entry+0x1000" for name in imports] + [" .quad 0", "iat:"]
    for name in imports:
        lines += [".globl __imp_" + name, "__imp_" + name + ": .quad name_" + name + "-entry+0x1000"]
    lines += [" .quad 0"]
    for name in imports:
        lines += [".p2align 1", "name_" + name + ': .short 0', ' .asciz "' + name + '"']
    final = OUT / "AudioSceneWatcher.s"
    final.write_text("\n".join(lines) + "\n")
    subprocess.run(["clang", "-target", "x86_64-windows-gnu", "-c", final, "-o", OUT / "AudioSceneWatcher.obj"], check=True)
    subprocess.run([sys.executable, HERE / "make_pe.py", str(OUT / "AudioSceneWatcher")], check=True)
    exe = OUT / "AudioSceneWatcher.exe"
    if hashlib.sha256(exe.read_bytes()).hexdigest()!=EXPECTED_SHA:
        raise RuntimeError("Watcher output differs from reviewed pinned binary; no guard regeneration or installation.")
    manifest={"purpose":"Generic read-only OpenVR Background scene watcher", "exe":str(exe),"bytes":exe.stat().st_size,"sha256":hashlib.sha256(exe.read_bytes()).hexdigest(),"installed_windows_path":"C:\\ALVR\\AudioSceneWatcher.exe","openvr":"IVRApplications_008 FnTable, checked official header", "wire_bytes":456,"poll_ms":200,"selection":"GetCurrentSceneProcessId; never LastRenderer/focus","launch_guard":"Unique existing vrserver.exe and creation FILETIME before VR_Init; no Scene/Overlay/launch APIs", "stdout":"Private fixed-record PIPE; no shared PID file input"}
    (OUT/"watcher-manifest.json").write_text(json.dumps(manifest,indent=2)+"\n")
    print(json.dumps(manifest,indent=2))
if __name__=="__main__": main()
