import argparse
import os
import platform
import stat
import urllib.request

from retry import retry

BUILDIFIER_VERSION = "v6.4.0"
RELEASE_URL = (
    f"https://mdb-build-public.s3.amazonaws.com/bazel-buildifier-binaries/{BUILDIFIER_VERSION}/"
)


@retry(tries=3, delay=5)
def _download_with_retry(*args, **kwargs):
    return urllib.request.urlretrieve(*args, **kwargs)


def determine_platform():
    syst = platform.system()
    pltf = None
    if syst == "Darwin":
        pltf = "darwin"
    elif syst == "Windows":
        pltf = "windows"
    elif syst == "Linux":
        pltf = "linux"
    else:
        raise RuntimeError("Platform cannot be inferred.")
    return pltf


def determine_architecture():
    arch = None
    machine = platform.machine()
    if machine in ("AMD64", "x86_64"):
        arch = "amd64"
    elif machine in ("arm", "arm64", "aarch64"):
        arch = "arm64"
    else:
        raise RuntimeError(f"Detected architecture is not supported: {machine}")

    return arch


def download(download_location: str = "./"):
    operating_system = determine_platform()
    architechture = determine_architecture()
    if operating_system == "windows" and architechture == "arm64":
        raise RuntimeError("There are no published arm windows releases for buildifier.")

    extension = ".exe" if operating_system == "windows" else ""
    binary_name = f"buildifier-{operating_system}-{architechture}{extension}"
    url = f"{RELEASE_URL}{binary_name}"

    file_location = os.path.join(download_location, f"buildifier{extension}")
    _download_with_retry(url, file_location)
    print(f"Downloaded buildifier from {url} to {file_location}")
    os.chmod(file_location, stat.S_IRUSR | stat.S_IWUSR | stat.S_IXUSR)
    print(f"Set user executable permissions on {file_location}")


def main():
    parser = argparse.ArgumentParser(
        prog="DownloadBuildifier",
        description="This downloads buildifier, it is intended for use in evergreen."
        "This is our temperary solution to get bazel linting while we determine a "
        "long-term solution for getting buildifier on evergreen/development hosts.",
    )

    parser.add_argument(
        "--download-location",
        "-f",
        help="Name of directory to download the buildifier binary to.",
        default="./",
    )

    args = parser.parse_known_args()

    download(download_location=args[0].download_location)


if __name__ == "__main__":
    main()
