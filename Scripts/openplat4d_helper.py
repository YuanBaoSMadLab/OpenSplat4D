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
#  [E4] `--no_conda` skips `conda activate` (for venv / pipx / system Python
#       users); `--conda_env` still selects the env when activation is wanted.
#  [E5] Timestamped, tagged logging so multiple pipeline steps are easy to
#       follow in the UE output log.
#  [E6] `--hlod_clip` convenience: recursively clip every produced point cloud
#       under the work dir using the enhanced clip_model.py batch workflow.
#  [E7] The 4D training branch selects the `gaussian_splatting_4d` env by
#       default and forwards `--train` overrides verbatim (never reduced).
# ---------------------------------------------------------------------------

import subprocess
import os
import sys
import argparse
import shutil
import time


def _stamp():
    return time.strftime("%H:%M:%S", time.localtime())


def printImmediately(*args, **kwargs):
    print(_stamp(), "[OpenSplat4D]", *args, **kwargs, flush=True)


def _fail(msg):
    printImmediately("ERROR:", msg, file=sys.stderr)
    sys.exit(1)


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
        parser.add_argument('--conda_env', help='conda env to activate before training', default=None)
        parser.add_argument('--no_conda', action='store_true', help='skip conda activation (use current python)')
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
    def runCommand(self, command, env_ext={}):
        env = os.environ.copy()
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
            printImmediately(f"Command failed (exit {process.returncode}): {command}", file=sys.stderr)
            sys.exit(process.returncode)

    def executeSparseReconstruction(self):
        if os.path.exists("./sparse"):
            shutil.rmtree("./sparse")
        os.makedirs("./images", exist_ok=True)
        os.makedirs("./sparse/0", exist_ok=True)
        if os.path.exists("./database.db"):
            os.remove("./database.db")
        command = f"{self.args.colmap} feature_extractor --database_path ./database.db --image_path ./images --ImageReader.camera_model SIMPLE_PINHOLE"
        if os.path.exists("./masks"):
            command += " --ImageReader.mask_path ./masks "
        if self.args.extractor:
            command += str(self.args.extractor)
        self.runCommand(command)

        command = f"{self.args.colmap} exhaustive_matcher --database_path ./database.db "
        if self.args.matcher:
            command += str(self.args.matcher)
        self.runCommand(command)

        command = f"{self.args.colmap} mapper --database_path ./database.db --image_path ./images --output_path ./sparse  --Mapper.fix_existing_images 1 "
        if self.args.mapper:
            command += str(self.args.mapper)
        self.runCommand(command)

        command = f"{self.args.colmap} model_aligner --input_path ./sparse/0 --output_path ./sparse/0 --ref_images_path ./cameras.txt --ref_is_gps 0 --alignment_type custom --alignment_max_error 3 "
        if self.args.aligner:
            command += str(self.args.aligner)
        self.runCommand(command)

    def executeColmapView(self):
        colmap_executable_path = self.args.colmap
        colmap_directory = os.path.dirname(os.path.dirname(colmap_executable_path))
        plugins_path = os.path.join(colmap_directory, "plugins")
        env = {"QT_PLUGIN_PATH": plugins_path}
        command = f"{self.args.colmap} gui --database_path ./database.db --image_path ./images --import_path ./sparse/0"
        self.runCommand(command, env)

    def executeColmapEdit(self):
        colmap_executable_path = self.args.colmap
        colmap_directory = os.path.dirname(os.path.dirname(colmap_executable_path))
        plugins_path = os.path.join(colmap_directory, "plugins")
        env = {"QT_PLUGIN_PATH": plugins_path}
        command = f"{self.args.colmap} gui --database_path ./database.db --image_path ./images "
        if os.path.exists("./masks"):
            command += " --ImageReader.mask_path ./masks "
        self.runCommand(command, env)

    def executeGaussianSplatting(self):
        # [E7] Pick the conda env: explicit override > bundled default.
        # Both 3DGS and 4DGS training share one env ("opensplat4d") that is
        # shipped/bundled with the plugin, so 3d and 4d no longer need separate
        # envs. Override with --conda_env <name> if you keep your own env.
        if self.args.no_conda:
            conda_prefix = ""
        elif self.args.conda_env:
            conda_prefix = f"conda activate {self.args.conda_env} && "
        else:
            conda_prefix = "conda activate opensplat4d && "

        if self.args.fourd:
            # 4d-gaussian-splatting (Wu et al.) training entry point.
            command = f"{conda_prefix}python {self.args.gaussian}/train.py -s . -m ./output "
        elif os.path.exists("./depths"):
            command = (f"{conda_prefix}python {self.scriptDir}/make_depth_scale.py --base_dir . "
                       f"--depths_dir ./depths && python {self.args.gaussian}/train.py -s . -m ./output --depths ./depths ")
        else:
            command = f"{conda_prefix}python {self.args.gaussian}/train.py -s . -m ./output "

        if self.args.train:
            command += str(self.args.train)
        self.runCommand(command)

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
            f"python {self.scriptDir}/clip_model.py"
            f" --base_dir {self.args.workDir}"
            f" --ply_path {ply_path}"
            f" --output_ply_path {out_ply_path}"
            f" --mask_dir {mask_dir}"
            f" --mask_dilation {self.args.mask_dilation}"
            f" --mask_clip_threshold {self.args.clip_threshold}"
            f" --model_type {self.args.model_type}"
        )
        self.runCommand(command)

    # [E6] Batch clip convenience wiring into the enhanced clip_model batch mode.
    def executeHLODBatchClip(self):
        mask_dir = self.args.mask_dir or os.path.join(self.args.workDir, "masks")
        command = (
            f"python {self.scriptDir}/clip_model.py"
            f" --batch_root {self.args.workDir}"
            f" --batch_mask_subdir {self.args.batch_mask_subdir}"
            f" --mask_dilation {self.args.mask_dilation}"
            f" --mask_clip_threshold {self.args.clip_threshold}"
            f" --model_type {self.args.model_type}"
        )
        self.runCommand(command)


if __name__ == "__main__":
    OpenSplat4DHelper = OpenSplat4DHelper()
