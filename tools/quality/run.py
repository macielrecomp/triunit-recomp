"""Current-project quality gates. No ROMs needed for fast; no persistent dumps."""
from collections import deque
from contextlib import contextmanager
from pathlib import Path
import argparse
import ctypes
import hashlib
import importlib.util
import json
import os
import platform
import statistics
import subprocess
import sys
import threading
import time
import uuid

ROOT = Path(__file__).resolve().parents[2]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))
import runpy
backend_arguments=runpy.run_path(ROOT/'tools/launcher_backend_build.py')['arguments']
STAGE = ROOT / "build/native/quality"
CURRENT = "tools/mk1/product"
PRODUCT = ROOT / "build/native/manual-temporary-8-20261025/product"
DEFAULT_CC = Path("C:/msys64/mingw64/bin/g++.exe")
ASAN_CC = Path("C:/msys64/clang64/bin/clang++.exe")
OUTPUT_LIMIT = 128 * 1024
WARNINGS_AS_ERRORS = True  # tools/setup/setup.py turns it off: -Werror is dropped from every command


def select_compiler(explicit, sanitizer):
    return explicit if explicit is not None else (ASAN_CC if sanitizer == "address" else DEFAULT_CC)


def sha(path):
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


@contextmanager
def scratch_tree():
    if not __debug__:
        raise RuntimeError("Do not use Python -O: the shared cleanup guard requires assertions")
    guard_path = ROOT / "tools/quality/temporary_tree.py"
    spec = importlib.util.spec_from_file_location("temporary_guard", guard_path)
    guard = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(guard)
    # Check ancestors before creating anything, including the stage itself.
    for path in (STAGE, *STAGE.parents):
        if path.is_symlink() or path.is_junction():
            raise RuntimeError(f"Linked quality path: {path}")
    STAGE.mkdir(parents=True, exist_ok=True)
    scratch = STAGE / ("temporary-quality-" + uuid.uuid4().hex)
    scratch.mkdir()
    cleanup = {}
    try:
        yield scratch, cleanup
    finally:
        cleanup.update(guard.remove_tree(scratch, STAGE))


def process_metrics(process):
    """Windows counters for this process only; no heap/leak claims."""
    if os.name != "nt":
        return {}
    from ctypes import wintypes as w

    class Memory(ctypes.Structure):
        _fields_ = [("cb", w.DWORD), ("faults", w.DWORD)] + [
            (name, ctypes.c_size_t) for name in
            ("peak_rss", "rss", "peak_pool", "pool", "peak_nonpaged", "nonpaged",
             "pagefile", "peak_pagefile")]

    handle = w.HANDLE(int(process._handle))
    memory = Memory()
    memory.cb = ctypes.sizeof(memory)
    result = {}
    psapi = ctypes.WinDLL("psapi", use_last_error=True)
    psapi.GetProcessMemoryInfo.argtypes = [w.HANDLE, ctypes.POINTER(Memory), w.DWORD]
    if psapi.GetProcessMemoryInfo(handle, ctypes.byref(memory), memory.cb):
        result["peak_working_set_bytes"] = int(memory.peak_rss)
    created, ended, kernel, user = (w.FILETIME() for _ in range(4))
    get_times = ctypes.WinDLL("kernel32", use_last_error=True).GetProcessTimes
    get_times.argtypes = [w.HANDLE] + [ctypes.POINTER(w.FILETIME)] * 4
    if get_times(handle, *map(ctypes.byref, (created, ended, kernel, user))):
        ticks = sum((x.dwHighDateTime << 32) | x.dwLowDateTime for x in (kernel, user))
        result["cpu_seconds"] = ticks / 10_000_000
    return result


class ProcessJob:
    """Keep descendants owned after their root exits; close kills the whole job."""
    def __init__(self):
        from ctypes import wintypes as w

        class Limits(ctypes.Structure):
            _fields_ = [("per_process", ctypes.c_int64), ("per_job", ctypes.c_int64),
                        ("flags", w.DWORD), ("minimum_ws", ctypes.c_size_t),
                        ("maximum_ws", ctypes.c_size_t), ("active_limit", w.DWORD),
                        ("affinity", ctypes.c_size_t), ("priority", w.DWORD), ("scheduling", w.DWORD)]

        class Extended(ctypes.Structure):
            _fields_ = [("basic", Limits), ("io_counters", ctypes.c_uint64 * 6),
                        ("process_memory", ctypes.c_size_t), ("job_memory", ctypes.c_size_t),
                        ("peak_process_memory", ctypes.c_size_t), ("peak_job_memory", ctypes.c_size_t)]

        self.kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        self.kernel.CreateJobObjectW.argtypes = [ctypes.c_void_p, w.LPCWSTR]
        self.kernel.CreateJobObjectW.restype = w.HANDLE
        self.kernel.SetInformationJobObject.argtypes = [w.HANDLE, ctypes.c_int, ctypes.c_void_p, w.DWORD]
        self.kernel.AssignProcessToJobObject.argtypes = [w.HANDLE, w.HANDLE]
        self.kernel.TerminateJobObject.argtypes = [w.HANDLE, w.UINT]
        self.kernel.QueryInformationJobObject.argtypes = [w.HANDLE, ctypes.c_int, ctypes.c_void_p,
                                                         w.DWORD, ctypes.c_void_p]
        self.kernel.CloseHandle.argtypes = [w.HANDLE]
        self.handle = self.kernel.CreateJobObjectW(None, None)
        if not self.handle:
            raise ctypes.WinError(ctypes.get_last_error())
        self.Extended = Extended
        limits = Extended()
        limits.basic.flags = 0x2000  # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
        if not self.kernel.SetInformationJobObject(self.handle, 9, ctypes.byref(limits), ctypes.sizeof(limits)):
            self.kernel.CloseHandle(self.handle)
            raise ctypes.WinError(ctypes.get_last_error())

    def assign(self, process):
        if not self.kernel.AssignProcessToJobObject(self.handle, ctypes.c_void_p(int(process._handle))):
            raise ctypes.WinError(ctypes.get_last_error())

    def resume(self, process):
        # Popen closes the initial thread handle. Enumerate its suspended thread
        # so assignment to the job always precedes any executable/user code.
        from ctypes import wintypes as w

        class ThreadEntry(ctypes.Structure):
            _fields_ = [("size", w.DWORD), ("usage", w.DWORD), ("id", w.DWORD),
                        ("owner", w.DWORD), ("base_priority", w.LONG),
                        ("delta_priority", w.LONG), ("flags", w.DWORD)]

        k = self.kernel
        k.CreateToolhelp32Snapshot.argtypes = [w.DWORD, w.DWORD]
        k.CreateToolhelp32Snapshot.restype = w.HANDLE
        k.Thread32First.argtypes = k.Thread32Next.argtypes = [w.HANDLE, ctypes.POINTER(ThreadEntry)]
        k.OpenThread.argtypes = [w.DWORD, w.BOOL, w.DWORD]
        k.OpenThread.restype = w.HANDLE
        k.ResumeThread.argtypes = [w.HANDLE]
        k.ResumeThread.restype = w.DWORD
        snapshot = k.CreateToolhelp32Snapshot(4, 0)  # TH32CS_SNAPTHREAD
        if snapshot == ctypes.c_void_p(-1).value:
            raise ctypes.WinError(ctypes.get_last_error())
        try:
            entry = ThreadEntry()
            entry.size = ctypes.sizeof(entry)
            found = k.Thread32First(snapshot, ctypes.byref(entry))
            while found:
                if entry.owner == process.pid:
                    thread = k.OpenThread(2, False, entry.id)  # THREAD_SUSPEND_RESUME
                    if not thread:
                        raise ctypes.WinError(ctypes.get_last_error())
                    try:
                        if k.ResumeThread(thread) != 1:
                            raise RuntimeError("Unexpected initial thread suspend count")
                        return
                    finally:
                        k.CloseHandle(thread)
                found = k.Thread32Next(snapshot, ctypes.byref(entry))
            raise RuntimeError("Suspended initial thread not found")
        finally:
            k.CloseHandle(snapshot)

    def active(self):
        # JOBOBJECT_BASIC_ACCOUNTING_INFORMATION: four LARGE_INTEGER + four DWORD.
        accounting = (ctypes.c_uint64 * 6)()
        if not self.kernel.QueryInformationJobObject(self.handle, 1, ctypes.byref(accounting),
                                                     ctypes.sizeof(accounting), None):
            raise ctypes.WinError(ctypes.get_last_error())
        return (ctypes.c_uint32 * 12).from_buffer(accounting)[10]

    def peak(self):
        """Peak memory of the largest process the job ever held (cc1plus for a g++ driver)."""
        limits = self.Extended()
        if not self.kernel.QueryInformationJobObject(self.handle, 9, ctypes.byref(limits), ctypes.sizeof(limits), None):
            raise ctypes.WinError(ctypes.get_last_error())
        return int(limits.peak_process_memory)

    def close(self):
        try:
            if not self.kernel.TerminateJobObject(self.handle, 1):
                raise ctypes.WinError(ctypes.get_last_error())
            deadline = time.monotonic() + 10
            while self.active():
                if time.monotonic() >= deadline:
                    raise RuntimeError("Owned job did not become empty")
                time.sleep(0.025)
        finally:
            self.kernel.CloseHandle(self.handle)


def command(args, scratch, timeout=120, env=None, cwd=None, output_limit=OUTPUT_LIMIT, *, unset_env=(), parallel_cpp=True):
    """Bound output in memory and reap the owned process tree on timeout."""
    if not WARNINGS_AS_ERRORS:
        args = [a for a in args if str(a) != "-Werror"]
    if args and Path(str(args[0])).name.lower().startswith(("g++", "clang++", "c++")):
        from tools.setup.paths import with_generated   # includes generated on this machine (generated/<game>/<sha>/)
        args = with_generated(args)
    if parallel_cpp and args and Path(args[0]) == DEFAULT_CC and not unset_env and output_limit == OUTPUT_LIMIT and (cwd is None or Path(cwd) == ROOT):
        from tools.quality.cpp_build import route, build
        selected = route(args)
        if selected is not None:
            return build(**selected, scratch=scratch, env=env, timeout=timeout)
    started = time.perf_counter()
    chunks = deque()
    size = 0
    discarded = 0
    result = {"passed": False, "timed_out": False}
    process = None
    job = None
    reader = None
    metrics = {}
    environment = dict(os.environ, TEMP=str(scratch), TMP=str(scratch),
                       PYTHONDONTWRITEBYTECODE="1")
    for name in unset_env:
        environment.pop(name, None)
    environment.pop("PYTHONOPTIMIZE", None)
    environment.pop("ASAN_SAVE_DUMPS", None)
    # Test policies must not inherit a user's active presentation/audio mode.
    for key in list(environment):
        if key.startswith("MK1_"):
            del environment[key]
    if env:
        environment.update(env)
    try:
        if os.name == "nt":
            job = ProcessJob()
        process = subprocess.Popen(list(map(str, args)), cwd=ROOT if cwd is None else cwd, env=environment,
                                   stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT,
                                   creationflags=4 if job else 0)  # CREATE_SUSPENDED
        if job:
            job.assign(process)
            job.resume(process)

        def read_output():
            nonlocal size, discarded
            while block := process.stdout.read(4096):
                chunks.append(block)
                size += len(block)
                while size > output_limit:
                    removed = chunks.popleft()
                    size -= len(removed)
                    discarded += len(removed)

        reader = threading.Thread(target=read_output, daemon=True)
        reader.start()
        while process.poll() is None:
            metrics.update(process_metrics(process))
            if time.perf_counter() - started > timeout:
                result["timed_out"] = True
                break
            try:
                process.wait(timeout=0.05)
            except subprocess.TimeoutExpired:
                pass
        # A root that exits while leaving children behind must not pass.
        if job and not result["timed_out"]:
            # ASan's symbolizer can finish just after the instrumented root.
            # Require normal shutdown within a bounded grace, then still fail
            # and terminate any surviving descendant (never ignore it).
            shutdown_deadline = time.monotonic() + 0.5
            while job.active() and time.monotonic() < shutdown_deadline:
                time.sleep(0.025)
            result["descendants_after_exit"] = job.active()
        result["passed"] = (process.returncode == 0 and not result["timed_out"]
                            and not result.get("descendants_after_exit", 0))
    except Exception as exc:
        result.update(passed=False, error=repr(exc))
    finally:
        # Always stop descendants before removing scratch, including on metrics,
        # launch, timeout and output-pipe failures. The job survives root exit.
        if job:
            try:
                metrics["peak_process_bytes"] = job.peak()
            except Exception:
                pass
            try:
                job.close()
                result["owned_job_empty"] = True
            except Exception as exc:
                result.update(passed=False, cleanup_error=repr(exc))
        if process and process.poll() is None:
            process.kill()
        if process:
            try:
                process.wait(timeout=10)
                metrics.update(process_metrics(process))
            except Exception as exc:
                result.update(passed=False, cleanup_error=repr(exc))
        if reader:
            reader.join(timeout=5)
            if reader.is_alive():
                result.update(passed=False, cleanup_error="Owned child still holds the output pipe")
        if process and process.stdout and (not reader or not reader.is_alive()):
            process.stdout.close()
        result.update(exit_code=process.returncode if process else None, metrics=metrics,
                      stdout=b"".join(chunks).decode("utf-8", errors="replace"),
                      discarded_output_bytes=discarded)
    result["wall_seconds"] = round(time.perf_counter() - started, 6)
    return result


def all_passed(rows):
    return bool(rows) and all(row.get("passed") is True for row in rows)


def compact(result):
    result = dict(result)
    result["output_tail"] = result.pop("stdout", "")[-4000:]
    return result


def negative_control_detected(result, sanitizer):
    code = result.get("exit_code")
    if (not isinstance(code, int) or code == 0 or result.get("error")
            or result.get("cleanup_error") or result.get("timed_out")
            or result.get("descendants_after_exit", 0)
            or (os.name == "nt" and result.get("owned_job_empty") is not True)):
        return False
    if sanitizer == "undefined":
        return code in (3221225501, -4)  # Windows illegal instruction / POSIX SIGILL.
    return "ERROR: AddressSanitizer: heap-buffer-overflow" in result.get("stdout", "")


def parse_engine_output(output):
    rows = []
    for line in output.splitlines():
        try:
            rows.append(json.loads(line))
        except ValueError:
            pass
    games = [row for row in rows if isinstance(row, dict) and "scope" in row]
    audio = [row for row in rows if isinstance(row, dict) and "audio" in row]
    if len(games) != 1 or len(audio) != 1:
        raise ValueError("Expected exactly one game result and one audio result")
    return games[0], audio[0]


def validate_headless(game, audio):
    main = game.get("main", {})
    return (game.get("scope") == "mk5-main-loop"
            and game.get("stop") == "main-di-limit"
            and game.get("body", {}).get("normal_boot_complete") is True
            and main.get("executed") is True and main.get("calls", 0) > 0
            and main.get("di_limit") == 64
            and main.get("instructions", 0) > 0
            and game.get("reference_model_cycles", 0) > 0
            and game.get("window_presented") is False
            and audio.get("sound_frontier") == "" and audio.get("underruns") == 0
            and audio.get("submitted", 0) > 0
            and audio.get("submitted") == audio.get("completed")
            and audio.get("submitted") == audio.get("pcm_48k_s16_mono"))


def fast(cc, sanitizer, scratch, graphics=False):
    flags = [str(cc), "-std=c++17", "-O1" if sanitizer != "none" else "-O2",
             "-Wall", "-Wextra", "-Werror", "-I" + str(ROOT / "native/include"),
             "-I" + str(ROOT / "native/launcher")]
    env = {"PATH": str(cc.parent) + os.pathsep + os.environ.get("PATH", "")}
    if sanitizer == "address":
        # All diagnostics stay on stderr (captured in bounded memory). Windows
        # ASan has no LeakSanitizer; do not inherit dump/log policies from users.
        env["ASAN_OPTIONS"] = ("halt_on_error=1:abort_on_error=0:exitcode=86:"
                               "detect_leaks=0:disable_coredump=1:symbolize=1")
        symbolizer = cc.parent / "llvm-symbolizer.exe"
        if symbolizer.is_file():
            env["ASAN_SYMBOLIZER_PATH"] = str(symbolizer)
    rows = []
    if sanitizer != "none":
        flags += ["-g", "-fno-omit-frame-pointer", "-fsanitize=" + sanitizer,
                  "-fno-sanitize-recover=all"]
        if sanitizer == "undefined":
            # MinGW can trap inline UBSan checks without a libubsan runtime.
            flags += ["-fsanitize-undefined-trap-on-error", "-fno-sanitize=vptr"]
        probe = scratch / "sanitizer-probe.cpp"
        probe.write_text("int main(){return 0;}\n")
        exe = scratch / "sanitizer-probe.exe"
        build = command(flags + [str(probe), "-o", str(exe)], scratch, env=env)
        row = {"name": "sanitizer-support", **compact(build)}
        if build["passed"]:
            row = {"name": "sanitizer-support", **compact(command([exe], scratch, env=env))}
        rows.append(row)
        if not row["passed"]:
            row["error"] = f"Requested {sanitizer} instrumentation unavailable; no sanitized tests ran"
            return rows
        # A successful link alone does not prove checks are actually active.
        probe.write_text("#include <climits>\nint main(int argc,char**){volatile int n=INT_MAX;n+=argc;(void)n;return 0;}\n"
                         if sanitizer == "undefined" else
                         "#include <cstdlib>\nint main(int argc,char**){volatile char* p=(char*)malloc(argc);p[argc]=0;free((void*)p);return 0;}\n")
        build = command(flags + [str(probe), "-o", str(exe)], scratch, env=env)
        detection = command([exe], scratch, env=env) if build["passed"] else build
        active = build["passed"] and negative_control_detected(detection, sanitizer)
        rows.append({"name": "sanitizer-negative-control", "passed": active,
                     "expected_failure_detected": active, "diagnostic": compact(detection)})
        if not active:
            return rows
    cases = [
        ("controls", [f"{CURRENT}/controls_contract.cpp"], [], []),
        ("player-controls", ["native/tests/mk5_controls_tests.cpp"], [], []),
        ("training-overlay", ["native/tests/mk5_training_tests.cpp"], [], []),
        ("mono-audio", [f"{CURRENT}/audio_contract.cpp"], [], []),
        ("stereo-audio", ["native/tests/mk5_stereo_output_tests.cpp"], [], []),
        ("pacing", ["native/tests/mk5_pacing_tests.cpp"], [], []),
        ("custom-audio", ["native/tests/custom_audio_tests.cpp", "native/src/mk5_remastered_music.cpp"], [], [str(scratch / "custom-audio")]),
        ("timing-edges", ["native/tests/mk5_timing_contract_tests.cpp"], [], []),
        ("mk1-stage-bicubic", ["native/tests/mk5_stage_bicubic_tests.cpp"], [], []),
        ("fighter-bicubic", ["native/tests/mk5_fighter_bicubic_tests.cpp"], [], []),
        ("audio-lifetime", ["native/tests/mk5_audio_lifetime_tests.cpp"], [], []),
        ("game-volume", ["native/tests/mk5_game_volume_tests.cpp"], [], []),
        ("host-gain", ["native/tests/host_gain_tests.cpp"], [], []),
        ("sinc-resampler", ["native/tests/sinc_resampler_tests.cpp"], [], []),
        ("studio-master", ["native/tests/studio_master_tests.cpp"], [], []),
        ("mk2-video", ["native/tests/mk2_video_tests.cpp"], [], []),
        ("mk2-music", ["native/tests/mk2_music_tests.cpp","native/mk2/mk2_music.cpp","native/mk2/mk2_adsp.cpp",
                       "native/mk2/mk2_dcs.cpp","native/src/mk5_program.cpp"], ["-lbcrypt"], [str(scratch / "mk2-music")]),
        ("mk2-scanout", ["native/tests/mk2_scanout_tests.cpp"], [], []),
        ("mk2-scanout-clock", ["native/tests/mk2_scanout_clock_tests.cpp", "native/mk2/mk2_adsp.cpp",
                                "native/mk2/mk2_dcs.cpp", "native/src/mk5_program.cpp"], ["-lbcrypt"], []),
        ("mk2-pixblt", ["native/tests/mk2_pixblt_tests.cpp"], [], []),
        ("mk2-boot-ops", ["native/tests/mk2_boot_ops_tests.cpp", "native/mk2/mk2_adsp.cpp",
                           "native/mk2/mk2_dcs.cpp", "native/src/mk5_program.cpp"], ["-lbcrypt"], [str(ROOT / "roms/mk2/su2.l1")]),
        ("mk2-resumed-dsp", ["native/tests/mk2_adsp_resume_tests.cpp", "native/mk2/mk2_adsp.cpp",
                              "native/mk2/mk2_dcs.cpp", "native/src/mk5_program.cpp"],
                             ["-fno-access-control", "-lbcrypt"], [str(ROOT / "roms/mk2")]),
        ("mk2-foundation", ["native/mk2/mk2_task_batch.cpp", "native/mk2/mk2_executor.cpp", "native/tests/mk2_foundation_tests.cpp", "native/mk2/mk2_program.cpp", "native/mk2/mk2_dcs.cpp", "native/mk2/mk2_adsp.cpp",
                            "native/src/mk5_program.cpp"], ["-lbcrypt"], []),
        ("backend", [f"{CURRENT}/backend_tests.cpp", "native/launcher/launcher_backend.cpp"],
         ["-municode", "-luser32"]+backend_arguments(ROOT), [str(scratch / "backend-root")]),
        ("setup-queue", ["native/tests/setup_queue_tests.cpp", "native/launcher/launcher_backend.cpp"],
         ["-municode", "-luser32"]+backend_arguments(ROOT), [str(scratch / "setup-queue-root")]),
        ("mk3-launcher", ["native/tests/mk3_launcher_tests.cpp", "native/launcher/launcher_backend.cpp"],
         ["-municode", "-luser32"]+backend_arguments(ROOT), [str(scratch / "mk3-launcher-root")]),
        ("launcher-selection", ["native/tests/launcher_selection_tests.cpp", "native/launcher/launcher_renderer.cpp", "native/launcher/launcher_backend.cpp"],
         ["-municode", *["-l"+name for name in ("d3d12","dxgi","d3dcompiler","dxguid","windowscodecs","ole32","uuid","gdi32","user32","shell32","xinput9_1_0")]]+backend_arguments(ROOT), [str(scratch / "selection-root")]),
        ("launcher-art", ["native/tests/launcher_art_tests.cpp", "native/launcher/launcher_renderer.cpp", "native/launcher/launcher_backend.cpp"],
         ["-municode", *["-l"+name for name in ("d3d12","dxgi","d3dcompiler","dxguid","windowscodecs","ole32","uuid","gdi32","user32","shell32","xinput9_1_0")]]+backend_arguments(ROOT), [str(ROOT), str(scratch / "art-root")]),
        ("operator-ui", ["native/tests/launcher_operator_ui_tests.cpp", "native/launcher/launcher_renderer.cpp", "native/launcher/launcher_backend.cpp"],
         ["-municode", *["-l"+name for name in ("d3d12","dxgi","d3dcompiler","dxguid","windowscodecs","ole32","uuid","gdi32","user32","shell32","xinput9_1_0")]]+backend_arguments(ROOT), [str(scratch / "operator-ui-root")]),
    ]
    if graphics:
        cases = [
            ("scalefx-window", ["native/tests/mk5_scalefx_window_tests.cpp", "native/src/mk5_window.cpp"], ["-DUNICODE", "-D_UNICODE", "-ld3d12", "-ldxgi", "-ld3dcompiler", "-ldxguid", "-lgdi32", "-lwindowscodecs", "-lole32", "-luuid"], ["effects"]),
            ("stage-bicubic-window", ["native/tests/mk5_scalefx_window_tests.cpp", "native/src/mk5_window.cpp"], ["-DUNICODE", "-D_UNICODE", "-ld3d12", "-ldxgi", "-ld3dcompiler", "-ldxguid", "-lgdi32", "-lwindowscodecs", "-lole32", "-luuid"], ["stage"]),
            ("fighter-bicubic-window", ["native/tests/mk5_scalefx_window_tests.cpp", "native/src/mk5_window.cpp"], ["-DUNICODE", "-D_UNICODE", "-ld3d12", "-ldxgi", "-ld3dcompiler", "-ldxguid", "-lgdi32", "-lwindowscodecs", "-lole32", "-luuid"], ["fighter"]),
            ("hdr-window", ["native/tests/mk5_scalefx_window_tests.cpp", "native/src/mk5_window.cpp"], ["-DUNICODE", "-D_UNICODE", "-ld3d12", "-ldxgi", "-ld3dcompiler", "-ldxguid", "-lgdi32", "-lwindowscodecs", "-lole32", "-luuid"], ["hdr"]),
            ("frame-window", ["native/tests/mk5_scalefx_window_tests.cpp", "native/src/mk5_window.cpp"], ["-DUNICODE", "-D_UNICODE", "-ld3d12", "-ldxgi", "-ld3dcompiler", "-ldxguid", "-lgdi32", "-lwindowscodecs", "-lole32", "-luuid"], ["frame"]),
            ("training-window", ["native/tests/mk5_scalefx_window_tests.cpp", "native/src/mk5_window.cpp"], ["-DUNICODE", "-D_UNICODE", "-ld3d12", "-ldxgi", "-ld3dcompiler", "-ldxguid", "-lgdi32", "-lwindowscodecs", "-lole32", "-luuid"], ["training"]),
            ("wide-window", ["native/tests/mk5_scalefx_window_tests.cpp", "native/src/mk5_window.cpp"], ["-DUNICODE", "-D_UNICODE", "-ld3d12", "-ldxgi", "-ld3dcompiler", "-ldxguid", "-lgdi32", "-lwindowscodecs", "-lole32", "-luuid"], ["wide"]),
            ("scalefx", ["native/tests/mk5_scalefx_tests.cpp"], ["-ld3d12", "-ldxgi", "-ld3dcompiler", "-ldxguid"], []),
            ("smaa", ["native/tests/mk5_smaa_tests.cpp"], ["-ld3d12", "-ldxgi", "-ld3dcompiler", "-ldxguid"], []),
            ("presentation", [f"{CURRENT}/presentation_contract.cpp", "native/src/mk5_window.cpp"],
             ["-DUNICODE", "-D_UNICODE", "-ld3d12", "-ldxgi", "-ld3dcompiler", "-ldxguid", "-lgdi32", "-lwindowscodecs", "-lole32", "-luuid"], []),
            ("renderer", ["native/launcher/renderer_contract.cpp", "native/launcher/launcher_renderer.cpp"],
             ["-Wno-missing-field-initializers", "-ld3d12", "-ldxgi", "-ld3dcompiler", "-ldxguid",
              "-lwindowscodecs", "-lole32", "-luuid", "-lgdi32", "-luser32"], []),
            ("launcher-art-render", ["native/tests/launcher_art_tests.cpp", "native/launcher/launcher_renderer.cpp", "native/launcher/launcher_backend.cpp"],
             ["-municode", *["-l"+name for name in ("d3d12","dxgi","d3dcompiler","dxguid","windowscodecs","ole32","uuid","gdi32","user32","shell32","xinput9_1_0")]]+backend_arguments(ROOT), [str(ROOT), str(scratch / "art-render-root"), "render"]),
        ]
    else:
        cases.append(("wide-layer", ["native/tests/mk5_wide_layer_tests.cpp"], [], []))
        mk2_roms = ROOT / "roms/mk2"
        mk2_manifest = json.loads((ROOT / "tools/mk2/roms-l31.json").read_text(encoding="utf-8"))
        if all((mk2_roms / row["path"]).is_file() for row in mk2_manifest["roms"] if row["region"] != "dcs"):
            cases.append(("mk2-remastered-blood", ["native/tests/mk2_remastered_blood_tests.cpp",
                          "native/mk2/mk2_program.cpp", "native/src/mk5_program.cpp"],
                          ["-lbcrypt"], [str(mk2_roms)]))
        mk1_roms = ROOT / "roms/mk1"
        mk1_manifest = json.loads((ROOT / "tools/mk1/roms-l1.json").read_text(encoding="utf-8"))
        if all((mk1_roms / row["path"]).is_file() for row in mk1_manifest["roms"]):
            cases.append(("mk1-remastered-blood", ["native/tests/mk5_remastered_blood_tests.cpp", "native/src/mk5_live_video.cpp",
                          "native/src/mk5_scanout.cpp", "native/src/mk5_irq.cpp", "native/src/mk5_program.cpp"],
                          ["-fno-access-control", "-lbcrypt"], [str(mk1_roms)]))
        from tools.mk3 import roms as mk3_roms   # the ROM_LOAD32_BYTE "video" rows (tools/mk3/reference_dma.py)
        mk3_graphics = [(ROOT / "roms/mk3" / row["path"], row["offset_bytes"], row["size"]) for row in mk3_roms.definition() if row["region"] == "video"]
        if mk3_graphics and all(path.is_file() for path, _, _ in mk3_graphics) and (ROOT / "roms/mk3/mk321u54.bin").is_file():
            (scratch / "mk3-gfx.txt").write_text("".join(f"{o} {s} {p}\n" for p, o, s in mk3_graphics), encoding="utf-8")
            cases.append(("mk3-remastered-blood", ["native/tests/mk3_remastered_blood_tests.cpp",
                          "native/mk3/mk3_program.cpp", "native/src/mk5_program.cpp"],
                          ["-lbcrypt"], [str(ROOT / "roms/mk3"), str(scratch / "mk3-gfx.txt")]))
    for name, sources, extra, arguments in cases:
        exe = scratch / (name + ".exe")
        build = command(flags + [str(ROOT / path) for path in sources] + extra
                        + ["-o", str(exe)], scratch, timeout=240 if name == "mk2-foundation" else 120, env=env)
        row = {"name": name, "build": compact(build), "passed": False,
               "sources": {path: sha(ROOT / path) for path in sources}}
        if build["passed"]:
            run = command([exe, *arguments], scratch, timeout=90 if name=="scalefx-window" else 30, env=env)
            row.update(run=compact(run), passed=run["passed"])
            if name == "scalefx-window":
                # Use a fresh process for the remaining DX12 queues.
                crt = command([exe, "crt"], scratch, timeout=90, env=env)
                row.update(crt_run=compact(crt), passed=row["passed"] and crt["passed"])
        rows.append(row)
        print(f"{'PASS' if row['passed'] else 'FAIL'} {name}", flush=True)
    if not graphics:
        for name in ("test_foundation.py", "test_executor.py", "test_reference.py", "test_adsp.py", "test_dsp_branch_closure.py", "test_attract_dma.py", "test_video_irq.py", "test_dispatcher.py", "test_task_resume.py", "test_task_queue.py", "test_task_palette.py", "test_task_draw.py", "test_task_batch.py", "test_coin_queue.py", "test_coin_accounting.py", "test_coin_pair.py", "test_start_wait.py", "test_next_wait.py", "test_prestart_wait.py", "test_start_input.py", "test_start_release.py", "test_selection.py", "test_first_fight.py", "test_architectural_dispatch.py", "test_replay.py", "test_coverage.py", "test_replay_state.py", "test_qualification.py", "test_live_suspend.py", "test_save_state.py", "test_first_fight_controls.py", "test_first_fight_ops.py", "test_first_fight_video.py", "test_first_fight_adsp.py", "test_stream_transport.py", "test_native_json.py", "test_native.py", "test_scanout_reference.py", "test_native_settings.py", "test_dac_timeline.py", "test_dac_reference.py", "test_audio.py", "test_batch_cli.py", "test_continuation.py", "test_native_continuation.py", "test_profile.py", "test_valid_cmos_boot.py", "test_host_controls.py", "test_host_runtime.py", "test_native_dependencies.py", "test_compact_dsp.py", "test_offline_decode.py", "test_host_resume.py", "test_profile_session.py", "test_first_boot_sport.py", "test_launcher_lifecycle.py", "test_native_launcher.py", "test_static_cfg.py", "test_valid_cmos_startup.py", "test_valid_cmos_draw.py", "test_valid_cmos_draw_generator.py", "test_valid_host_session.py", "test_host_close.py", "test_valid_host_edges.py", "test_profile_cpp.py", "test_cmos_file_cpp.py", "test_cmos_rename_retry.py"):
            script = ROOT / "tools/mk2" / name
            classes = ["GuardTests"] if name == "test_offline_decode.py" else ["ExternalIrqTests","PairWindowTests"] if name == "test_coin_pair.py" else ["NativeSettingsTests"] if name == "test_native_settings.py" else []
            result = command([sys.executable, "-B", script, *classes], scratch,
                             timeout=600 if name == "test_reference.py" else
                             # These compile the generated whole-program closure (executor from reset).
                             900 if name in ("test_executor.py", "test_native.py", "test_profile.py", "test_profile_session.py",
                                             "test_valid_cmos_boot.py", "test_valid_cmos_startup.py") else
                             480 if name == "test_live_suspend.py" else
                             900 if name == "test_save_state.py" else
                             600 if name == "test_host_runtime.py" else
                             300 if name == "test_valid_cmos_startup.py" else
                             300 if name == "test_first_fight_video.py" else
                             240 if name == "test_architectural_dispatch.py" else
                             180 if name in ("test_host_close.py", "test_profile_cpp.py", "test_native_launcher.py") else
                             120 if name in ("test_attract_dma.py", "test_valid_cmos_boot.py", "test_profile_session.py",
                                             "test_first_boot_sport.py", "test_launcher_lifecycle.py",
                                             "test_continuation.py") else
                             120 if name in ("test_start_release.py", "test_first_fight_ops.py", "test_valid_cmos_draw_generator.py", "test_valid_host_edges.py",
                                             "test_first_fight_adsp.py") else 30)
            row = {"name": "mk2-" + name.removesuffix(".py"), "passed": result["passed"],
                   "script_sha256": sha(script), "run": compact(result)}
            rows.append(row)
            print(f"{'PASS' if row['passed'] else 'FAIL'} {row['name']}", flush=True)
        script = ROOT / "tools/setup/test_setup.py"
        result = command([sys.executable, "-B", script], scratch, timeout=60)
        rows.append({"name": "setup-cli", "passed": result["passed"], "script_sha256": sha(script), "run": compact(result)})
        print(f"{'PASS' if result['passed'] else 'FAIL'} setup-cli", flush=True)
    return rows


def smoke(scratch):
    launcher = ROOT / "triunit.exe"
    paths = [launcher, PRODUCT / "mk5-native-audio.exe", PRODUCT / "manifest.json"]
    identities = {str(p.relative_to(ROOT)): sha(p) for p in paths}
    rows = []
    for mode, frames in (("original", 113), ("remastered", 127)):
        data = scratch / mode
        data.mkdir()
        run = command([launcher, "--smoke-frames", str(frames), "--game-mode", mode,
                       "--data-root", data], scratch, timeout=40)
        row = {"name": mode, **compact(run)}
        if run["passed"]:
            try:
                latest = json.loads((data / "logs/latest.json").read_text(encoding="utf-8-sig"))
                summary_path = Path(latest["summary"]).resolve()
                if not summary_path.is_relative_to(data.resolve()):
                    raise ValueError("Smoke-test summary must belong to this test")
                summary = json.loads(summary_path.read_text(encoding="utf-8-sig"))
                audio = summary["audio"]
                row["passed"] = (summary["exit_code"] == 0 and summary["cmos_committed"] is True
                                 and not summary["logger_failed"]
                                 and summary["game"]["stop"] == "window-frame-limit"
                                 and summary["game"]["window"]["frames"] == frames
                                 and audio["music_mode"] == mode
                                 and audio["submitted"] == audio["completed"]
                                 and audio["submitted"] > 0 and audio["underruns"] == 0
                                 and audio["sound_frontier"] == "")
                row.update(frames=frames, audio={k: audio[k] for k in
                           ("music_mode", "submitted", "completed", "underruns", "sound_frontier")},
                           stop=summary["game"]["stop"], cmos_committed=summary["cmos_committed"],
                           producer_intervals=summary["game"]["window"]["presentation"].get("producer_intervals"))
            except (OSError, ValueError, KeyError) as exc:
                row.update(passed=False, error=repr(exc))
        rows.append(row)
        print(f"{'PASS' if row['passed'] else 'FAIL'} smoke {mode}", flush=True)
    if any(sha(ROOT / path) != identity for path, identity in identities.items()):
        raise RuntimeError("Product changed during smoke")
    return rows, identities


def headless(scratch, repetitions):
    exe = PRODUCT / "mk5-native-audio.exe"
    from tools.setup import mk1 as setup_mk1
    seed = setup_mk1.seed_path()   # CMOS seed written by the setup
    arguments = [exe, "--roms", ROOT / "roms", "--stage", "main-loop",
                 "--cmos-input", seed, "--irq-pending", "0x400",
                 "--raster-next-line", "1", "--raster-quanta", "6325",
                 "--body-sound-status-byte", "128", "--main-slices", "16",
                 "--main-di-limit", "64"]
    for prefix in ("", "boot-", "task-", "body-", "main-"):
        arguments += [f"--{prefix}dsw-word", "65279"]
    for prefix in ("", "body-", "active-"):
        arguments += [f"--{prefix}irq-in01", "0xffffffff",
                      f"--{prefix}irq-in2-dsw", "0xfeffffff"]
    identity = {"engine_sha256": sha(exe), "seed_sha256": sha(seed),
                "arguments": list(map(str, arguments[1:])),
                "scope": "bounded boot/main-loop, no window; real audio device, no simulated inputs"}
    rows = []
    for index in range(repetitions):
        run = command(arguments, scratch, timeout=45,
                      env={"MK1_MUSIC_MODE": "original", "MK1_AUDIO_LATENCY": "safe"})
        row = {"name": f"headless-{index + 1}", **compact(run)}
        if run["passed"]:
            try:
                game, audio = parse_engine_output(run["stdout"])
                row["passed"] = validate_headless(game, audio)
                row["game"] = {key: game.get(key) for key in
                               ("scope", "stop", "main", "reference_model_cycles")}
                row["audio"] = {key: audio.get(key) for key in
                                ("submitted", "completed", "underruns", "sound_frontier")}
                row.pop("output_tail", None)
            except ValueError as exc:
                row.update(passed=False, error=str(exc))
        rows.append(row)
        print(f"{'PASS' if row['passed'] else 'FAIL'} {row['name']}", flush=True)
    if sha(exe) != identity["engine_sha256"] or sha(seed) != identity["seed_sha256"]:
        raise RuntimeError("Product or seed changed during quality run")
    return rows, identity


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("suite", nargs="?", default="fast",
                        choices=("fast", "integration", "smoke", "headless", "benchmark"))
    parser.add_argument("--compiler", type=Path,
                        help="explicit compiler; default is CLANG64 for address, MinGW GCC otherwise")
    parser.add_argument("--sanitizer", choices=("none", "undefined", "address"), default="none")
    parser.add_argument("--repetitions", type=int, default=5)
    args = parser.parse_args()
    args.compiler = select_compiler(args.compiler, args.sanitizer)
    if args.sanitizer != "none" and args.suite != "fast":
        parser.error("--sanitizer instruments only fast source builds, not the installed product")
    if not 3 <= args.repetitions <= 20:
        parser.error("--repetitions must be between 3 and 20")
    receipt = {"passed": False, "suite": args.suite, "sanitizer": args.sanitizer,
               "utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
               "host": platform.platform(), "python": platform.python_version(),
               "cases": [], "runner_sha256": sha(Path(__file__)),
               "certifies_arcade_fidelity": False, "certifies_no_memory_leaks": False}
    try:
        with scratch_tree() as (scratch, cleanup):
            receipt["cleanup"] = cleanup
            if args.suite in ("fast", "integration"):
                receipt["compiler_sha256"] = sha(args.compiler)
                receipt["compiler_path"] = str(args.compiler)
                if args.sanitizer == "address":
                    runtime = args.compiler.parent / "libclang_rt.asan_dynamic-x86_64.dll"
                    receipt["asan_runtime"] = {"path": str(runtime), "sha256": sha(runtime)} if runtime.is_file() else None
                    receipt["leak_detection_supported"] = False
                receipt["compiler"] = compact(command([args.compiler, "--version"], scratch))
                # Include direct and embedded test dependencies in compact provenance.
                source_paths = list((ROOT / "native/include").glob("*.hpp"))
                source_paths.extend((ROOT / "native/mk2").glob("*.hpp"))
                source_paths.extend((ROOT / "native/mk2").glob("*.inc"))
                source_paths.extend(ROOT / "tools/mk2" / name for name in
                                    ("run.py", "reference.py", "generate_adsp.py", "generate_video_irq.py",
                                     "generate_architectural.py", "replay.py", "replay_audio.py", "replay_reference.py",
                                     "generate_dispatcher.py", "generate_task_resume.py", "generate_task_queue.py", "generate_task_palette.py", "generate_task_draw.py"))
                source_paths += [ROOT / p for p in
                                 ("native/launcher/launcher_backend.hpp", "native/launcher/launcher_renderer.hpp",
                                  "native/src/mk5_audio_output.cpp", "native/tests/mk5_audio_queue_contract.cpp")]
                receipt["dependencies"] = {p.relative_to(ROOT).as_posix(): sha(p) for p in source_paths}
                receipt["cases"] = fast(args.compiler, args.sanitizer, scratch, args.suite == "integration")
            elif args.suite in ("headless", "benchmark"):
                rows, identity = headless(scratch, args.repetitions if args.suite == "benchmark" else 1)
                receipt.update(cases=rows, workload=identity)
                if args.suite == "benchmark" and all_passed(rows):
                    receipt["measurement"] = {
                        "wall_median_seconds": statistics.median(r["wall_seconds"] for r in rows),
                        "wall_min_seconds": min(r["wall_seconds"] for r in rows),
                        "wall_max_seconds": max(r["wall_seconds"] for r in rows),
                        "cpu_median_seconds": statistics.median(r["metrics"]["cpu_seconds"] for r in rows),
                        "peak_working_set_bytes": max(r["metrics"].get("peak_working_set_bytes", 0) for r in rows),
                        "scope": "includes boot, ROM I/O and audio drain; excludes GPU and child processes; no FPS or leak certification"}
            else:
                receipt["cases"], receipt["artifacts"] = smoke(scratch)
        receipt["passed"] = all_passed(receipt["cases"]) and cleanup.get("exists_after") is False
    except Exception as exc:
        receipt.update(passed=False, error=repr(exc))
    # No write if stage itself is linked or unavailable.
    if STAGE.is_dir() and not any(p.is_symlink() or p.is_junction() for p in (STAGE, *STAGE.parents)):
        path = STAGE / (args.suite + ("-" + args.sanitizer if args.sanitizer != "none" else "") + ".json")
        if path.is_symlink() or path.is_junction():
            raise RuntimeError(f"Linked receipt path: {path}")
        path.write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
        print(f"Receipt: {path}")
    print("PASS" if receipt["passed"] else "FAIL")
    if "error" in receipt:
        print(receipt["error"])
    return 0 if receipt["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
