Import("env")

import os
import re
import shutil
import subprocess


def read_config_value(config_path, key):
    pattern = re.compile(r'^#define ' + re.escape(key) + r' "([^"]+)"$')
    with open(config_path, "r", encoding="utf-8") as config_file:
        for line in config_file:
            match = pattern.match(line.strip())
            if match:
                return match.group(1)
    raise RuntimeError("Missing {} in {}".format(key, config_path))


def package_firmware(source, target, env):
    project_dir = env.subst("$PROJECT_DIR")
    build_dir = env.subst("$BUILD_DIR")
    with open(os.path.join(project_dir, "version.txt"), "r", encoding="utf-8") as version_file:
        version = version_file.read().strip()

    if not re.fullmatch(r"[0-9]+(?:\.[0-9]+){0,3}", version):
        raise RuntimeError("version.txt must contain an ESP-IDF numeric version")

    config_path = os.path.join(build_dir, "config", "sdkconfig.h")
    flash_mode = read_config_value(config_path, "CONFIG_ESPTOOLPY_FLASHMODE")
    flash_freq = read_config_value(config_path, "CONFIG_ESPTOOLPY_FLASHFREQ")
    flash_size = read_config_value(config_path, "CONFIG_ESPTOOLPY_FLASHSIZE")

    platform = env.PioPlatform()
    esptool_dir = platform.get_package_dir("tool-esptool")
    esptool = os.path.join(esptool_dir, "esptool.exe" if os.name == "nt" else "esptool")
    output_dir = os.path.join(project_dir, "release")
    os.makedirs(output_dir, exist_ok=True)
    output_path = os.path.join(output_dir, "ble-esp32-v{}.bin".format(version))
    ota_path = os.path.join(output_dir, "ble-esp32-app-v{}.bin".format(version))
    shutil.copyfile(os.path.join(build_dir, "firmware.bin"), ota_path)

    command = [
        esptool,
        "--chip", "esp32",
        "merge-bin",
        "--output", output_path,
        "--target-offset", "0",
        "--flash-mode", flash_mode,
        "--flash-freq", flash_freq,
        "--flash-size", flash_size,
        "0x1000", os.path.join(build_dir, "bootloader.bin"),
        "0x8000", os.path.join(build_dir, "partitions.bin"),
        "0x20000", os.path.join(build_dir, "firmware.bin"),
    ]
    subprocess.run(command, check=True)
    print("Release firmware created: {}".format(output_path))
    print("OTA application image created: {}".format(ota_path))


env.AddPostAction("$BUILD_DIR/firmware.bin", package_firmware)
