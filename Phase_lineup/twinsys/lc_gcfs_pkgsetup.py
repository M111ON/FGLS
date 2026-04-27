"""
setup.py — lc_gcfs package
builds lc_api.so from src/ using gcc during pip install
"""
import os
import subprocess
import shutil
from setuptools import setup, find_packages
from setuptools.command.build_py import build_py


SRC_DIR     = os.path.join(os.path.dirname(__file__), "src")
PKG_DIR     = os.path.join(os.path.dirname(__file__), "lc_gcfs")
SO_NAME     = "lc_api.so"
SO_SRC      = os.path.join(SRC_DIR, "lc_api.c")
SO_OUT_TMP  = os.path.join(SRC_DIR, SO_NAME)
SO_OUT_PKG  = os.path.join(PKG_DIR, SO_NAME)


class BuildWithSO(build_py):
    """Compile lc_api.so before copying package files."""

    def run(self):
        self._compile_so()
        super().run()

    def _compile_so(self):
        print(f"[lc_gcfs] compiling {SO_NAME} ...")
        cmd = [
            "gcc", "-O2", "-shared", "-fPIC",
            "-I", SRC_DIR,
            "-o", SO_OUT_TMP,
            SO_SRC,
        ]
        result = subprocess.run(cmd, capture_output=True, text=True)
        if result.returncode != 0:
            raise RuntimeError(
                f"[lc_gcfs] gcc failed:\n{result.stderr}"
            )
        shutil.copy2(SO_OUT_TMP, SO_OUT_PKG)
        print(f"[lc_gcfs] {SO_NAME} → {SO_OUT_PKG} ✓")


setup(
    name="lc_gcfs",
    version="1.0.0",
    description="LetterCube GCFS — geometric file store (local org lib)",
    packages=find_packages(),
    package_data={"lc_gcfs": [SO_NAME]},
    include_package_data=True,
    cmdclass={"build_py": BuildWithSO},
    python_requires=">=3.9",
    install_requires=[],          # zero deps — ctypes is stdlib
    extras_require={
        "server": ["fastapi>=0.110", "uvicorn[standard]>=0.29"],
    },
)
