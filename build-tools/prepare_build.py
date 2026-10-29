"""Extract the customized display and CC1101 libraries shipped upstream."""
from pathlib import Path
import shutil
import zipfile

Import("env")
root = Path(env.subst("$PROJECT_DIR"))
vendor = root / "vendor"
vendor.mkdir(exist_ok=True)

for archive, folder in (
    ("TFT_eSPI-master.zip", "TFT_eSPI-master"),
    ("SmartRC-CC1101-Driver-Lib-master.zip", "SmartRC-CC1101-Driver-Lib-master"),
):
    if not (vendor / folder).exists():
        with zipfile.ZipFile(root / "Libraries" / archive) as package:
            for entry in package.infolist():
                target = (vendor / entry.filename).resolve()
                if not target.is_relative_to(vendor.resolve()):
                    raise RuntimeError("Library archive path escapes vendor directory")
            package.extractall(vendor)

shutil.copyfile(
    root / "Libraries" / "User_Setup v2.h",
    vendor / "TFT_eSPI-master" / "User_Setup.h",
)

env.Append(LINKFLAGS=["-Wl,-z,muldefs"])

def isolate_cc1101_spi(build_env, node):
    defines = list(build_env.get("CPPDEFINES", []))
    defines.append(("spi", "div_cc1101_spi"))
    return build_env.Object(node, CPPDEFINES=defines)

env.AddBuildMiddleware(
    isolate_cc1101_spi,
    "*/SmartRC-CC1101-Driver-Lib-master/ELECHOUSE_CC1101_SRC_DRV.cpp",
)
