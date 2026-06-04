# BUP Packaging Guide for BoredOS

This guide explains how to package applications and libraries into BoredOS Package (`.bup`) files for distribution via the BoredOS Package Manager (`bpm`).

---

## What is a `.bup` File?

A `.bup` file is a **TAR archive compressed with LZ4** (`.tar.lz4`) containing a metadata manifest, payload directories, and optional installation hook scripts.

---

## Package Directory Structure

To build a `.bup` file, you must first organize your files inside a package root directory (e.g., `pkg_staging/`). The structure inside this directory must follow this layout:

```text
pkg_staging/
├── MANIFEST.toml         # [Mandatory] Package metadata and install paths
├── bin/                  # [Optional] Executable binaries (e.g. .elf files)
├── config/               # [Optional] Configuration files
├── assets/               # [Optional] Shared assets (images, themes, data, etc.)
└── scripts/              # [Optional] Hook scripts executed during install/remove
    ├── install.bsh       # [Optional] Post-install script
    └── remove.bsh        # [Optional] Pre-remove / post-update script
```

> [!WARNING]
> Only include directories and files that your package actually uses. Do not leave empty directories.

---

## 1. The Manifest File (`MANIFEST.toml`)

The `MANIFEST.toml` file must reside at the root of the package. It defines the package name, version, and the target directories where each payload folder should be copied.

### Format and Constraints
* **Syntax**: Standard TOML format.
* **Strings**: Values **must** be enclosed in double quotes (`"..."`). Single quotes or unquoted values are not supported by the parser.
* **Sections**: The `[install]` section header must be present exactly as `[install]` for installation paths.

### Structure Example
```toml
name = "my-app"
version = "1.0.0"

[install]
bin = "/usr/bin"
config = "/Library/conf"
assets = "/usr/share/my-app"
```

### Destination Resolution
When `bpm` installs a package, it maps the contents of your staging directories to destinations specified in the `[install]` block of `MANIFEST.toml`:

| Source Folder | Destination Option | Default Destination | Notes |
| :--- | :--- | :--- | :--- |
| `bin/` | `bin` | `/usr/bin` | Target directory for binaries and executables. |
| `config/` | `config` | `/Library/conf` | Target directory for configuration files. |
| `assets/` | `assets` | *No Default* | **Required** if `assets/` folder is present. If `assets` is omitted from `[install]`, the `assets/` folder will **not** be installed. |

---

## 2. Directory Mappings

* **`bin/`**: Any files in `bin/` will be copied into the destination path defined by `bin` in `MANIFEST.toml`. Files beginning with `._` (macOS metadata files) are automatically ignored.
* **`config/`**: Any files in `config/` will be copied into the destination path defined by `config` in `MANIFEST.toml`.
* **`assets/`**: Any files in `assets/` will be copied into the destination path defined by `assets` in `MANIFEST.toml`. **Important**: Ensure `assets` is explicitly set in `MANIFEST.toml`.

---

## 3. Hook Scripts (`scripts/`)

`bpm` supports optional installation lifecycle hooks. These hooks must be written in Bored Shell script format (`.bsh`) and placed inside the `scripts/` directory:

* **`install.bsh`**: Executed via `/bin/bsh` **immediately after** files are copied to the system. Used for setting up permissions, initial data, or starting services.
* **`remove.bsh`**: Executed via `/bin/bsh` **immediately before** package files are deleted. Used to stop services, clean up logs, or remove custom directories.

### Script Execution During Upgrades
When upgrading a package (via `bpm upgrade`), `bpm` will:
1. Locate the currently installed package's `remove.bsh` script (cached in `/var/lib/bpm/packages/<pkgname>/remove.bsh`).
2. Execute it.
3. Perform the package removal (`cmd_remove`).
4. Install the new package version (`cmd_install`), which copies new files and executes the new `install.bsh` script.

---

## 4. Creating the `.bup` Archive

Once your staging directory is ready, compress it from the staging directory using `tar` with LZ4 compression.

### CLI Command Example

If your files are located in `build/package/`:
```bash
# Define files/directories to include. Ensure they exist.
tar --lz4 -C build/package -cf build/my-app-1.0.0.bup MANIFEST.toml bin config assets scripts
```

Ensure that the archive lists `MANIFEST.toml` and other directories at its top-level root, rather than inside a nested subdirectory.

---

## 5. Integrating with Repository Index (`index.toml`)

To publish your `.bup` package to a repository, it must be registered in the repository's `index.toml`.

A repository index entry contains the package metadata, binary location, and SHA256 checksum for verification:

```toml
[package.my-app]
name = "my-app"
version = "1.0.0"
description = "A short description of my-app"
author = "Your Name"
license = "XXX"
url = "https://your-domain.com/packages/my-app-1.0.0.bup"
sha256 = "64_character_hexadecimal_sha256_checksum"
```

When a user runs `bpm install my-app`, `bpm`:
1. Fetches the repository index.
2. Resolves `my-app` to the entry.
3. Downloads the `.bup` archive from the `url`.
4. Computes the SHA256 of the downloaded file and verifies it against the `sha256` value before extraction.

To publish your `.bup` to the bur (Bored User Repository), open up a PR to: https://github.com/boredos/bur