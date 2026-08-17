#-*- coding: utf-8 -*-
#
# openplat4d_helper.py  (OpenSplat4D enhanced edition)
#
# Orchestration bridge between the OpenSplat4D Unreal Engine plugin and the
# external COLMAP / gaussian-splatting toolchain. Ported from the reference
# plugin's `gaussian_splatting_helper.py`
# (teachers/GaussianSplattingForUnrealEngine/Scripts) and EXTENDED with a 4DGS
# training branch (`--4d`) that targets the 4d-gaussian-splatting repo, plus
# robustness/observability improvements (see ENHANCEMENT MAP below).
#
# The UE C++ side (UOpenSplat4DStep_*) shells out to this script:
#   Sparse reconstruction : <py> openplat4d_helper.py <workDir> --colmap <colmap> --sparse ...
#   Colmap view / edit    : <py> openplat4d_helper.py <workDir> --colmap <colmap> --view|--edit
#   Train (3DGS)          : <py> openplat4d_helper.py <workDir> --colmap <colmap> --gaussian <repo> --train="..."
#   Train (4DGS)          : ... --gaussian <4d_repo> --train="..." --4d
#   Clip                  : <py> openplat4d_helper.py <workDir> --clip --ply <ply> --mask_dilation N --clip_threshold F
#   Batch HLOD clip       : <py> openplat4d_helper.py <workDir> --hlod_clip [--batch_mask_subdir masks]
#
# ---------------------------------------------------------------------------
# ENHANCEMENT MAP vs. the reference helper
# ---------------------------------------------------------------------------
#  [E1] The reference helper declared `if self.args.clip: self.executeGaussianSplattingClip()`
#       but NEVER DEFINED that method -> `AttributeError` on --clip. OpenSplat4D
#       implements executeGaussianSplattingClip() fully.
#  [E2] Input validation: workDir must exist; --colmap must point to a real
#       executable when sparse/view/edit is requested; --gaussian must exist
#       when training is requested; masks dir must exist for --clip. Fail fast
#       with actionable messages instead of obscure colmap tracebacks.
#  [E3] runCommand() now PROPAGATES non-zero exit codes (sys.exit) so the UE
#       orchestrator sees a real failure instead of a silent success. The
#       reference only printed the code and kept going.
#  [E4] Python resolution is conda-FREE and distribution-safe: the training /
#       clip subprocesses are launched with the plugin's OWN bundled Python
#       (<Plugin>/ThirdParty/Python/python.exe) by default, with override via
#       --python <path> or the OPENSPLAT4D_PYTHON env var. We never assume the
#       end user has conda or a compatible system Python (the CUDA extensions
#       _C.pyd are ABI-locked to the bundled interpreter).
#  [E5] Timestamped, tagged logging so multiple pipeline steps are easy to
#       follow in the UE output log.
#  [E6] `--hlod_clip` convenience: recursively clip every produced point cloud
#       under the work dir using the enhanced clip_model.py batch workflow.
#  [E7] The 4D training branch forwards `--train` overrides verbatim (never
#       reduced); the python interpreter is resolved the same conda-free way.
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# ENCODING FIX: Python on Chinese Windows decodes sys.argv with GBK, garbling
# Chinese paths. We fix this with two complementary mechanisms:
# 1. CommandLineToArgvW: replaces sys.argv with correctly-decoded UTF-16
#    arguments from the raw Windows command line.
# 2. If C++ has base64-encoded workDir (b64:...), decode it. Tries UTF-8
#    first (new DLL), falls back to UTF-16-LE (old DLL).
# ---------------------------------------------------------------------------
import sys as _sf
if hasattr(_sf, 'getwindowsversion'):
    import ctypes as _ct
    _g = _ct.windll.kernel32.GetCommandLineW; _g.restype = _ct.c_wchar_p
    _c = _ct.windll.shell32.CommandLineToArgvW
    _c.argtypes = [_ct.c_wchar_p, _ct.POINTER(_ct.c_int)]
    _c.restype = _ct.POINTER(_ct.c_wchar_p)
    _n = _ct.c_int(); _a = _c(_g(), _ct.byref(_n))
    _sf.argv = [_a[i] for i in range(_n.value)]
    _ct.windll.kernel32.LocalFree(_a)
    # CommandLineToArgvW includes the interpreter exe as argv[0], which
    # shifts every real argument right by one slot vs. Python's normal
    # argv layout.  Pop it so argv[1] is the first argument again.
    if _sf.argv and _sf.argv[0].lower().endswith(('.exe', '.com')):
        _sf.argv.pop(0)
    del _n, _a, _g, _c, _ct
del _sf

import base64 as _b64
# When C++ calls CreateProc(python.exe, "<script> <workDir> ..."), the raw
# command-line becomes "python.exe" "<script>" "<workDir>" ..., so
# sys.argv[0] is the interpreter and argv[1] is the script path.  The
# base64-encoded workDir can land at argv[2] (or later).  Scan ALL entries.
for _bi in range(1, len(__import__('sys').argv)):
    _bw = __import__('sys').argv[_bi]
    if _bw.startswith('b64:'):
        try:
            _bb = _bw[4:]
            # FBase64::Encode (UE) omits '=' padding.  Compute the correct
            # number of padding chars Python's b64decode expects.
            _pad = 4 - len(_bb) % 4
            if _pad != 4:
                _bb += '=' * _pad
            _decoded_bytes = _b64.b64decode(_bb)
            # FTCHARToUTF8 in C++ converts TCHAR (UTF-16LE) → UTF-8.
            # Try UTF-8 first, fall back to UTF-16LE if it fails.
            try:
                _decoded = _decoded_bytes.decode('utf-8')
            except UnicodeDecodeError:
                _decoded = _decoded_bytes.decode('utf-16-le')
            __import__('sys').argv[_bi] = _decoded
        except Exception as _e:
            import builtins as _bi2
            _bi2.print('[B64ERR]', repr(_e), '| falling back to raw argv', flush=True)
            del _bi2
        break
try:
    del _b64, _bi, _bw, _bb, _pad
except NameError:
    pass

import subprocess
import os
import sys
import argparse
import shutil
import time
import importlib
import importlib.util
import tempfile


def _stamp():
    return time.strftime("%H:%M:%S", time.localtime())


def printImmediately(*args, **kwargs):
    print(_stamp(), "[OpenSplat4D]", *args, **kwargs, flush=True)


def _fail(msg):
    printImmediately("ERROR:", msg, file=sys.stderr)
    sys.exit(1)


def _quote_path(p):
    """Wrap a filesystem path in double quotes when it contains spaces so that
    shell=True (cmd.exe) does not split it into multiple tokens. Already-quoted
    paths are left untouched."""
    s = str(p)
    if not s:
        return s
    if s[0] == '"' and s[-1] == '"':
        return s
    if ' ' in s or '\t' in s:
        return f'"{s}"'
    return s


class OpenSplat4DHelper:
    def __init__(self):
        parser = argparse.ArgumentParser(description='OpenSplat4D Helper')
        parser.add_argument("workDir", help="work directory")
        parser.add_argument('-s', '--sparse', action='store_true', help='execute sparse reconstruction')
        parser.add_argument('-v', '--view', action='store_true', help='execute colmap view')
        parser.add_argument('-d', '--edit', action='store_true', help='execute colmap edit')
        parser.add_argument('-c', '--colmap', help='colmap executable path')
        parser.add_argument('-g', '--gaussian', help='gaussian-splatting (or 4d-gaussian-splatting) repo dir')
        parser.add_argument('-e', '--extractor', help='extra feature_extractor params')
        parser.add_argument('-mat', '--matcher', help='extra exhaustive_matcher params')
        parser.add_argument('-map', '--mapper', help='extra mapper params')
        parser.add_argument('-a', '--aligner', help='extra model_aligner params')
        parser.add_argument('-t', '--train', help='extra train params')
        parser.add_argument('--4d', dest='fourd', action='store_true', help='train a 4D (spatio-temporal) gaussian model')
        # [Enhanced] 使用 OpenSplat4D/Scripts/train_enhanced.py 替代 teachers/train.py，
        # 启用 depth loss / 过曝降权 / mask-aware L1 / Coarse-to-Fine 等增强项。
        # 详见 train_enhanced.py 顶部文档。开启后 --gaussian 仍指向 teachers 目录
        # (用于 import GaussianModel/Scene)，但实际执行的是 overlay 脚本。
        parser.add_argument('--enhanced', action='store_true',
                            help='use train_enhanced.py overlay (depth loss, overexp downweight, mask-aware L1)')
        parser.add_argument('--lambda_depth', type=float, default=0.05,
                            help='[enhanced only] depth loss weight, 0=off, recommended 0.02~0.1')
        parser.add_argument('--overexp_weight', type=float, default=0.2,
                            help='[enhanced only] L1 weight for overexposed pixels (0=ignore, 1=no downweight)')
        parser.add_argument('--resolution_coarse', type=int, default=0,
                            help='[enhanced only] coarse-to-fine first-stage long-edge resolution, 0=off')
        parser.add_argument('--coarse_until_iter', type=int, default=5000,
                            help='[enhanced only] iterations to stay at coarse resolution')
        parser.add_argument('--python', dest='python_override', help='explicit python interpreter to run training/clip (overrides auto-detection)', default=None)
        parser.add_argument('--clip', action='store_true', help='execute clip')
        parser.add_argument('--hlod_clip', action='store_true', help='recursively clip all point clouds under work dir')
        parser.add_argument('--clip_threshold', type=float, help='', default=0.8)
        parser.add_argument('--mask_dilation', type=int, help='', default=100)
        parser.add_argument('--mask_dir', default=None, help='mask directory (default: <workDir>/masks)')
        parser.add_argument('--batch_mask_subdir', default='masks', help='mask subdir name used in batch clip')
        parser.add_argument('--model_type', default='bin', choices=['bin', 'txt'], help='colmap model type for clip')
        parser.add_argument('--ply', help='')

        self.args = parser.parse_args()
        self.scriptDir = os.path.dirname(os.path.abspath(__file__))

        # [E2] Validate work dir up-front.
        if not os.path.isdir(self.args.workDir):
            _fail(f"workDir does not exist: {self.args.workDir}")
        os.chdir(self.args.workDir)

        # [E2] Validate colmap presence when needed.
        needs_colmap = self.args.sparse or self.args.view or self.args.edit
        if needs_colmap and not self.args.colmap:
            _fail("--colmap is required for --sparse/--view/--edit")
        if self.args.colmap and not (os.path.isfile(self.args.colmap) or shutil.which(self.args.colmap)):
            _fail(
                f"--colmap executable not found: {self.args.colmap}\n"
                f"  Please set the full path to colmap.exe in UE Editor:\n"
                f"    [Settings] -> OpenSplat4D -> ColmapExecutablePath\n"
                f"  (e.g. C:/Program Files/Colmap/colmap.exe), or add colmap to your system PATH.\n"
                f"  在 UE 编辑器的【设置】→ OpenSplat4D → ColmapExecutablePath 填写 colmap 的完整路径，\n"
                f"  或把 colmap 加入系统 PATH 后重启编辑器。"
            )

        # [E2] Validate gaussian repo when training.
        if (self.args.gaussian or self.args.fourd) and self.args.gaussian:
            if not os.path.isdir(self.args.gaussian):
                _fail(f"--gaussian repo dir not found: {self.args.gaussian}")

        if self.args.sparse:
            self.executeSparseReconstruction()
        if self.args.view:
            self.executeColmapView()
        if self.args.edit:
            self.executeColmapEdit()
        if (self.args.gaussian and len(self.args.gaussian) > 0) or self.args.fourd:
            self.executeGaussianSplatting()
        if self.args.clip:
            self.executeGaussianSplattingClip()
        if self.args.hlod_clip:
            self.executeHLODBatchClip()

    # [E3] Propagate failures so the UE orchestrator gets a non-zero exit code.
    # COLMAP is known to exit non-zero on recoverable warnings (discarded
    # reconstructions, CHOLMOD issues).  Pass bStrict=False for commands where
    # the output files are the real signal of success.
    # NOTE: env_ext defaults to None (not {}) to avoid the classic Python
    # mutable-default-argument bug — a shared dict would accumulate state
    # across calls if anything ever appended to it.
    def runCommand(self, command, env_ext=None, bStrict=True):
        env = os.environ.copy()
        if env_ext:
            env.update(env_ext)
        printImmediately("run command:", command)
        process = subprocess.Popen(
            command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            stdin=subprocess.DEVNULL, cwd=self.args.workDir, env=env,
            shell=True, universal_newlines=True,
        )
        for output in iter(process.stdout.readline, ''):
            if output:
                printImmediately(output.strip())
        process.stdout.close()
        process.wait()
        if process.returncode != 0:
            msg = f"Command exited {process.returncode}: {command}"
            if bStrict:
                printImmediately(f"FATAL: {msg}", file=sys.stderr)
                sys.exit(process.returncode)
            else:
                printImmediately(f"Warning (non-fatal): {msg}")
        return process.returncode

    def executeSparseReconstruction(self):
        colmap = _quote_path(self.args.colmap)
        if os.path.exists("./sparse"):
            shutil.rmtree("./sparse")
        os.makedirs("./images", exist_ok=True)
        os.makedirs("./sparse/0", exist_ok=True)
        if os.path.exists("./database.db"):
            os.remove("./database.db")
        command = f"{colmap} feature_extractor --database_path ./database.db --image_path ./images --ImageReader.camera_model SIMPLE_PINHOLE"
        if os.path.exists("./masks"):
            command += " --ImageReader.mask_path ./masks "
        if self.args.extractor:
            command += str(self.args.extractor)
        self.runCommand(command)

        command = f"{colmap} exhaustive_matcher --database_path ./database.db "
        if self.args.matcher:
            command += str(self.args.matcher)
        self.runCommand(command)

        # NOTE: COLMAP 4.x removed the legacy `--Mapper.fix_existing_images`
        # option (it now models "frames"/"rigs"); since the plugin always runs
        # the mapper from scratch (database.db + sparse/ are wiped above), the
        # option is unnecessary and only causes "unrecognised option" errors.
        #
        # COLMAP mapper / aligner can exit non-zero on recoverable warnings
        # (CHOLMOD, discarded small reconstructions) while still producing
        # valid output.  Run them non-strict and verify the result files below.
        command = f"{colmap} mapper --database_path ./database.db --image_path ./images --output_path ./sparse "
        if self.args.mapper:
            command += str(self.args.mapper)
        self.runCommand(command, bStrict=False)

        # --- Model alignment (into the UE capture frame) -------------------
        # cameras.txt (written by the Capture step) lists every image's camera
        # position in metres relative to the subject's bounds origin.
        # `model_aligner --alignment_type custom` registers the COLMAP model
        # into that frame so the trained splat lands correctly in Unreal.
        #
        # COMPATIBILITY FIX (2026-08, fan report "colmap alignment broken"):
        # --robust_alignment / --robust_alignment_max_error only existed in
        # COLMAP 3.8/3.9 and were REMOVED again in later releases (see
        # colmap/colmap issues #2695 / #2645). Passing them on a current
        # COLMAP makes model_aligner exit with an unrecognized-option error
        # WITHOUT writing any output. Combined with bStrict=False that used
        # to be swallowed silently -> the model was trained UNALIGNED and
        # appeared at the wrong position/scale in UE.
        # We now only pass flags that exist in every COLMAP version
        # (--alignment_max_error), retry with a looser threshold, and fail
        # LOUDLY (instead of silently) when alignment never succeeds.
        if not os.path.isfile("./cameras.txt"):
            printImmediately(
                "WARNING: cameras.txt not found - skipping model alignment. "
                "The trained model will stay in COLMAP's arbitrary coordinate "
                "frame (expected when reconstructing a foreign colmap workspace).")
        else:
            aligner_base = (
                f"{colmap} model_aligner"
                f" --input_path ./sparse/0 --output_path ./sparse/0"
                f" --ref_images_path ./cameras.txt"
                f" --ref_is_gps 0 --alignment_type custom")
            # Custom aligner params configured in the UE editor (if any).
            custom = f" {self.args.aligner}" if self.args.aligner else ""
            aligned_rc = None
            for max_error in ("3", "10"):
                cmd = f"{aligner_base} --alignment_max_error {max_error}{custom}"
                aligned_rc = self.runCommand(cmd, bStrict=False)
                if aligned_rc == 0:
                    printImmediately(
                        f"model_aligner succeeded (alignment_max_error={max_error}).")
                    break
                printImmediately(
                    f"model_aligner failed (rc={aligned_rc}) with "
                    f"alignment_max_error={max_error}; retrying with looser threshold ...")
            if aligned_rc != 0:
                _fail(
                    "COLMAP model alignment failed. 稀疏重建完成，但模型对齐失败：\n"
                    "  - 确认 COLMAP 版本（旧版参数差异见脚本内注释）\n"
                    "  - 确认工作目录 cameras.txt 的文件名与 images/ 中一致\n"
                    "  - 可在 UE【稀疏重建】步骤的『模型对齐参数（自定义）』中调整参数后重试\n"
                    "Model was NOT aligned; aborting so training cannot silently\n"
                    "produce a splat in the wrong coordinate frame.")

        # COLMAP can exit non-zero on warnings yet still write valid output.
        # Trust the files, not the exit code.
        model_type = self.args.model_type if hasattr(self.args, 'model_type') else 'bin'
        ext = 'bin' if model_type == 'bin' else 'txt'
        cameras_file = os.path.join('./sparse/0', f'cameras.{ext}')
        images_file  = os.path.join('./sparse/0', f'images.{ext}')
        points_file  = os.path.join('./sparse/0', f'points3D.{ext}')
        if not (os.path.isfile(cameras_file) and os.path.isfile(images_file) and os.path.isfile(points_file)):
            _fail(
                f"Sparse reconstruction output incomplete.\n"
                f"  Expected: {cameras_file}, {images_file}, {points_file}\n"
                f"  At least one of these files is missing. Check the COLMAP log above for details."
            )
        printImmediately("Sparse reconstruction verified: cameras, images, points3D all present.")

    def executeColmapView(self):
        colmap_executable_path = _quote_path(self.args.colmap)
        colmap_directory = os.path.dirname(os.path.dirname(self.args.colmap))
        plugins_path = os.path.join(colmap_directory, "plugins")
        env = {"QT_PLUGIN_PATH": plugins_path}
        command = f"{colmap_executable_path} gui --database_path ./database.db --image_path ./images --import_path ./sparse/0"
        self.runCommand(command, env)

    def executeColmapEdit(self):
        colmap_executable_path = _quote_path(self.args.colmap)
        colmap_directory = os.path.dirname(os.path.dirname(self.args.colmap))
        plugins_path = os.path.join(colmap_directory, "plugins")
        env = {"QT_PLUGIN_PATH": plugins_path}
        command = f"{colmap_executable_path} gui --database_path ./database.db --image_path ./images "
        if os.path.exists("./masks"):
            command += " --ImageReader.mask_path ./masks "
        self.runCommand(command, env)

    # [E9] Resolve the Python interpreter used to launch the actual training /
    # clip subprocesses. We do NOT use `conda activate X && python`: a shipped
    # plugin must not assume the end user has conda, nor that PATH's `python` is
    # ABI-compatible with the CUDA extensions (_C.pyd) we compiled. Resolution
    # order:
    #   1) --python <path>        (explicit override from the C++ / CLI caller)
    #   2) OPENSPLAT4D_PYTHON     (env var for power users)
    #   3) <Plugin>/ThirdParty/Python/python.exe   (bundled with the plugin)
    #   4) sys.executable         (the interpreter UE launched THIS helper with)
    #   5) "python"               (last-resort PATH lookup)
    # Every candidate is checked for existence before being used.
    def _python_executable(self):
        candidates = []
        if self.args.python_override:
            candidates.append(self.args.python_override)
        env_var = os.environ.get("OPENSPLAT4D_PYTHON")
        if env_var:
            candidates.append(env_var)
        # Bundled python lives at <Plugin>/ThirdParty/Python/python.exe.
        # scriptDir = <Plugin>/Scripts, so go up one level then into ThirdParty/Python.
        plugin_root = os.path.dirname(self.scriptDir)
        candidates.append(os.path.join(plugin_root, "ThirdParty", "Python", "python.exe"))
        candidates.append(sys.executable)
        candidates.append("python")
        for cand in candidates:
            if cand and os.path.exists(cand):
                return cand
        # Fall through: let it fail loudly with a clear "not found" downstream.
        return candidates[-1]

    # [E9] Prepend the selected python's native runtime dirs (Library/bin, etc.)
    # to PATH so torch's CUDA runtime DLLs and any shipped native deps are found
    # when the compiled extension modules (_C.pyd, _C.cp310...) are loaded.
    def _env_path_ext(self):
        exe = self._python_executable()
        env_dir = os.path.dirname(exe)
        extra = []
        for sub in (os.path.join(env_dir, "Library", "bin"),
                    os.path.join(env_dir, "Scripts"),
                    os.path.join(env_dir, "bin")):
            if os.path.isdir(sub):
                extra.append(sub)
        if not extra:
            return {}
        cur = os.environ.get("PATH", "")
        return {"PATH": os.pathsep.join(extra + [cur])}

    def executeGaussianSplatting(self):
        # [E10/E11] Ensure the CUDA extensions are available. Preferred path:
        # the plugin was built WITH precompiled extensions (build_plugin.bat
        # ships the _C.pyd in the dedicated <plugin>/Extensions folder, OUTSIDE
        # ThirdParty), so the end user needs NO compiler. Only if a .pyd is
        # genuinely missing AND a toolchain is present do we self-build it here.
        # Either way, make sure the Extension folder is on PYTHONPATH for the
        # train.py child process.
        self.ensure_extensions_built()

        # [E9] Use the resolved interpreter directly instead of
        # `conda activate ... && python`. See _python_executable for why.
        python_exe = _quote_path(self._python_executable())
        gaussian = _quote_path(self.args.gaussian)
        script_dir = _quote_path(self.scriptDir)
        env_ext = self._env_path_ext()
        env_ext.update(self._ext_pypath_env())

        if self.args.fourd:
            # 4d-gaussian-splatting (Wu et al.) training entry point.
            command = f'{python_exe} {gaussian}/train.py -s . -m ./output '
        elif self.args.enhanced:
            # [Enhanced] 走 train_enhanced.py overlay —— 在 teachers 之上添加
            # depth loss / 过曝降权 / mask-aware L1 / Coarse-to-Fine。
            # --gaussian 仍指向 teachers 目录（用于 import GaussianModel/Scene/render），
            # 但实际入口是 OpenSplat4D/Scripts/train_enhanced.py。
            enhanced_script = _quote_path(os.path.join(self.scriptDir, "train_enhanced.py"))
            cmd_parts = [f'{python_exe} {enhanced_script} -s . -m ./output',
                         f'--lambda_depth {self.args.lambda_depth}',
                         f'--overexp_weight {self.args.overexp_weight}']
            if os.path.exists("./depths"):
                # depths 目录存在时，让 train_enhanced.py 自动加载（用 image_name 匹配）
                # 不需要显式 --depths 参数（train_enhanced.py 会从 source_path/depths 自动找）
                pass
            if self.args.resolution_coarse > 0:
                cmd_parts.append(f'--resolution_coarse {self.args.resolution_coarse}')
                cmd_parts.append(f'--coarse_until_iter {self.args.coarse_until_iter}')
            command = ' '.join(cmd_parts) + ' '
        elif os.path.exists("./depths"):
            command = (f'{python_exe} {script_dir}/make_depth_scale.py --base_dir . '
                       f'--depths_dir ./depths && {python_exe} {gaussian}/train.py -s . -m ./output --depths ./depths ')
        else:
            command = f'{python_exe} {gaussian}/train.py -s . -m ./output '

        if self.args.train:
            command += str(self.args.train)
        self.runCommand(command, env_ext)

    def executeGaussianSplattingClip(self):
        # [E1] Fully implemented (the reference never defined this method).
        if not self.args.ply:
            _fail("--clip requires --ply")
        ply_path = self.args.ply
        if not os.path.exists(ply_path):
            _fail(f"--ply not found: {ply_path}")
        file_name, file_ext = os.path.splitext(ply_path)
        out_ply_path = file_name + "_clipped" + file_ext
        mask_dir = self.args.mask_dir or os.path.join(self.args.workDir, "masks")
        if not os.path.isdir(mask_dir):
            _fail(f"mask dir not found: {mask_dir}")
        command = (
            f'{_quote_path(self._python_executable())} {_quote_path(self.scriptDir)}/clip_model.py'
            f" --base_dir {_quote_path(self.args.workDir)}"
            f" --ply_path {_quote_path(ply_path)}"
            f" --output_ply_path {_quote_path(out_ply_path)}"
            f" --mask_dir {_quote_path(mask_dir)}"
            f" --mask_dilation {self.args.mask_dilation}"
            f" --mask_clip_threshold {self.args.clip_threshold}"
            f" --model_type {self.args.model_type}"
        )
        self.runCommand(command, self._env_path_ext())

    # [E6] Batch clip convenience wiring into the enhanced clip_model batch mode.
    def executeHLODBatchClip(self):
        mask_dir = self.args.mask_dir or os.path.join(self.args.workDir, "masks")
        command = (
            f'{_quote_path(self._python_executable())} {_quote_path(self.scriptDir)}/clip_model.py'
            f" --batch_root {_quote_path(self.args.workDir)}"
            f" --batch_mask_subdir {self.args.batch_mask_subdir}"
            f" --mask_dilation {self.args.mask_dilation}"
            f" --mask_clip_threshold {self.args.clip_threshold}"
            f" --model_type {self.args.model_type}"
        )
        self.runCommand(command, self._env_path_ext())

    # ---------------------------------------------------------------------------
    # [E10/E11] CUDA EXTENSION AVAILABILITY (prebuilt preferred, self-build fallback)
    # ---------------------------------------------------------------------------
    # Preferred (distribution): the plugin is built WITH precompiled extensions
    # by build_plugin.bat, which compiles diff_gaussian_rasterization/_C,
    # simple_knn/_C, fused_ssim_cuda and copies the built packages into a
    # DEDICATED, lightweight folder: <plugin>/Extensions/ (at the plugin ROOT,
    # OUTSIDE ThirdParty). This keeps the compiled binaries OUT of the huge
    # ThirdParty tree (esp. ThirdParty/Python) so an end user can update just
    # the small Extensions/ folder without re-copying the heavy Python. The
    # .pyd is a FATBIN covering RTX 30/40/50 (sm_86/90/120 + PTX), so a single
    # build runs on every target GPU. At runtime we add <plugin>/Extensions to
    # sys.path/PYTHONPATH so the shipped .pyd imports with NO compiler on the
    # user's machine (no editable .pth required).
    #
    # Fallback (developer only): if a .pyd is genuinely missing AND a toolchain
    # is present, we self-build it in-place with the same toolchain as
    # build_ext.bat, then COPY it into <plugin>/Extensions:
    #   - Visual Studio 2026 x64 native tools (vcvars64, MSVC 14.38 toolset,
    #     because CUDA 12.8 rejects the VS2026 default 14.51 toolset)
    #   - CUDA 12.8 on PATH
    #   - pip install -e . (editable compat mode => _C.pyd lands next to the
    #     package __init__.py)
    # The build only happens when a .pyd is actually absent, so normal training
    # is unaffected after the one-time compile.
    # ---------------------------------------------------------------------------
    _EXT_PACKAGES = ["diff_gaussian_rasterization", "simple_knn", "fused_ssim"]

    def _locate_vs(self):
        """Return the VS install root via vswhere, or None."""
        vswhere = os.path.join(
            os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"),
            "Microsoft Visual Studio", "Installer", "vswhere.exe")
        if not os.path.isfile(vswhere):
            return None
        try:
            out = subprocess.run(
                [vswhere, "-latest", "-products", "*",
                 "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                 "-property", "installationPath"],
                capture_output=True, text=True, shell=True)
            path = out.stdout.strip()
            return path or None
        except Exception:
            return None

    def _locate_cuda(self):
        """Return the CUDA toolkit root (dir holding bin/nvcc.exe), or None."""
        for env_key in ("CUDA_PATH", "CUDA_HOME"):
            env = os.environ.get(env_key)
            if env and os.path.isfile(os.path.join(env, "bin", "nvcc.exe")):
                return env
        toolkit = r"C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA"
        if os.path.isdir(toolkit):
            # newest v12.x first (torch cu128 expects CUDA 12.8)
            cands = sorted((d for d in os.listdir(toolkit) if d.lower().startswith("v")),
                           reverse=True)
            for c in cands:
                nvcc = os.path.join(toolkit, c, "bin", "nvcc.exe")
                if os.path.isfile(nvcc):
                    return os.path.join(toolkit, c)
        return None

    def _package_location(self, pkg):
        """Directory of a package's __init__.py (via find_spec, no import)."""
        try:
            spec = importlib.util.find_spec(pkg)
        except Exception:
            spec = None
        if spec is None or spec.origin is None:
            return None
        origin = spec.origin
        if origin.endswith("__init__.py") or origin.endswith("__init__.pyc"):
            return os.path.dirname(origin)
        # single-module package
        return os.path.dirname(origin)

    # ---------------------------------------------------------------------------
    # [E11] DISTRIBUTION-SAFE EXTENSION RESOLUTION (no editable .pth needed)
    # ---------------------------------------------------------------------------
    # The compiled _C.pyd / fused_ssim_cuda.pyd are SHIPPED in a DEDICATED,
    # lightweight folder: <plugin>/Extensions/ (plugin ROOT, OUTSIDE ThirdParty),
    # built ahead of time by build_plugin.bat and packaged with the plugin. This
    # means an end user does NOT need a C++ compiler / CUDA toolkit to train:
    # the .pyd is already in the plugin. We only have to make sure <plugin>/
    # Extensions is on sys.path / PYTHONPATH so `import diff_gaussian_rasterization`
    # (and the others) find the in-place .pyd WITHOUT relying on the editable
    # `pip install -e` .pth file. <gaussian>/submodules is kept ONLY as a
    # fallback (e.g. a developer self-build); at runtime Extensions wins.
    # ---------------------------------------------------------------------------
    def _ext_deploy_dir(self):
        """Dedicated prebuilt-extensions folder at the plugin ROOT, OUTSIDE
        ThirdParty (e.g. <plugin>/Extensions). This is the PRIMARY location the
        shipped plugin imports the compiled CUDA extensions from."""
        return os.path.join(os.path.dirname(self.scriptDir), "Extensions")

    def _ext_search_dirs(self):
        """Directories that must be on sys.path/PYTHONPATH so the compiled
        CUDA extension packages can be imported without an editable .pth.
        The plugin-bundled <plugin>/Extensions is PRIMARY; the requested repo's
        `submodules` and the bundled 3DGS `submodules` are fallbacks only."""
        dirs = []
        ext = self._ext_deploy_dir()
        if os.path.isdir(ext) and ext not in dirs:
            dirs.append(ext)
        if self.args.gaussian:
            d = os.path.join(self.args.gaussian, "submodules")
            if os.path.isdir(d) and d not in dirs:
                dirs.append(d)
        plugin_root = os.path.dirname(self.scriptDir)
        bundled = os.path.join(plugin_root, "ThirdParty", "gaussian-splatting", "submodules")
        if os.path.isdir(bundled) and bundled not in dirs:
            dirs.append(bundled)
        return dirs

    def _ensure_ext_on_path(self):
        """Prepend the extension search dirs to THIS process's sys.path so the
        in-process import probe (_package_built) can find the shipped .pyd
        without an editable .pth. Idempotent."""
        for d in self._ext_search_dirs():
            if d not in sys.path:
                sys.path.insert(0, d)

    def _ext_pypath_env(self):
        """Env dict that extends PYTHONPATH with the extension search dirs so
        child processes (train.py) can import the shipped extensions too."""
        dirs = self._ext_search_dirs()
        if not dirs:
            return {}
        cur = os.environ.get("PYTHONPATH", "")
        new = os.pathsep.join(dirs + ([cur] if cur else []))
        return {"PYTHONPATH": new}

    def _package_built(self, pkg):
        """True if the package imports successfully (its compiled extension is
        present and loadable). We probe by actually importing rather than
        globbing for a .pyd, because the compiled module may be a top-level
        module (e.g. fused_ssim_cuda) rather than a _C inside the package dir.
        The real exception is logged so an import failure is never masked.
        We also drop any stale partial import from sys.modules first so a
        re-probe after the search dirs were added sees the real result."""
        try:
            sys.modules.pop(pkg, None)
            importlib.import_module(pkg)
            return True
        except Exception as e:
            printImmediately(f"    (import probe failed for {pkg}: {type(e).__name__}: {e})")
            return False

    def _copy_ext_pkg(self, src_pkg_dir, deploy_pkg_dir):
        """Copy a built extension package dir (e.g. diff_gaussian_rasterization,
        which contains __init__.py + _C.pyd) into the deploy Extensions folder,
        replacing any previous copy. Falls back to merge-copy if replacement
        fails (e.g. a file is locked by a running editor)."""
        if not os.path.isdir(src_pkg_dir):
            return False
        try:
            if os.path.isdir(deploy_pkg_dir):
                shutil.rmtree(deploy_pkg_dir)
            shutil.copytree(src_pkg_dir, deploy_pkg_dir)
        except Exception as e:
            printImmediately(f"    (copy {deploy_pkg_dir} failed: {type(e).__name__}: {e}; trying merge)")
            try:
                if not os.path.isdir(deploy_pkg_dir):
                    os.makedirs(deploy_pkg_dir)
                for item in os.listdir(src_pkg_dir):
                    s = os.path.join(src_pkg_dir, item)
                    d = os.path.join(deploy_pkg_dir, item)
                    if os.path.isdir(s):
                        shutil.copytree(s, d, dirs_exist_ok=True)
                    else:
                        shutil.copy2(s, d)
            except Exception as e2:
                printImmediately(f"    (merge copy also failed: {type(e2).__name__}: {e2})")
                return False
        return True

    def _deploy_built_extensions(self):
        """Copy the freshly built extension packages from the gaussian repo's
        submodules into the plugin's dedicated Extensions/ folder (plugin ROOT,
        OUTSIDE ThirdParty). Idempotent; only copies when a built .pyd exists.
        This is what makes the shipped plugin import the prebuilt extensions
        from the lightweight Extensions/ folder with no compiler."""
        src_root = None
        if self.args.gaussian:
            cand = os.path.join(self.args.gaussian, "submodules")
            if os.path.isdir(cand):
                src_root = cand
        if src_root is None:
            return
        deploy = self._ext_deploy_dir()
        os.makedirs(deploy, exist_ok=True)
        # diff_gaussian_rasterization (inner package dir with _C.pyd)
        self._copy_ext_pkg(
            os.path.join(src_root, "diff-gaussian-rasterization", "diff_gaussian_rasterization"),
            os.path.join(deploy, "diff_gaussian_rasterization"))
        # simple_knn (inner package dir with _C.pyd)
        self._copy_ext_pkg(
            os.path.join(src_root, "simple-knn", "simple_knn"),
            os.path.join(deploy, "simple_knn"))
        # fused_ssim: top-level fused_ssim_cuda*.pyd + the fused_ssim package dir
        fused_dir = os.path.join(src_root, "fused-ssim")
        if os.path.isdir(fused_dir):
            for f in os.listdir(fused_dir):
                if f.startswith("fused_ssim_cuda") and f.endswith(".pyd"):
                    shutil.copy2(os.path.join(fused_dir, f), os.path.join(deploy, f))
            self._copy_ext_pkg(
                os.path.join(fused_dir, "fused_ssim"),
                os.path.join(deploy, "fused_ssim"))

    def _ensure_py_runtime_on_path(self):
        """Prepend the bundled python's native runtime dirs (Library/bin, which
        holds torch's CUDA runtime DLLs) to PATH for the in-process import probe
        AND register them as a DLL search dir, so `import diff_gaussian_rasterization`
        can actually load torch/cu128 instead of failing with a DLL-not-found
        that would otherwise masquerade as 'extension not built'."""
        ext = self._env_path_ext()
        p = ext.get("PATH")
        if p:
            os.environ["PATH"] = p
        exe = self._python_executable()
        lib_bin = os.path.join(os.path.dirname(exe), "Library", "bin")
        if os.path.isdir(lib_bin):
            try:
                os.add_dll_directory(lib_bin)
            except Exception:
                pass

    def ensure_extensions_built(self):
        if not (self.args.gaussian or self.args.fourd):
            return
        # [E11] Make the shipped in-place .pyd discoverable BEFORE probing, so a
        # plugin that was built WITH precompiled extensions (build_plugin.bat)
        # works for the end user with no compiler at all. Only if the probe
        # still fails do we fall back to self-building (which needs VS+CUDA).
        self._ensure_ext_on_path()
        # Make sure the bundled python's native runtime dirs (torch CUDA DLLs,
        # etc.) are on PATH for the in-process import probe and the build.
        self._ensure_py_runtime_on_path()
        printImmediately("checking CUDA extensions (prebuilt if shipped, self-build if missing) ...")
        python = self._python_executable()
        built_any = False
        for pkg in self._EXT_PACKAGES:
            if self._package_built(pkg):
                printImmediately(f"  [ok]   {pkg} (imports OK)")
                continue
            # Resolve the build dir explicitly to the gaussian repo's submodules
            # (where setup.py lives). Prefer <gaussian>/submodules/<pkg>; only
            # fall back to the import-resolved location if --gaussian is absent.
            if self.args.gaussian:
                build_dir = os.path.join(self.args.gaussian, "submodules", pkg)
            else:
                pkg_dir = self._package_location(pkg)
                build_dir = os.path.dirname(pkg_dir) if pkg_dir else None
            if not build_dir or not os.path.isdir(build_dir):
                printImmediately(f"  [skip] {pkg}: build dir not found ({build_dir})")
                continue
            printImmediately(f"  [build] {pkg} import failed -> compiling in {build_dir}")
            self._build_extension(python, build_dir)
            # Ship the freshly built package into the dedicated Extensions/
            # folder (outside ThirdParty) so the running plugin imports it from
            # there; this also keeps the self-build result consistent with the
            # prebuilt distribution layout.
            self._deploy_built_extensions()
            built_any = True
        if built_any:
            for pkg in self._EXT_PACKAGES:
                if not self._package_built(pkg):
                    _fail(f"self-build finished but {pkg} still fails to import")
            printImmediately("self-build verified: all extensions import OK")

    def _build_extension(self, python, build_dir):
        vs = self._locate_vs()
        cuda = self._locate_cuda()
        if not vs:
            _fail("Visual Studio (MSVC) not found via vswhere, and the CUDA "
                  "extensions are not prebuilt in this plugin. The shipped plugin "
                  "should already contain the compiled _C.pyd files (built by "
                  "build_plugin.bat). If you are an end user, please obtain a plugin "
                  "build that includes the precompiled extensions; if you are the "
                  "developer, run build_plugin.bat (or install the 'Desktop "
                  "development with C++' workload for Visual Studio 2026).")
        if not cuda:
            _fail("CUDA toolkit (nvcc) not found, and the CUDA extensions are not "
                  "prebuilt in this plugin. The shipped plugin should already contain "
                  "the compiled _C.pyd files (built by build_plugin.bat against "
                  "CUDA 12.8). If you are an end user, please obtain a plugin build "
                  "that includes the precompiled extensions; if you are the developer, "
                  "install CUDA 12.8 and re-run build_plugin.bat.")
        vcvars = os.path.join(vs, "VC", "Auxiliary", "Build", "vcvars64.bat")
        # Build for RTX 20/30/40 (8.6/9.0) plus PTX for RTX 50 (12.0). Override
        # with OPENSPLAT4D_CUDA_ARCHS if a different mix is needed.
        archs = os.environ.get("OPENSPLAT4D_CUDA_ARCHS", "8.6;9.0;12.0+PTX")
        # IMPORTANT: write a MULTI-LINE build script (mirroring build_ext.bat).
        # A single-line `cmd /c "call vcvars && set PATH=x;%PATH% && ..."` expands
        # %PATH% UP FRONT (before vcvars runs), which clobbers the MSVC bin that
        # vcvars adds -> `cl.exe` disappears from PATH -> ninja fails with
        # "CreateProcess failed / The system cannot find the file specified".
        # With separate lines, %PATH% is re-expanded per line, so the vcvars-added
        # MSVC path is preserved and cl.exe is found.
        bat_lines = [
            "@echo off",
            "setlocal",
            f'call "{vcvars}" -vcvars_ver=14.38.33130',
            "if errorlevel 1 exit /b 1",
            f'set "CUDA12={cuda}"',
            "set PATH=%CUDA12%\\bin;%PATH%",
            "set CUDA_PATH=%CUDA12%",
            "set CUDA_HOME=%CUDA12%",
            "set DISTUTILS_USE_SDK=1",
            "set MSSdk=1",
            f'set "TORCH_CUDA_ARCH_LIST={archs}"',
            f'cd /d "{build_dir}"',
            f'"{python}" -m pip install -e . --no-build-isolation --no-deps '
            f'--config-settings editable_mode=compat',
            "if errorlevel 1 exit /b 1",
            "endlocal",
        ]
        bat = os.path.join(build_dir, "_opensplat4d_build_tmp.bat")
        with open(bat, "w") as f:
            f.write("\n".join(bat_lines) + "\n")
        # runCommand honors cwd=self.args.workDir; point it at the build dir so
        # the editable install resolves the right pyproject.toml, then restore.
        prev_workdir = self.args.workDir
        self.args.workDir = build_dir
        try:
            self.runCommand(f'"{bat}"', self._env_path_ext())
        finally:
            self.args.workDir = prev_workdir
            try:
                os.remove(bat)
            except Exception:
                pass


if __name__ == "__main__":
    OpenSplat4DHelper = OpenSplat4DHelper()
