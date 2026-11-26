"""Verify MK2 L3.1 ROMs and exercise the bounded native foundation; no game launch."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tools.quality import run as quality

MANIFEST = Path(__file__).with_name("roms-l31.json")
SOURCES = ["native/mk2/mk2_task_batch.cpp", "native/tests/mk2_foundation_tests.cpp", "native/mk2/mk2_program.cpp", "native/mk2/mk2_dcs.cpp", "native/mk2/mk2_adsp.cpp", "native/src/mk5_program.cpp"]
HEADERS = ["native/mk2/mk2_task_batch.hpp", "native/mk2/mk2_task_batch.inc", "native/mk2/mk2_task_draw.inc", "native/mk2/mk2_task_palette.inc", "native/mk2/mk2_task_queue.inc", "native/mk2/mk2_task_resume.inc", "native/mk2/mk2_dispatcher.inc", "native/mk2/mk2_program.hpp", "native/mk2/mk2_reset.hpp",
           "native/mk2/mk2_board.hpp", "native/mk2/mk2_video.hpp", "native/mk2/mk2_pixblt.hpp", "native/mk2/mk2_widescreen.hpp",
           "native/include/mk5_stage_bicubic.hpp", "native/include/mk5_fighter_bicubic.hpp", "native/include/mk5_fighter_video.hpp", "native/include/mk5_blood_shade.hpp", "native/include/mk5_wide.hpp",
           "native/mk2/mk2_boot_ops.hpp",
           "native/mk2/mk2_protection.hpp", "native/mk2/mk2_dcs.hpp", "native/mk2/mk2_adsp.hpp",
           "native/mk2/mk2_adsp_boot.inc", "native/mk2/mk2_video_irq.inc", "native/include/mk5_program.hpp"]
VERIFIERS = ["tools/mk2/batch_reference.py", "tools/mk2/test_task_batch.py", "tools/mk2/stream_transport.py", "tools/mk2/test_stream_transport.py", "tools/mk2/generate_task_batch.py", "tools/mk2/generate_task_draw.py", "tools/mk2/test_task_draw.py", "tools/mk2/generate_task_palette.py", "tools/mk2/test_task_palette.py", "tools/mk2/generate_task_queue.py", "tools/mk2/test_task_queue.py", "tools/mk2/generate_task_resume.py", "tools/mk2/test_task_resume.py", "tools/mk2/generate_dispatcher.py", "tools/mk2/test_dispatcher.py", "tools/mk2/run.py", "tools/mk2/reference.py", "tools/mk2/test_reference.py",
             "native/tests/mk2_adsp_resume_tests.cpp",
             "native/tests/mk2_boot_ops_tests.cpp",
             "native/tests/mk2_video_tests.cpp",
             "tools/quality/run.py", "tools/mk2/test_foundation.py", "docs/investigation/sources/dcs.cpp",
             "docs/investigation/sources/midtunit.cpp",
             "docs/investigation/sources/midtunit_m.cpp", "docs/investigation/sources/midtunit_v.cpp",
             "docs/investigation/decoder-research/upstream/34010ops.hxx",
             "docs/investigation/decoder-research/upstream/34010gfx.hxx"]

VERIFIERS += ["tools/launcher_backend_build.py", "native/launcher/native_roms.inc", "native/launcher/mk2_native_starter.cpp", "native/tests/mk2_native_launcher_tests.cpp", "tools/mk2/test_native_launcher.py"]
VERIFIERS += ["tools/mk2/generate_adsp.py", "tools/mk2/test_adsp.py",
              "tools/mk2/dsp_branch_closure.json", "tools/mk2/test_dsp_branch_closure.py",
              "native/tests/mk2_attract_dma_tests.cpp", "tools/mk2/test_attract_dma.py", "tools/mk2/probe_attract_dma.py",
              "tools/mk2/coin_pair_dsp_path.json",
              "tools/mk2/generate_video_irq.py", "tools/mk2/test_video_irq.py",
              "docs/investigation/decoder-research/build.json",
              "tools/setup/decoder.py",
              "docs/investigation/decoder-research/disassemble.cpp",
              "docs/investigation/decoder-research/upstream/34010dsm.cpp",
              "docs/investigation/decoder-research/upstream/34010dsm.h"]
VERIFIERS += ["docs/investigation/decoder-research/adsp-sources.json"]
VERIFIERS += ["docs/investigation/decoder-research/field-sources.json",
              "docs/investigation/decoder-research/upstream/34010ops.h",
              "docs/investigation/decoder-research/upstream/34010fld.hxx"]
VERIFIERS += ["tools/mk2/native_json.py", "tools/mk2/test_native_json.py",
              "tools/mk2/offline_decode.py", "tools/mk2/test_offline_decode.py"]
VERIFIERS += ["tools/mk2/packed_trace.py", "tools/mk2/generate_coin_queue.py", "tools/mk2/test_coin_queue.py", "tools/mk2/coin_queue_path.json"]
HEADERS += ["native/mk2/mk2_coin_queue.inc"]
VERIFIERS += ["tools/mk2/generate_coin_accounting.py", "tools/mk2/test_coin_accounting.py", "tools/mk2/coin_accounting_path.json"]
HEADERS += ["native/mk2/mk2_coin_accounting.inc", "native/mk2/mk2_coin_pair.inc"]
VERIFIERS += ["tools/mk2/generate_coin_pair.py", "tools/mk2/test_coin_pair.py", "tools/mk2/coin_pair_path.json", "tools/mk2/physical_fetch.py"]
HEADERS += ["native/mk2/mk2_start_wait.inc", "native/mk2/mk2_next_wait.inc"]
HEADERS += ["native/mk2/mk2_prestart_wait.inc", "native/mk2/mk2_prestart_wait_bounds.inc"]
VERIFIERS += ["tools/mk2/generate_prestart_wait.py", "tools/mk2/test_prestart_wait.py", "tools/mk2/prestart_wait_path.json"]
VERIFIERS += ["tools/mk2/generate_start_input.py", "tools/mk2/test_start_input.py", "tools/mk2/start_input_path.json"]
HEADERS += ["native/mk2/mk2_start_release.inc"]
VERIFIERS += ["tools/mk2/generate_start_release.py", "tools/mk2/test_start_release.py", "tools/mk2/start_release_path.json"]
VERIFIERS += ["tools/mk2/start_release_dsp_path.json"]
HEADERS += ["native/mk2/mk2_selection.inc", "native/mk2/mk2_selection_bounds.inc"]
VERIFIERS += ["tools/mk2/generate_selection.py", "tools/mk2/selection_path.json", "tools/mk2/test_selection.py"]
VERIFIERS += ["tools/mk2/selection_dsp_path.json"]
VERIFIERS += ["tools/mk2/generate_start_wait.py", "tools/mk2/test_start_wait.py", "tools/mk2/start_wait_path.json"]
VERIFIERS += ["tools/mk2/generate_next_wait.py", "tools/mk2/test_next_wait.py", "tools/mk2/next_wait_path.json"]
VERIFIERS += ["docs/investigation/decoder-research/upstream/" + name for name in
              ("adsp2100.cpp", "adsp2100.h", "2100ops.hxx", "schedule.cpp", "diexec.cpp", "debugcpu.cpp",
               "debugcmd.cpp", "tms34010.cpp", "tms34010.h", "distate.cpp", "distate.h", "dmadac.h", "dmadac.cpp",
               "sound.cpp", "sound.h", "disound.cpp", "disound.h",
               "screen.cpp", "screen.h", "attotime.cpp", "attotime.h")]
HEADERS += ['native/mk2/mk2_first_fight.inc','native/mk2/mk2_first_fight_main.inc','native/mk2/mk2_first_fight_bounds.inc']
HEADERS += ['native/mk2/mk2_first_fight_high_punch_bounds.inc']
HEADERS += ['native/mk2/mk2_cmos_image.hpp']
HEADERS += ['native/mk2/mk2_valid_cmos_boot.hpp','native/mk2/mk2_operator_boundaries.hpp','native/mk2/mk2_oracle_digest.hpp']
SOURCES += ['native/mk2/mk2_executor.cpp']
# Generated executor from reset with the hardware model, for component tests (closure in mk2_task_batch.cpp).
EXECUTOR_SOURCES = ['native/mk2/mk2_executor.cpp','native/mk2/mk2_task_batch.cpp','native/mk2/mk2_program.cpp',
                    'native/mk2/mk2_dcs.cpp','native/mk2/mk2_adsp.cpp','native/src/mk5_program.cpp']
HEADERS += ['native/mk2/mk2_executor.hpp']
HEADERS += ['native/mk2/mk2_valid_cmos_startup.hpp','native/mk2/mk2_valid_cmos_continuation.hpp',
            'native/mk2/mk2_valid_cmos_startup.inc']
VERIFIERS += ['tools/mk2/generate_valid_cmos_startup.py','tools/mk2/valid_cmos_startup_path.json',
              'tools/mk2/test_valid_cmos_startup.py']
VERIFIERS += ['native/tests/mk2_valid_cmos_boot_tests.cpp','tools/mk2/test_valid_cmos_boot.py']
VERIFIERS += ['native/tests/mk2_host_controls_tests.cpp',
              'tools/mk2/test_host_controls.py','tools/mk2/test_host_runtime.py']
VERIFIERS += ['tools/mk2/generate_first_fight.py','tools/mk2/first_fight_path.json',
              'tools/mk2/first_fight_dsp_path.json',
              'tools/mk2/test_first_fight.py','tools/mk2/test_first_fight_ops.py',
              'tools/mk2/test_first_fight_video.py','tools/mk2/test_first_fight_adsp.py',
              'tools/mk2/test_first_fight_controls.py']



VERIFIERS += ['native/mk2/mk2_audio.hpp','native/tests/mk2_dac_timeline_tests.cpp',
              'native/tests/mk2_dac_boot_tests.cpp','native/tests/mk2_audio_tests.cpp',
              'tools/mk2/dac_reference.py','tools/mk2/dac_reference/emu.h',
              'tools/mk2/dac_reference/logmacro.h','tools/mk2/test_dac_timeline.py',
              'tools/mk2/test_dac_reference.py','tools/mk2/test_audio.py',
              'tools/mk2/test_native_settings.py','tools/mk2/test_batch_cli.py',
              'native/audio/williams_resampler.hpp','native/audio/sinc_resampler.hpp','native/audio/studio_master.hpp','native/include/mk5_audio_output.hpp',
              'native/src/mk5_audio_output.cpp','native/include/mk5_audio_volume.hpp','native/mk2/mk2_native_check.cpp','tools/mk2/native.py']
VERIFIERS += ['tools/mk2/test_continuation.py','tools/mk2/test_native_continuation.py',
              'tools/mk2/continuation_audio_reference.py',
              'native/tests/mk2_continuation_tests.cpp','native/tests/mk2_continuation_audio_tests.cpp']
VERIFIERS += ['tools/mk2/profile.py','tools/mk2/test_profile.py',
              'tools/mk2/test_native_dependencies.py','tools/mk2/test_compact_dsp.py',
              'native/runtime/mk2-factory-default-cmos.bin']
VERIFIERS += ['tools/mk2/valid_boot_dsp_path.json','tools/mk2/test_profile_session.py',
              'tools/mk2/test_first_boot_sport.py','native/tests/mk2_first_boot_sport_tests.cpp',
              'tools/mk2/test_host_resume.py','tools/mk2/test_launcher_lifecycle.py',
              'native/tests/mk2_launcher_lifecycle_tests.cpp','native/tests/mk2_launcher_settings_tests.cpp',
              'tools/mk2/generate_static_cfg.py','tools/mk2/static_cfg_path.json','tools/mk2/test_static_cfg.py']
VERIFIERS += ['native/tests/mk2_valid_cmos_startup_tests.cpp']
VERIFIERS += ['tools/mk2/generate_architectural.py','tools/mk2/architectural_roots.json',
              'tools/mk2/test_architectural_dispatch.py','tools/mk2/test_live_suspend.py',
              'native/tests/mk2_live_suspend_tests.cpp','tools/mk2/package_native.py']
HEADERS += ['native/mk2/mk2_valid_cmos_draw.hpp','native/mk2/mk2_valid_cmos_draw.inc',
            'native/mk2/mk2_valid_cmos_draw_boundary.hpp']
VERIFIERS += ['tools/mk2/generate_valid_cmos_draw.py','tools/mk2/valid_cmos_draw_path.json',
              'tools/mk2/test_valid_cmos_draw.py','tools/mk2/test_valid_cmos_draw_generator.py']
VERIFIERS += ['tools/mk2/test_valid_host_session.py']
VERIFIERS += ['native/tests/mk2_host_close_window.cpp','tools/mk2/test_host_close.py']
SOURCES += ['native/mk2/mk2_profile.cpp']
HEADERS += ['native/mk2/mk2_profile.hpp']
VERIFIERS += ['tools/mk2/operator_reference.py','tools/mk2/generate_operator_boundaries.py','tools/mk2/operator_boundary_digests.json','tools/mk2/oracle_digest.py']
VERIFIERS += ['tools/mk2/generate_valid_host_edges.py','tools/mk2/valid_host_edges_path.json',
              'tools/mk2/test_valid_host_edges.py']
VERIFIERS += ['native/tests/mk2_profile_tests.cpp','tools/mk2/test_profile_cpp.py']


def read_manifest():
    return json.loads(MANIFEST.read_text(encoding="utf-8"))


def verify_roms(directory, manifest=None):
    manifest = read_manifest() if manifest is None else manifest
    rows = []
    for rom in manifest["roms"]:
        name = rom["path"]
        if not name or "/" in name or "\\" in name or ":" in name or name in (".", ".."):
            raise ValueError(f"Invalid ROM path: {name}")
        path = Path(directory) / name
        if not path.is_file():
            raise ValueError(f"ROM missing: {name}")
        if path.stat().st_size != rom["size"]:
            raise ValueError(f"ROM size mismatch: {name}")
        observed = quality.sha(path)
        if observed != rom["sha256"]:
            raise ValueError(f"ROM SHA-256 mismatch: {name}")
        rows.append({"path": name, "sha256": observed})
    if not rows:
        raise ValueError("Empty ROM manifest")
    return {"verified": len(rows), "roms": rows}


def event_summary(events):
    return {"ordered_events": len(events),
            "events_sha256": hashlib.sha256(json.dumps(events, separators=(",", ":")).encode("ascii")).hexdigest(),
            "events_encoding": "ASCII JSON array of [space,PC,address,value,core_cycle] rows, separators comma/colon, no whitespace"}


def run(mode, rom_directory, compiler=None,
        reference_executable=Path("D:/Emuladores/Mame/mame2080/mame.exe"), sanitizer="none", batch=False,
        extended_batch=False, next_batch=False, larger_batch=False, observation_loader=json.loads, coin_queue=False, coin_accounting=False,
        stream_native=False, observation_consumer=None, coin_pair=False, start_wait=False, next_wait=False, prestart_window=None, start_input=False, start_release=False, selection_window=None, first_fight_window=None):
    selected=None
    if prestart_window is not None:
        from tools.mk2.generate_prestart_wait import interval
        selected=interval(prestart_window)
    if selection_window is not None:
        if selected is not None:raise ValueError('Select one measured interval')
        from tools.mk2.generate_selection import interval
        selected=interval(selection_window)
    if first_fight_window is not None:
        if selected is not None:raise ValueError('Select one measured interval')
        from tools.mk2.generate_first_fight import interval
        selected=interval(first_fight_window)
    if (extended_batch or next_batch or larger_batch or coin_queue or coin_accounting or coin_pair or start_wait or next_wait or start_input or start_release or selected is not None) and not batch: raise ValueError("Extended batch requires batch observations")
    if sum((extended_batch,next_batch,larger_batch,coin_queue,coin_accounting,coin_pair,start_wait,next_wait,start_input,start_release))+(selected is not None)>1: raise ValueError("Select one batch endpoint")
    if batch and mode != "reset": raise ValueError("Batch observations require native reset mode")
    if mode == "check" and sanitizer != "none":
        raise ValueError("check only verifies ROMs and does not execute instrumented code")
    compiler = quality.select_compiler(compiler, sanitizer)
    receipt = {"scope": "mk2-foundation", "mode": mode, "sanitizer": sanitizer, "passed": False,
               "limitations": "Finite cold default-DIP L3.1 boot with the qualified legal P1 High Punch timeline, through the first loaded C382 graphics DMA, TRAP/software RETI, display RETI and following task yield at PCff80dd30/cycle1261260. All164 historical CPU checkpoints remain mandatory among177 total, together with all1998125 DSP instruction boundaries and76 visible fields/exact cycles, complete memory/shift-register/DSP images, ordered physical data lanes and four input events. Historical720+1680+480 PCM, DMA reads and IRQ1 events remain compared; final DAC physical slots2880 and enabled1/bufin2160/bufout2015 are separate from PCM packet history. task_queue/task_draw new SPORT vectors must be present and empty. All task_palette observations are frozen at1258434 before advancing. New graphics state includes523 ordered data lanes, complete final VRAM and separate busy/completed full VRAM plus18 DMA registers; command/INTPEND are independently observed at1259256/1260496/1260517. The native41ns-per-pixel deadline is guarded internally; the reference brackets its completion but does not expose the internal callback timestamp. Fixed authenticated TMS/ADSP translations only, without runtime opcode decoding, imported state or shortened waits. Concurrent DSP snapshots retain only the documented cached-cycle exclusion; integral instruction cycles are exact. Other contexts/branches, general interrupt arbitration, hidden ADSP stack contents, fetch-bus equality, final RGB presentation, arbitrary DMA modes/timing, complete gameplay, audible playback, universal synthesis and physical PCB/FPGA equivalence remain outside this finite qualification."}
    try:
        if mode in ("check", "reset", "reference"):
            receipt["roms"] = verify_roms(rom_directory)
        if mode in ("reset", "reference"):
            from tools.mk2.generate_adsp import generate, OUTPUT
            if generate((rom_directory / "su2.l1").read_bytes()) != OUTPUT.read_text(encoding="utf-8"):
                raise ValueError("ADSP generated source is stale")
            from tools.mk2.generate_video_irq import generate as generate_video, load_program, OUTPUT as VIDEO_OUTPUT
            if generate_video(load_program(rom_directory)) != VIDEO_OUTPUT.read_text(encoding="utf-8"):
                raise ValueError("TMS video IRQ generated source is stale")
            from tools.mk2.generate_dispatcher import generate as generate_dispatcher, OUTPUT as DISPATCHER_OUTPUT
            if generate_dispatcher(load_program(rom_directory)) != DISPATCHER_OUTPUT.read_text(encoding="utf-8"):
                raise ValueError("TMS dispatcher generated source is stale")
            from tools.mk2.generate_task_resume import generate as generate_task_resume, OUTPUT as TASK_RESUME_OUTPUT
            if generate_task_resume(load_program(rom_directory)) != TASK_RESUME_OUTPUT.read_text(encoding="utf-8"):
                raise ValueError("TMS task resumption generated source is stale")
            from tools.mk2.generate_task_queue import generate as generate_task_queue, OUTPUT as TASK_QUEUE_OUTPUT
            if generate_task_queue(load_program(rom_directory)) != TASK_QUEUE_OUTPUT.read_text(encoding="utf-8"):
                raise ValueError("TMS task queue generated source is stale")
            from tools.mk2.generate_task_palette import generate as generate_task_palette, OUTPUT as TASK_PALETTE_OUTPUT
            if generate_task_palette(load_program(rom_directory)) != TASK_PALETTE_OUTPUT.read_text(encoding="utf-8"):
                raise ValueError("TMS task palette generated source is stale")
            from tools.mk2.generate_task_draw import generate as generate_task_draw, OUTPUT as TASK_DRAW_OUTPUT
            if generate_task_draw(load_program(rom_directory)) != TASK_DRAW_OUTPUT.read_text(encoding="utf-8"):
                raise ValueError("TMS task draw generated source is stale")
            from tools.mk2.generate_task_batch import generate as generate_batch, OUTPUT as BATCH_OUTPUT
            if generate_batch(load_program(rom_directory)) != BATCH_OUTPUT.read_text(encoding="utf-8"):
                raise ValueError("TMS task batch generated source is stale")
            from tools.mk2.generate_coin_queue import generate as generate_coin, OUTPUT as COIN_OUTPUT
            if generate_coin(load_program(rom_directory)) != COIN_OUTPUT.read_text(encoding="utf-8"):
                raise ValueError("TMS coin queue generated source is stale")
            from tools.mk2.generate_coin_accounting import generate as generate_accounting, OUTPUT as ACCOUNTING_OUTPUT
            if generate_accounting(load_program(rom_directory)) != ACCOUNTING_OUTPUT.read_text(encoding="utf-8"):
                raise ValueError("TMS coin accounting generated source is stale")
            if coin_pair or start_wait or next_wait or start_input or start_release or selected is not None:
                from tools.mk2.generate_coin_pair import generate as generate_pair, OUTPUT as PAIR_OUTPUT
                if generate_pair(load_program(rom_directory)) != PAIR_OUTPUT.read_text(encoding="utf-8"):
                    raise ValueError("TMS coin pair generated source is stale")
            if start_wait or next_wait or start_input or start_release or selected is not None:
                from tools.mk2.generate_start_wait import generate as generate_wait, OUTPUT as WAIT_OUTPUT
                if generate_wait(load_program(rom_directory)) != WAIT_OUTPUT.read_text(encoding="utf-8"):
                    raise ValueError("TMS Start wait generated source is stale")
            if next_wait or start_input or start_release or selected is not None:
                from tools.mk2.generate_next_wait import generate as generate_next_wait, OUTPUT as NEXT_WAIT_OUTPUT
                if generate_next_wait(load_program(rom_directory)) != NEXT_WAIT_OUTPUT.read_text(encoding="utf-8"):
                    raise ValueError("TMS next wait generated source is stale")
            if start_input or start_release or selected is not None:
                from tools.mk2 import generate_prestart_wait as prestart
                if (prestart.generate(load_program(rom_directory))!=prestart.OUTPUT.read_text(encoding='utf-8') or
                    prestart.generate_bounds()!=prestart.BOUNDS_OUTPUT.read_text(encoding='utf-8')):
                    raise ValueError('TMS pre-Start generated source or bounds are stale')
            if start_input or start_release or (selection_window is not None or first_fight_window is not None):
                from tools.mk2 import generate_start_input as start
                if start.generate(load_program(rom_directory))!=start.OUTPUT.read_text(encoding='utf-8'):
                    raise ValueError('TMS Start input fixed source is stale')
            if start_release or (selection_window is not None or first_fight_window is not None):
                from tools.mk2 import generate_start_release as release
                if release.generate(load_program(rom_directory))!=release.OUTPUT.read_text(encoding='utf-8'):
                    raise ValueError('TMS Start release fixed source is stale')
            if (selection_window is not None or first_fight_window is not None):
                from tools.mk2 import generate_selection as selection
                if (selection.generate(load_program(rom_directory))!=selection.OUTPUT.read_text(encoding='utf-8') or
                    selection.generate_bounds()!=selection.BOUNDS_OUTPUT.read_text(encoding='utf-8')):
                    raise ValueError('TMS selection source or boundaries are stale')
            if first_fight_window is not None:
                from tools.mk2 import generate_first_fight as fight
                if (fight.generate(load_program(rom_directory))!=fight.OUTPUT.read_text(encoding='utf-8') or
                    fight.generate_bounds()!=fight.BOUNDS_OUTPUT.read_text(encoding='utf-8') or
                    fight.generate_high_punch_bounds()!=fight.HIGH_PUNCH_BOUNDS_OUTPUT.read_text(encoding='utf-8')):
                    raise ValueError('TMS first-fight source or boundaries are stale')
        if mode != "check":
            with quality.scratch_tree() as (scratch, cleanup):
                receipt["cleanup"] = cleanup
                executable = scratch / "mk2-foundation.exe"
                flags = [str(compiler), "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror",
                         "-I" + str(ROOT / "native/include")]
                env = {"PATH": str(compiler.parent) + os.pathsep + os.environ.get("PATH", "")}
                if sanitizer != "none":
                    flags += ["-O1", "-g", "-fno-omit-frame-pointer", "-fsanitize=" + sanitizer,
                              "-fno-sanitize-recover=all"]
                    if sanitizer == "undefined":
                        flags += ["-fsanitize-undefined-trap-on-error", "-fno-sanitize=vptr"]
                    elif sanitizer == "address":
                        env["ASAN_OPTIONS"] = ("halt_on_error=1:abort_on_error=0:exitcode=86:"
                                               "detect_leaks=0:disable_coredump=1:symbolize=1")
                        env["ASAN_SYMBOLIZER_PATH"] = str(compiler.parent / "llvm-symbolizer.exe")
                build = quality.command(flags + [str(ROOT / p) for p in SOURCES] +
                                        ["-lbcrypt", "-o", str(executable)], scratch, timeout=240, env=env)
                receipt["build"] = quality.compact(build)
                if not build["passed"]:
                    raise RuntimeError("MK2 foundation build failed")
                command = [executable] + ([str(rom_directory)] if mode in ("reset", "reference") else [])
                if batch: command.append("--first-fight-window="+str(first_fight_window) if first_fight_window is not None else "--selection-window="+str(selection_window) if selection_window is not None else "--start-release" if start_release else "--start-input" if start_input else "--prestart-window="+str(prestart_window) if selected is not None else "--next-wait" if next_wait else "--start-wait" if start_wait else "--coin-pair" if coin_pair else "--coin-accounting" if coin_accounting else "--coin-queue" if coin_queue else "--batch-larger" if larger_batch else "--batch-next" if next_batch else "--batch-extended" if extended_batch else "--batch")
                decoder = None
                if stream_native:
                    from tools.mk2 import native_json, stream_transport
                    decoder = native_json.NativeDecoder()
                    backend = Path(native_json.orjson.__file__)
                    receipt["native_json_backend"] = {
                        "version": native_json.orjson.__version__,
                        "files": {str(p): quality.sha(p) for p in
                                  (backend, *backend.parent.glob("*.pyd"))}}
                    receipt["native_json_limits"] = {"block_bytes": decoder.block_byte_limit,
                                                      "pending_value_bytes": decoder.pending_value_byte_limit}
                    result = stream_transport.command(command, scratch, lambda _: None, lambda _: None,
                                                      timeout=120, decoder=decoder, env=env)
                else:
                    result = quality.command(command, scratch, env=env, timeout=120,
                                             output_limit=(1024 if next_batch or larger_batch or coin_queue or coin_accounting else 768 if batch else 512)*1024*1024)
                if not result["passed"]:
                    receipt["execution"] = quality.compact(result)
                    raise RuntimeError("MK2 foundation contracts failed")
                compact = quality.compact(result)
                receipt["execution"] = compact
                if mode in ("reset", "reference"):
                    if result.get("discarded_output_bytes", 0):
                        raise ValueError("Incomplete native observations: output budget exceeded")
                    native = decoder.result if decoder is not None else observation_loader(result["stdout"])
                    receipt["native_output_bytes"] = decoder.bytes if decoder is not None else len(result["stdout"])
                    if observation_consumer is not None:
                        observation_consumer(native)
                    # Keep opcode observations only in memory, never in the receipt's stdout tail.
                    compact["output_tail"] = "Structured CPU, write and ADSP boot observations compared in memory."
                    if coin_pair or start_wait or next_wait or start_input or start_release or selected is not None:
                        from tools.mk2.batch_reference import ARRAY_KEYS
                        keys=set(ARRAY_KEYS)-{'batch_dma_reads'}
                        if start_release or (selection_window is not None or first_fight_window is not None): keys.add('batch_protection')
                        if set(native)!=keys|{'scope'} or native['scope']!=('mk2-first-fight-suffix' if first_fight_window is not None else 'mk2-selection-suffix' if selection_window is not None else 'mk2-start-release-suffix' if start_release else 'mk2-start-input-suffix' if start_input else 'mk2-prestart-wait-suffix' if selected is not None else 'mk2-next-wait-suffix' if next_wait else 'mk2-start-wait-suffix' if start_wait else 'mk2-coin-pair-suffix'):
                            raise ValueError('Incomplete or unexpected native coin pair observation schema')
                        if (len(native['batch_cpu'])!=(389582 if start_release else 439431 if start_input else selected['cpu_rows'] if selected is not None else 392474 if next_wait else 425616 if start_wait else 485629) or
                            __import__('tools.mk2.oracle_digest',fromlist=['words_digest']).words_digest(native['batch_final_cpu'][:3])!=(release.measured['end_cpu34_head_sha256'] if start_release else start.measured['end_cpu34_head_sha256'] if start_input else selected['end_cpu34_head_sha256'] if selected is not None else 'efa405f42aee1937ea37564bd1c654f0863d253ee9a9387adbb1c9791bfe37c7' if next_wait else '2a7a1c40e10ddd3ed007629bb2c887dfc50e3780737a7d60f24383c7c79a18fe' if start_wait else 'aeb87e805d2167e2377bb89c24469b334e2e87b0033b50202840b6a8e97c6a7e')):
                            raise ValueError('Incomplete native coin pair horizon')
                        receipt['native_counts']={key:len(native[key]) for key in sorted(keys)}
                        receipt['historical_observations_included']=False
                    else:
                        receipt["native"] = {"checkpoints": native["checkpoints"],
                                             "writes": native["writes"], "boot_word_count": len(native["boot_words"]),
                                             "dsp_instructions": len(native["dsp_trace"]), "dsp_final": native["dsp_points"][-1][:16],
                                             "dsp_accesses_summary": event_summary(native["dsp_accesses"]), "ram": native["ram"],
                                             "cmos_reads": native["cmos_reads"], "cmos_writes": native["cmos_writes"],
                                             "video_write_count": len(native["video_writes"]),
                                             "palette_words": len(native["palette_ram"]),
                                             "local_vram_words": len(native["vram"]),
                                             "cpu_vram_bytes_written": 65536 * 4,
                                             "video_display": native["video_display"],
                                             "text_instructions": native["text_instructions"],
                                             "text_bus_accesses": len(native["text_bus"]),
                                             "text_word_reads": len(native["text_reads"]),
                                             "text_word_writes": len(native["text_writes"]),
                                             "text_pixblt_entries": len(native["text_pixblt"]),
                                             "text_ink_pixels": sum(value != 0 for value in native["text_vram"]),
                                             "text_display": native["text_display"],
                                             "factory_instructions": native["factory_instructions"],
                                             "factory_bus_accesses": len(native["factory_bus"]),
                                             "factory_cmos_nonzero_words": sum(value != 0 for value in native["factory_cmos"]),
                                             "factory_main_ram_words": len(native["factory_main_ram"]),
                                             "factory_display": native["factory_display"],
                                             "post_factory_instructions": native["post_factory_instructions"],
                                             "post_factory_bus_accesses": len(native["post_factory_bus"]),
                                             "post_factory_pixblt_entries": len(native["post_factory_pixblt"]),
                                             "post_factory_cmos_nonzero_words": sum(value != 0 for value in native["post_factory_cmos"]),
                                             "post_factory_ink_pixels": sum(value != 0 for value in native["post_factory_vram"]),
                                             "post_factory_display": native["post_factory_display"],
                                             "sound_command_instructions": native["sound_command_instructions"],
                                             "sound_command_bus_accesses": len(native["sound_command_bus"]),
                                             "sound_command_main_ram_words": len(native["sound_command_main_ram"]),
                                             "sound_command_cmos_nonzero_words": sum(value != 0 for value in native["sound_command_cmos"]),
                                             "sound_command_display": native["sound_command_display"],
                                             "input_wait_instructions": native["input_wait_instructions"],
                                             "input_wait_bus_accesses": len(native["input_wait_bus"]),
                                             "input_wait_pixblt_entries": len(native["input_wait_pixblt"]),
                                             "input_wait_main_ram_words": len(native["input_wait_main_ram"]),
                                             "input_wait_cmos_nonzero_words": sum(value != 0 for value in native["input_wait_cmos"]),
                                             "input_wait_ink_pixels": sum(value != 0 for value in native["input_wait_vram"]),
                                             "input_wait_display": native["input_wait_display"],
                                             "input_poll_instructions": native["input_poll_instructions"],
                                             "input_poll_bus_accesses": len(native["input_poll_bus"]),
                                             "input_poll_pixblt_entries": len(native["input_poll_pixblt"]),
                                             "input_poll_main_ram_words": len(native["input_poll_main_ram"]),
                                             "input_poll_cmos_nonzero_words": sum(value != 0 for value in native["input_poll_cmos"]),
                                             "input_poll_ink_pixels": sum(value != 0 for value in native["input_poll_vram"]),
                                             "input_poll_display": native["input_poll_display"],
                                             "input_idle_instructions": native["input_idle_instructions"],
                                             "input_idle_bus_accesses": len(native["input_idle_bus"]),
                                             "input_idle_main_ram_words": len(native["input_idle_main_ram"]),
                                             "input_idle_cmos_nonzero_words": sum(value != 0 for value in native["input_idle_cmos"]),
                                             "input_idle_ink_pixels": sum(value != 0 for value in native["input_idle_vram"]),
                                             "input_idle_display": native["input_idle_display"],
                                             "sport_wrap_instructions": native["sport_wrap_instructions"],
                                             "sport_wrap_bus_accesses": len(native["sport_wrap_bus"]),
                                             "sport_wrap_main_ram_words": len(native["sport_wrap_main_ram"]),
                                             "sport_wrap_cmos_nonzero_words": sum(value != 0 for value in native["sport_wrap_cmos"]),
                                             "sport_wrap_ink_pixels": sum(value != 0 for value in native["sport_wrap_vram"]),
                                             "sport_wrap_display": native["sport_wrap_display"],
                                             "button_exit_instructions": native["button_exit_instructions"],
                                             "button_exit_bus_accesses": len(native["button_exit_bus"]),
                                             "button_exit_main_ram_words": len(native["button_exit_main_ram"]),
                                             "button_exit_cmos_nonzero_words": sum(value != 0 for value in native["button_exit_cmos"]),
                                             "button_exit_ink_pixels": sum(value != 0 for value in native["button_exit_vram"]),
                                             "button_exit_display": native["button_exit_display"],
                                             "button_exit_dma_registers": native["button_exit_dma_registers"],
                                             "input_events": native["input_events"],
                                             "sport_dac_buffer_samples": len(native["sport_dac_buffer"]),
                                             "sport_wrap_dac_buffer_samples": len(native["sport_wrap_dac_buffer"]),
                                             "video_irq_instructions": native["video_irq_instructions"],
                                             "video_irq_bus_accesses": len(native["video_irq_bus"]),
                                             "video_irq_main_ram_words": len(native["video_irq_main_ram"]),
                                             "video_irq_cmos_nonzero_words": sum(value != 0 for value in native["video_irq_cmos"]),
                                             "video_irq_ink_pixels": sum(value != 0 for value in native["video_irq_vram"]),
                                             "video_irq_display": native["video_irq_display"],
                                             "video_irq_dma_registers": native["video_irq_dma_registers"],
                                             "startup_instructions": native["startup_instructions"],
                                             "startup_bus_accesses": len(native["startup_bus"]),
                                             "startup_main_ram_words": len(native["startup_main_ram"]),
                                             "startup_cmos_nonzero_words": sum(value != 0 for value in native["startup_cmos"]),
                                             "startup_ink_pixels": sum(value != 0 for value in native["startup_vram"]),
                                             "startup_display": native["startup_display"],
                                             "startup_dma_registers": native["startup_dma_registers"],
                                             "dispatcher_instructions": native["dispatcher_instructions"],
                                             "dispatcher_bus_accesses": len(native["dispatcher_bus"]),
                                             "dispatcher_main_ram_words": len(native["dispatcher_main_ram"]),
                                             "dispatcher_cmos_nonzero_words": sum(value != 0 for value in native["dispatcher_cmos"]),
                                             "dispatcher_ink_pixels": sum(value != 0 for value in native["dispatcher_vram"]),
                                             "dispatcher_display": native["dispatcher_display"],
                                             "dispatcher_dma_registers": native["dispatcher_dma_registers"],
                                             "dispatcher_shiftreg_words": len(native["dispatcher_shiftreg"]),
                                             "dispatcher_shiftreg_json_sha256": hashlib.sha256(json.dumps(native["dispatcher_shiftreg"], separators=(",", ":")).encode("ascii")).hexdigest(),
                                             "dispatcher_event_counts": native["dispatcher_event_counts"],
                                             "dispatcher_dcs_control": native["dispatcher_dcs_control"],
                                             "dispatcher_irq1_state": native["dispatcher_irq1_state"],
                                             "dispatcher_dac_buffer_samples": len(native["dispatcher_dac_buffer"]),
                                             "dispatcher_sport_deadline_ticks_native": native["dispatcher_sport_deadline"],
                                             "task_resume_instructions": native["task_resume_instructions"],
                                             "task_queue_instructions": native["task_queue_instructions"],
                                             "task_draw_instructions": native["task_draw_instructions"],
                                             "task_draw_bus_accesses": len(native["task_draw_bus"]),
                                             "task_draw_bus_json_sha256": hashlib.sha256(json.dumps(native["task_draw_bus"], separators=(",", ":")).encode("ascii")).hexdigest(),
                                             "task_draw_main_ram_words": len(native["task_draw_main_ram"]),
                                             "task_draw_cmos_words": len(native["task_draw_cmos"]),
                                             "task_draw_palette_words": len(native["task_draw_palette_ram"]),
                                             "task_draw_vram_words": len(native["task_draw_vram"]),
                                             "task_draw_shiftreg_words": len(native["task_draw_shiftreg"]),
                                             "task_draw_event_counts": native["task_draw_event_counts"],
                                             "task_draw_dcs_control": native["task_draw_dcs_control"],
                                             "task_draw_display": native["task_draw_display"],
                                             "task_draw_dma_registers": native["task_draw_dma_registers"],
                                             "task_draw_graphics_dma": native["task_draw_graphics_dma"],
                                             "task_draw_dma_deadline_subticks_native": native["task_draw_dma_deadline_subticks"],
                                             "task_draw_dma_busy_vram_words": len(native["task_draw_dma_busy_vram"]),
                                             "task_draw_dma_busy_vram_json_sha256": hashlib.sha256(json.dumps(native["task_draw_dma_busy_vram"], separators=(",", ":")).encode("ascii")).hexdigest(),
                                             "task_draw_dma_busy_registers": native["task_draw_dma_busy_registers"],
                                             "task_draw_dma_completed_vram_words": len(native["task_draw_dma_completed_vram"]),
                                             "task_draw_dma_completed_vram_json_sha256": hashlib.sha256(json.dumps(native["task_draw_dma_completed_vram"], separators=(",", ":")).encode("ascii")).hexdigest(),
                                             "task_draw_dma_completed_registers": native["task_draw_dma_completed_registers"],
                                             "task_draw_irq1_state": native["task_draw_irq1_state"],
                                             "task_draw_transfers": native["task_draw_transfers"],
                                             "task_draw_dma_positions": native["task_draw_dma_positions"],
                                             "task_draw_irq1_events": native["task_draw_irq1_events"],
                                             "task_draw_sport_deadline_ticks_native": native["task_draw_sport_deadline"],
                                             "task_draw_pcm_samples": len(native["task_draw_pcm"]),
                                             "task_draw_pcm_json_sha256": hashlib.sha256(json.dumps(native["task_draw_pcm"], separators=(",", ":")).encode("ascii")).hexdigest(),
                                             "task_draw_dac_buffer_samples": len(native["task_draw_dac_buffer"]),
                                             "task_draw_dac_json_sha256": hashlib.sha256(json.dumps(native["task_draw_dac_buffer"], separators=(",", ":")).encode("ascii")).hexdigest(),
                                             "task_draw_dsp_device": native["task_draw_dsp_device"],
                                             "task_palette_instructions": native["task_palette_instructions"],
                                             "task_palette_bus_accesses": len(native["task_palette_bus"]),
                                             "task_palette_bus_json_sha256": hashlib.sha256(json.dumps(native["task_palette_bus"], separators=(",", ":")).encode("ascii")).hexdigest(),
                                             "task_palette_main_ram_words": len(native["task_palette_main_ram"]),
                                             "task_palette_cmos_words": len(native["task_palette_cmos"]),
                                             "task_palette_palette_words": len(native["task_palette_palette_ram"]),
                                             "task_palette_vram_words": len(native["task_palette_vram"]),
                                             "task_palette_shiftreg_words": len(native["task_palette_shiftreg"]),
                                             "task_palette_event_counts": native["task_palette_event_counts"],
                                             "task_palette_dcs_control": native["task_palette_dcs_control"],
                                             "task_palette_display": native["task_palette_display"],
                                             "task_palette_dma_registers": native["task_palette_dma_registers"],
                                             "task_palette_irq1_state": native["task_palette_irq1_state"],
                                             "task_palette_transfers": native["task_palette_transfers"],
                                             "task_palette_dma_positions": native["task_palette_dma_positions"],
                                             "task_palette_irq1_events": native["task_palette_irq1_events"],
                                             "task_palette_sport_deadline_ticks_native": native["task_palette_sport_deadline"],
                                             "task_palette_pcm_samples": len(native["task_palette_pcm"]),
                                             "task_palette_pcm_json_sha256": hashlib.sha256(json.dumps(native["task_palette_pcm"], separators=(",", ":")).encode("ascii")).hexdigest(),
                                             "task_palette_dac_buffer_samples": len(native["task_palette_dac_buffer"]),
                                             "task_palette_dac_json_sha256": hashlib.sha256(json.dumps(native["task_palette_dac_buffer"], separators=(",", ":")).encode("ascii")).hexdigest(),
                                             "task_palette_dsp_device": native["task_palette_dsp_device"],
                                             "task_queue_bus_accesses": len(native["task_queue_bus"]),
                                             "task_queue_main_ram_words": len(native["task_queue_main_ram"]),
                                             "task_queue_cmos_words": len(native["task_queue_cmos"]),
                                             "task_queue_palette_words": len(native["task_queue_palette_ram"]),
                                             "task_queue_vram_words": len(native["task_queue_vram"]),
                                             "task_queue_shiftreg_words": len(native["task_queue_shiftreg"]),
                                             "task_queue_bus_json_sha256": hashlib.sha256(json.dumps(native["task_queue_bus"], separators=(",", ":")).encode("ascii")).hexdigest(),
                                             "task_queue_event_counts": native["task_queue_event_counts"],
                                             "task_queue_dcs_control": native["task_queue_dcs_control"],
                                             "task_queue_display": native["task_queue_display"],
                                             "task_queue_dma_registers": native["task_queue_dma_registers"],
                                             "task_queue_irq1_state": native["task_queue_irq1_state"],
                                             "task_queue_transfers": native["task_queue_transfers"],
                                             "task_queue_dma_positions": native["task_queue_dma_positions"],
                                             "task_queue_irq1_events": native["task_queue_irq1_events"],
                                             "task_queue_sport_deadline_ticks_native": native["task_queue_sport_deadline"],
                                             "task_queue_pcm_samples": len(native["task_queue_pcm"]),
                                             "task_queue_pcm_json_sha256": hashlib.sha256(json.dumps(native["task_queue_pcm"], separators=(",", ":")).encode("ascii")).hexdigest(),
                                             "task_queue_dac_buffer_samples": len(native["task_queue_dac_buffer"]),
                                             "task_queue_dac_json_sha256": hashlib.sha256(json.dumps(native["task_queue_dac_buffer"], separators=(",", ":")).encode("ascii")).hexdigest(),
                                             "task_queue_dsp_device": native["task_queue_dsp_device"],
                                             "task_resume_bus_accesses": len(native["task_resume_bus"]),
                                             "task_resume_main_ram_words": len(native["task_resume_main_ram"]),
                                             "task_resume_cmos_words": len(native["task_resume_cmos"]),
                                             "task_resume_palette_words": len(native["task_resume_palette_ram"]),
                                             "task_resume_vram_words": len(native["task_resume_vram"]),
                                             "task_resume_cmos_nonzero_words": sum(value != 0 for value in native["task_resume_cmos"]),
                                             "task_resume_ink_pixels": sum(value != 0 for value in native["task_resume_vram"]),
                                             "task_resume_display": native["task_resume_display"],
                                             "task_resume_dma_registers": native["task_resume_dma_registers"],
                                             "task_resume_shiftreg_words": len(native["task_resume_shiftreg"]),
                                             "task_resume_shiftreg_json_sha256": hashlib.sha256(json.dumps(native["task_resume_shiftreg"], separators=(",", ":")).encode("ascii")).hexdigest(),
                                             "task_resume_event_counts": native["task_resume_event_counts"],
                                             "task_resume_dcs_control": native["task_resume_dcs_control"],
                                             "task_resume_irq1_state": native["task_resume_irq1_state"],
                                             "task_resume_sport_deadline_ticks_native": native["task_resume_sport_deadline"],
                                             "task_resume_transfers": native["task_resume_transfers"],
                                             "task_resume_dma_positions": native["task_resume_dma_positions"],
                                             "task_resume_irq1_events": native["task_resume_irq1_events"],
                                             "task_resume_dac_buffer_samples": len(native["task_resume_dac_buffer"]),
                                             "task_resume_dac_buffer_json_sha256": hashlib.sha256(json.dumps(native["task_resume_dac_buffer"], separators=(",", ":")).encode("ascii")).hexdigest(),
                                             "task_resume_pcm_samples": len(native["task_resume_pcm"]),
                                             "task_resume_pcm_nonzero_samples": sum(value != 0 for value in native["task_resume_pcm"]),
                                             "task_resume_pcm_json_sha256": hashlib.sha256(json.dumps(native["task_resume_pcm"], separators=(",", ":")).encode("ascii")).hexdigest(),
                                             "startup_event_counts": native["startup_event_counts"],
                                             "startup_dcs_control": native["startup_dcs_control"],
                                             "startup_irq1_state": native["startup_irq1_state"],
                                             "startup_dac_buffer_samples": len(native["startup_dac_buffer"]),
                                             "startup_sport_deadline_ticks_native": native["startup_sport_deadline"],
                                             "video_irq_event_counts": native["video_irq_event_counts"],
                                             "video_irq_dcs_control": native["video_irq_dcs_control"],
                                             "video_irq_irq1_state": native["video_irq_irq1_state"],
                                             "video_irq_dac_buffer_samples": len(native["video_irq_dac_buffer"]),
                                             "video_irq_sport_deadline_ticks_native": native["video_irq_sport_deadline"],
                                             "button_exit_event_counts": native["button_exit_event_counts"],
                                             "button_exit_dcs_control": native["button_exit_dcs_control"],
                                             "button_exit_irq1_state": native["button_exit_irq1_state"],
                                             "button_exit_dac_buffer_samples": len(native["button_exit_dac_buffer"]),
                                             "button_exit_sport_deadline_ticks_native": native["button_exit_sport_deadline_ticks"],
                                             "dsp_reboots": native["dsp_reboots"],
                                             "dsp_serial_events": native["dsp_serial_events"],
                                             "input_idle_dsp_device": native["input_idle_dsp_device"],
                                             "input_poll_dsp_device": native["input_poll_dsp_device"],
                                             "sport_transfers": native["sport_transfers"],
                                             "sport_dma_position": native["sport_dma_position"],
                                             "sport_dma_positions": native["sport_dma_positions"],
                                             "sport_irq1_state": native["sport_irq1_state"],
                                             "sport_irq1_events": native["sport_irq1_events"],
                                             "sport_deadline_ticks": native["sport_deadline_ticks"],
                                             "sport_pcm_samples": len(native["sport_pcm"]),
                                             "sport_pcm_nonzero_samples": sum(value != 0 for value in native["sport_pcm"]),
                                             "sport_pcm_json_sha256": hashlib.sha256(json.dumps(native["sport_pcm"], separators=(",", ":")).encode("ascii")).hexdigest(),
                                             "input_wait_dsp_device": native["input_wait_dsp_device"],
                                             "dsp_device": native["dsp_device"],
                                             "dsp_external_pm_words": len(native["dsp_external_pm"]),
                                             "dsp_external_dm_words": len(native["dsp_external_dm"])}
                        if mode == "reference":
                            from tools.mk2 import reference
                            receipt["dispatcher_native_read_transport"] = reference.validate_dispatcher_native_data(
                                native, rom_directory, read_manifest())
                            receipt["task_resume_native_read_transport"] = reference.validate_dispatcher_native_data(
                                native, rom_directory, read_manifest(), phase="task_resume")
                            receipt["task_queue_native_read_transport"] = reference.validate_dispatcher_native_data(
                                native, rom_directory, read_manifest(), phase="task_queue")
                            receipt["task_palette_native_read_transport"] = reference.validate_dispatcher_native_data(
                                native, rom_directory, read_manifest(), phase="task_palette")
                            receipt["task_draw_native_read_transport"] = reference.validate_dispatcher_native_data(
                                native, rom_directory, read_manifest(), phase="task_draw")
                            observed, metadata = reference.capture(scratch, reference_executable, rom_directory, read_manifest())
                            receipt["reference"] = metadata
                            receipt["comparison"] = reference.compare(native, observed)
                            if not receipt["comparison"]["passed"]:
                                raise RuntimeError("MK2 first divergence: " + receipt["comparison"]["first_difference"])
                            receipt["dsp_comparison"] = reference.compare_dsp(native, observed)
                            if not receipt["dsp_comparison"]["passed"]:
                                raise RuntimeError("MK2 first DSP divergence: " + receipt["dsp_comparison"]["first_difference"])
                            verify_roms(rom_directory)
                receipt["execution"] = compact
        receipt["passed"] = True
    except Exception as error:
        receipt["error"] = str(error)
    finally:
        if "native_json_backend" in receipt:
            backend = receipt["native_json_backend"]
            try:
                backend["exit_files"] = {path: quality.sha(Path(path)) for path in backend["files"]}
                if backend["exit_files"] != backend["files"]:
                    receipt.update(passed=False, error="Native JSON backend changed during execution")
            except Exception as error:
                receipt.update(passed=False, error="Native JSON backend verification failed: " + str(error))
        receipt["inputs"] = {p: quality.sha(__import__("tools.setup.mk2", fromlist=["source"]).source(p)) for p in SOURCES + HEADERS + VERIFIERS}
        receipt["manifest_sha256"] = quality.sha(MANIFEST)
        stage = ROOT / "build/native/mk2"
        stage.mkdir(parents=True, exist_ok=True)
        suffix = "" if sanitizer == "none" else "-" + sanitizer
        (stage / f"{mode}{'-first-fight-'+str(first_fight_window) if first_fight_window is not None else '-selection-'+str(selection_window) if selection_window is not None else '-start-release' if start_release else '-start-input' if start_input else '-prestart-'+str(prestart_window) if selected is not None else '-next-wait' if next_wait else '-start-wait' if start_wait else '-coin-pair' if coin_pair else ''}{suffix}.json").write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
    return receipt


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("check", "test", "reset", "reference"))
    parser.add_argument("--rom-directory", type=Path, default=ROOT / "roms/mk2")
    parser.add_argument("--reference-executable", type=Path, default=Path("D:/Emuladores/Mame/mame2080/mame.exe"))
    parser.add_argument("--sanitizer", choices=("none", "undefined", "address"), default="none")
    args = parser.parse_args()
    if args.mode == "check" and args.sanitizer != "none":
        parser.error("check only verifies ROMs and does not execute instrumented code")
    receipt = run(args.mode, args.rom_directory.resolve(), reference_executable=args.reference_executable.resolve(),
                  sanitizer=args.sanitizer)
    print(json.dumps(receipt, indent=2))
    return 0 if receipt["passed"] else 1


HEADERS += ['native/mk2/mk2_live_host.hpp','native/mk2/mk2_live_limits.hpp']
HEADERS += ['native/mk2/mk2_replay.hpp']
HEADERS += ['native/mk2/mk2_save_state.hpp','native/mk2/mk2_state_io.hpp']
VERIFIERS += ['native/tests/mk2_save_state_tests.cpp','native/tests/mk2_test_audio_sink.cpp','tools/mk2/test_save_state.py']

if __name__ == "__main__":
    raise SystemExit(main())

VERIFIERS += ['tools/mk2/generate_architectural.py','tools/mk2/test_architectural_dispatch.py','tools/mk2/architectural_roots.json']
